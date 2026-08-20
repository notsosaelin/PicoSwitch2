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
        assertEquals(ManagementProtocol.MAX_REPLY_PAYLOAD_BYTES, limits["bleReplyPayloadBytes"]!!.jsonPrimitive.int)
        assertEquals(ManagementProtocol.AMIIBO_CHUNK_BYTES, limits["amiiboChunkBytes"]!!.jsonPrimitive.int)
        assertEquals(ManagementProtocol.BONDS_PROTOCOL_VERSION, limits["bondEnvelopeVersion"]!!.jsonPrimitive.int)

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
        val joined = ManagementProtocol.chunks("amiibo status", 5).flatMap(ByteArray::asIterable).toByteArray()
        assertArrayEquals("amiibo status\n".encodeToByteArray(), joined)
    }

    @Test fun `wire payload limit rejects 512 bytes`() {
        ManagementProtocol.requireReplyWithinLimit(511)
        assertThrows(ManagementReplyTooLargeException::class.java) {
            ManagementProtocol.requireReplyWithinLimit(512)
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

    @Test fun `Amiibo and wake fixtures decode exact semantics`() {
        val amiiboVector = vector("amiiboStatus")
        val amiibo = ManagementProtocol.amiibo(amiiboVector.command, amiiboVector.reply)
        assertTrue(amiibo.v3Loaded)
        assertEquals(42, amiibo.generation)
        val wakeVector = vector("wakeStatus")
        assertEquals(WakeResult.Advertised, ManagementProtocol.wakeStatus(wakeVector.command, wakeVector.reply).result)
    }

    @Test fun `queued save does not claim completed durability`() {
        val vector = vector("saveQueued")
        val acknowledgement = ManagementProtocol.acknowledgement(vector.command, vector.reply)
        assertTrue(acknowledgement.queued)
        assertFalse(acknowledgement.reenumerating)
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
