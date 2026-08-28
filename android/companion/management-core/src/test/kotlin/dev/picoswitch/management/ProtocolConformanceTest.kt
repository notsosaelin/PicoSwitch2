package dev.picoswitch.management

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ProtocolConformanceTest {
    private val fixtureRoot = Json.parseToJsonElement(
        checkNotNull(javaClass.getResource("/protocol-v1.json")).readText(),
    ).jsonObject
    private val fixture = fixtureRoot["vectors"]!!.jsonObject

    @Test fun `fixture limits and BLE constants match the implementation`() {
        val limits = fixtureRoot["limits"]!!.jsonObject
        assertEquals(ManagementProtocol.MAX_COMMAND_BYTES, limits["commandPayloadBytes"]!!.jsonPrimitive.int)
        assertEquals(BleManagementContract.MAX_REPLY_PAYLOAD_BYTES, limits["bleReplyPayloadBytes"]!!.jsonPrimitive.int)
        assertEquals(ManagementProtocol.AMIIBO_CHUNK_BYTES, limits["amiiboChunkBytes"]!!.jsonPrimitive.int)
        assertEquals(ManagementProtocol.BONDS_PROTOCOL_VERSION, limits["bondEnvelopeVersion"]!!.jsonPrimitive.int)
        assertEquals(ManagementProtocol.PEERS_PROTOCOL_VERSION, limits["peerEnvelopeVersion"]!!.jsonPrimitive.int)

        val ble = fixtureRoot["ble"]!!.jsonObject
        assertEquals(BleManagementContract.SERVICE_UUID, ble["serviceUuid"]!!.jsonPrimitive.content)
        assertEquals(BleManagementContract.RX_UUID, ble["rxUuid"]!!.jsonPrimitive.content)
        assertEquals(BleManagementContract.TX_UUID, ble["txUuid"]!!.jsonPrimitive.content)
        assertEquals(
            BleManagementContract.ATT_PAYLOAD_WITH_DEFAULT_MTU,
            ble["minimumMtuPayloadBytes"]!!.jsonPrimitive.int,
        )
    }

    @Test fun `language-neutral builder vectors match command authority`() {
        val generated = mapOf(
            "inputNone" to ManagementCommands.inputActive(0),
            "setPersonality" to ManagementCommands.personality(Personality.JoyConRight),
            "bondPage" to ManagementCommands.bondsPage(3),
            "peerPage" to ManagementCommands.peersPage(2),
            "peerForget" to ManagementCommands.peersForget("p_5E6F7A8B"),
            "kbmDefault" to ManagementCommands.kbmBind(
                KbmProfile.KeyboardMouse,
                KbmSource(KbmSourceKind.Key, 0x1A),
                null,
            ),
            "kbmNone" to ManagementCommands.kbmBind(
                KbmProfile.KeyboardMouse,
                KbmSource(KbmSourceKind.Key, 0x1A),
                KbmDestination.None,
            ),
            "mouseSensitivityX" to ManagementCommands.kbmMouse(KbmMouseField.SensitivityX, 512),
            "bodyColor" to ManagementCommands.color(ColorTarget.Body, RgbColor(1, 2, 3)),
            "saveStatus" to ManagementCommands.SAVE_STATUS,
            "amiiboRead" to ManagementCommands.amiiboRead(32, 16),
        )
        fixtureRoot["builders"]!!.jsonArray.forEach { element ->
            val value = element.jsonObject
            assertEquals(
                value["command"]!!.jsonPrimitive.content,
                generated[value["operation"]!!.jsonPrimitive.content],
            )
        }
    }

    @Test fun `language-neutral paging vectors form complete sequences`() {
        val paging = fixtureRoot["pagination"]!!.jsonObject
        val bonds = paging["bonds"]!!.jsonArray.map { value ->
            val item = value.jsonObject
            ManagementProtocol.bondsPage(
                item["command"]!!.jsonPrimitive.content,
                item["reply"]!!.toString(),
            )
        }
        assertEquals(listOf(3, null), bonds.map { it.next })
        assertEquals(2, bonds.sumOf { it.entries.size })

        val peers = paging["peers"]!!.jsonArray.map { value ->
            val item = value.jsonObject
            ManagementProtocol.peersPage(
                item["command"]!!.jsonPrimitive.content,
                item["reply"]!!.toString(),
            )
        }
        assertEquals(listOf(2, null), peers.map { it.next })
        assertEquals(3, peers.sumOf { it.entries.size })
        assertEquals(listOf(3, 3), peers.map { it.total })
        // The vectors carry the shape that matters: the management phone holds
        // records on both transports, a controller holds one, and a stored peer
        // the adapter cannot identify is reported as unknown rather than guessed.
        val all = peers.flatMap { it.entries }
        assertEquals(PeerRole.ManagementCompanion, all[0].role)
        assertTrue(all[0].multiTransport)
        assertEquals(PeerRole.PhysicalController, all[1].role)
        assertEquals(PeerRole.Unknown, all[2].role)

        val kbm = paging["kbm"]!!.jsonArray.map { value ->
            val item = value.jsonObject
            ManagementProtocol.kbmMapPage(
                item["command"]!!.jsonPrimitive.content,
                item["reply"]!!.toString(),
            )
        }
        assertEquals(listOf(true, false), kbm.map { it.more })
        assertEquals(2, kbm.sumOf { it.bindings.size })
    }

    @Test fun `logical framing is newline terminated and byte bounded`() {
        assertArrayEquals("ping\n".encodeToByteArray(), ManagementProtocol.frame("ping"))
        assertEquals(128, ManagementProtocol.frame("x".repeat(127)).size)
        assertThrows(IllegalArgumentException::class.java) { ManagementProtocol.frame("x".repeat(128)) }
        assertThrows(IllegalArgumentException::class.java) { ManagementProtocol.frame("ping\nget") }
    }

    @Test fun `BLE chunks preserve the exact logical frame`() {
        val joined = BleManagementContract.commandChunks("amiibo status", 5)
            .flatMap(ByteArray::asIterable)
            .toByteArray()
        assertArrayEquals("amiibo status\n".encodeToByteArray(), joined)
    }

    @Test fun `BLE reply assembler handles fragments CRLF and payload limit`() {
        val assembler = BleReplyAssembler()
        assertEquals(null, assembler.accept("{\"ok\":".encodeToByteArray()))
        assertEquals("{\"ok\":true}", assembler.accept("true}\r\n".encodeToByteArray()))

        val boundary = BleReplyAssembler()
        assertEquals(null, boundary.accept(ByteArray(511) { 'x'.code.toByte() }))
        assertEquals("x".repeat(511), boundary.accept("\n".encodeToByteArray()))

        val oversized = BleReplyAssembler()
        assertThrows(ManagementReplyTooLargeException::class.java) {
            oversized.accept(ByteArray(512) { 'x'.code.toByte() })
        }
    }

    @Test fun `identity configuration controller and personality fixtures decode`() {
        val info = vector("info")
        assertEquals("picoswitch", ManagementProtocol.firmware(info.command, info.reply).id)
        val config = vector("config")
        assertEquals(RgbColor(4, 5, 6), ManagementProtocol.config(config.command, config.reply).leftAccent)
        val device = vector("device")
        assertEquals(75, ManagementProtocol.controller(device.command, device.reply).batteryPercent)
        val personality = vector("personality")
        assertEquals(Personality.Pro2, ManagementProtocol.personality(personality.command, personality.reply).current)
    }

    @Test fun `input source fixture preserves ownership fields`() {
        val vector = vector("inputSources")
        val state = ManagementProtocol.inputSources(vector.command, vector.reply)
        assertEquals(7, state.activeId)
        assertTrue(state.explicit)
        assertEquals(2, state.sources.single().connection)
    }

    @Test fun `KBM fixtures distinguish mode profile none and adapter ranges`() {
        val statusVector = vector("kbmStatus")
        val status = ManagementProtocol.kbmStatus(statusVector.command, statusVector.reply)
        assertEquals(KbmMode.KeyboardMouse, status.mode)
        assertEquals(KbmMode.Automatic, status.modeOverride)
        val mapVector = vector("kbmMap")
        val page = ManagementProtocol.kbmMapPage(mapVector.command, mapVector.reply)
        assertEquals(KbmDestination.LStickUp, page.bindings.first().destination)
        assertTrue(page.bindings.last().custom)
        val mouseVector = vector("kbmMouse")
        val mouse = ManagementProtocol.kbmMouse(mouseVector.command, mouseVector.reply)
        assertEquals(16, mouse.sensitivityMin)
        assertEquals(50, mouse.antiDeadzoneMax)
        assertEquals("kbm bind kb key:04 none", ManagementCommands.kbmBind(
            KbmProfile.Keyboard,
            KbmSource(KbmSourceKind.Key, 4),
            KbmDestination.None,
        ))
        assertEquals("kbm bind kb key:04 default", ManagementCommands.kbmBind(
            KbmProfile.Keyboard,
            KbmSource(KbmSourceKind.Key, 4),
            null,
        ))
    }

    @Test fun `bond fixture exposes cursor and total`() {
        val vector = vector("bondsPage")
        val page = ManagementProtocol.bondsPage(vector.command, vector.reply)
        assertEquals(2, page.total)
        assertEquals(3, page.next)
        assertEquals("001122334455", page.entries.single().address)
    }

    @Test fun `peer fixture exposes identity, role and cursor without key material`() {
        val vector = vector("peersPage")
        val page = ManagementProtocol.peersPage(vector.command, vector.reply)
        assertEquals(3, page.total)
        assertEquals(2, page.next)
        assertEquals("p_1A2B3C4D", page.entries.first().id)
        assertEquals("DualSense Wireless Controller", page.entries[1].name)
        // Derived identity travels beside the claimed name, not instead of it,
        // so a non-Kotlin client inherits both halves of the naming hierarchy.
        assertEquals("Sony DualSense", page.entries[1].classification)
        assertEquals(1356, page.entries[1].vendorId)
        assertEquals(3302, page.entries[1].productId)
        // A peer the adapter cannot identify carries neither, and carries them
        // as absent rather than as empty strings.
        assertEquals(null, page.entries.first().classification)
        // The gate, stated against the shared vector rather than only in code:
        // the management relationship is on the wire as management.
        assertEquals(PeerRole.ManagementCompanion, page.entries.first().role)
        // No key material crosses the management protocol. Asserted against the
        // shared vector so a non-Kotlin client's fixture carries the same claim.
        listOf("key", "ltk", "irk", "csrk").forEach { forbidden ->
            assertTrue(
                "peer vector must not contain '$forbidden'",
                !vector.reply.contains(forbidden, ignoreCase = true),
            )
        }
    }

    @Test fun `the forget vector carries a verified post-state and no key material`() {
        val vector = vector("peersForget")
        val outcome = ManagementProtocol.peersForget(vector.command, vector.reply)
        assertEquals(PeerForgetResult.Removed, outcome.result)
        // The claim that matters is the adapter's own re-read, not its intent.
        assertFalse(outcome.stillBonded)
        assertTrue(outcome.transports.isEmpty())
        listOf("key", "ltk", "irk", "csrk").forEach { forbidden ->
            assertTrue(
                "forget vector must not contain '$forbidden'",
                !vector.reply.contains(forbidden, ignoreCase = true),
            )
        }
    }

    @Test fun `Amiibo and wake fixtures decode exact semantics`() {
        val amiiboVector = vector("amiiboStatus")
        val amiibo = ManagementProtocol.amiibo(amiiboVector.command, amiiboVector.reply)
        assertTrue(amiibo.v3Loaded)
        assertEquals(42, amiibo.generation)
        val wakeVector = vector("wakeStatus")
        val wake = ManagementProtocol.wakeStatus(wakeVector.command, wakeVector.reply)
        assertEquals(WakeResult.Advertised, wake.result)
        assertEquals(1234, wake.lastAttemptMs)
    }

    @Test fun `queued save does not claim completed durability`() {
        val vector = vector("saveQueued")
        val acknowledgement = ManagementProtocol.acknowledgement(vector.command, vector.reply)
        assertTrue(acknowledgement.queued)
        assertEquals(7L, acknowledgement.requested)
        assertFalse(acknowledgement.reenumerating)

        val statusVector = vector("saveStatus")
        val status = ManagementProtocol.persistenceStatus(statusVector.command, statusVector.reply)
        assertTrue(status.pending)
        assertEquals(7, status.requested)
        assertEquals(6, status.completed)
    }

    @Test fun `firmware errors retain command code and message`() {
        val vector = vector("adapterError")
        val error = assertThrows(AdapterCommandException::class.java) {
            ManagementProtocol.acknowledgement(vector.command, vector.reply)
        }
        assertEquals("amiibo commit", error.command)
        assertEquals(8, error.code)
        assertEquals("dirty", error.adapterMessage)
    }

    @Test fun `unknown fields are tolerated but required fields fail closed`() {
        val withFuture = """{"id":"picoswitch","version":"2.0","future":true}"""
        assertEquals("picoswitch", ManagementProtocol.firmware("info", withFuture).id)
        assertThrows(ManagementProtocolException::class.java) {
            ManagementProtocol.firmware("info", "{}")
        }
    }

    @Test fun `malformed JSON and invalid command identifiers fail closed`() {
        assertThrows(ManagementProtocolException::class.java) {
            ManagementProtocol.controller("device", "not-json")
        }
        assertThrows(IllegalArgumentException::class.java) {
            ManagementCommands.personality(Personality.Config)
        }
        assertThrows(IllegalArgumentException::class.java) {
            ManagementCommands.kbmMap(KbmProfile.Keyboard, 33)
        }
        assertThrows(IllegalArgumentException::class.java) {
            ManagementCommands.bondsPage(-1)
        }
        assertThrows(IllegalArgumentException::class.java) {
            ManagementCommands.amiiboBegin(541, "12345678")
        }
        assertThrows(IllegalArgumentException::class.java) {
            ManagementCommands.amiiboBegin(540, "not-crc")
        }
        assertEquals("amiibo begin 540 A1B2C3D4", ManagementCommands.amiiboBegin(540, "a1b2c3d4"))
    }

    @Test fun `wrong JSON value and container types normalize to protocol errors`() {
        val malformed = listOf<() -> Unit>(
            { ManagementProtocol.firmware("info", """{"id":[],"version":"2.0"}""") },
            { ManagementProtocol.personality("personality", """{"current":"pro2","available":{}}""") },
            { ManagementProtocol.personality("personality", """{"current":"pro2","available":[2]}""") },
            { ManagementProtocol.config("get", """{"body_color":"red","joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}""") },
            { ManagementProtocol.config("get", """{"body_color":["0",0,0],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}""") },
            { ManagementProtocol.isVersionedBondResponse("bonds list", """{"v":"2","bonds":[]}""") },
            { ManagementProtocol.inputSources("input sources", """{"active":"0","pending":0,"explicit":false,"fresh":false,"transitions":0,"sources":[],"more":false}""") },
            { ManagementProtocol.acknowledgement("save", """{"ok":"true"}""") },
            { ManagementProtocol.acknowledgement("save", """{"error":[],"code":413}""") },
            { ManagementProtocol.acknowledgement("save", """{"error":"bad","code":"413"}""") },
        )
        malformed.forEachIndexed { index, operation ->
            assertThrows("malformed case $index", ManagementProtocolException::class.java) { operation() }
        }
    }

    @Test fun `language-neutral error fixtures fail through the expected boundary`() {
        fixtureRoot["errors"]!!.jsonArray.forEach { element ->
            val value = element.jsonObject
            val command = value["command"]!!.jsonPrimitive.content
            val reply = value["replyText"]!!.jsonPrimitive.content
            when (value["case"]!!.jsonPrimitive.content) {
                "malformedJson", "incompleteInfo" ->
                    assertThrows(ManagementProtocolException::class.java) {
                        ManagementProtocol.firmware(command, reply)
                    }
                "responseTooLarge" -> {
                    val error = assertThrows(AdapterCommandException::class.java) {
                        ManagementProtocol.acknowledgement(command, reply)
                    }
                    assertEquals(413, error.code)
                }
                "oddHex" ->
                    assertThrows(ManagementProtocolException::class.java) {
                        ManagementProtocol.readData(command, reply)
                    }
                else -> throw AssertionError("Unknown error fixture case")
            }
        }
    }

    private fun vector(name: String): Vector {
        val value: JsonObject = fixture[name]!!.jsonObject
        return Vector(
            value["command"]!!.jsonPrimitive.content,
            value["reply"]!!.toString(),
        )
    }

    private data class Vector(val command: String, val reply: String)
}
