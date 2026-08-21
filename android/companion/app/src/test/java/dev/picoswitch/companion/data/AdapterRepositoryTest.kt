package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.model.AdapterSnapshot
import dev.picoswitch.companion.model.RgbColor
import dev.picoswitch.companion.protocol.ManagementException
import dev.picoswitch.companion.protocol.ManagementConnectionContext
import dev.picoswitch.companion.protocol.ManagementProtocol
import dev.picoswitch.companion.protocol.ManagementTransport
import dev.picoswitch.companion.transport.GattFailureStage
import dev.picoswitch.companion.transport.GattTransportException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test

class AdapterRepositoryTest {
    @Test fun `upload is serialized in 32 byte chunks then committed and persisted`() = runTest {
        val data = validImage()
        val transport = ScriptedTransport(data)
        AdapterRepository(transport).uploadAmiibo(data)
        assertTrue(transport.commands.first().startsWith("amiibo status"))
        assertTrue(transport.commands.any { it.startsWith("amiibo begin 540 ") })
        assertEquals(17, transport.commands.count { it.startsWith("amiibo chunk ") })
        assertTrue(transport.commands.contains("amiibo commit"))
        assertTrue(transport.commands.contains("amiibo persist"))
    }

    @Test fun `dirty adapter refuses replacement before begin`() = runTest {
        val data = validImage()
        val transport = ScriptedTransport(data, dirty = true)
        val error = runCatching { AdapterRepository(transport).uploadAmiibo(data) }.exceptionOrNull()
        assertTrue(error?.message?.contains("Sync") == true)
        assertFalse(transport.commands.any { it.startsWith("amiibo begin") })
    }

    @Test fun `download does not acknowledge dirty data until caller confirms durable storage`() = runTest {
        val data = validImage()
        val transport = ScriptedTransport(data)
        val repository = AdapterRepository(transport)
        val download = repository.downloadAmiibo()
        assertArrayEquals(data, download.bytes)
        assertFalse(transport.commands.contains("amiibo downloaded"))
        repository.acknowledgeDownloadedAmiibo(download)
        assertTrue(transport.commands.contains("amiibo downloaded"))
        assertTrue(transport.commands.contains("amiibo persist"))
    }

    @Test fun `ordinary firmware zero CRC sentinel still syncs with generation protection`() = runTest {
        val data = validImage()
        val transport = ScriptedTransport(data, reportedPayloadCrc = "00000000")
        val repository = AdapterRepository(transport)
        val download = repository.downloadAmiibo()
        assertArrayEquals(data, download.bytes)
        assertNull(download.payloadCrc)
        repository.acknowledgeDownloadedAmiibo(download)
        assertTrue(transport.commands.contains("amiibo downloaded"))
    }

    @Test fun `nonzero whole image CRC mismatch remains a hard failure`() = runTest {
        val transport = ScriptedTransport(validImage(), reportedPayloadCrc = "DEADBEEF")
        val error = runCatching { AdapterRepository(transport).downloadAmiibo() }.exceptionOrNull()
        assertTrue(error?.message?.contains("CRC verification") == true)
        assertFalse(transport.commands.contains("amiibo downloaded"))
    }

    @Test fun `v3 whole image CRC is verified and retained through acknowledgement`() = runTest {
        val data = validV3Image()
        val transport = ScriptedTransport(data, v3Loaded = true)
        val repository = AdapterRepository(transport)
        val download = repository.downloadAmiibo()
        assertEquals(AmiiboFiles.crc32(data), download.payloadCrc)
        repository.acknowledgeDownloadedAmiibo(download)
        assertTrue(transport.commands.contains("amiibo downloaded"))
    }

    @Test fun `v3 zero CRC is not accepted as the ordinary sentinel`() = runTest {
        val transport = ScriptedTransport(validV3Image(), v3Loaded = true, reportedPayloadCrc = "00000000")
        val error = runCatching { AdapterRepository(transport).downloadAmiibo() }.exceptionOrNull()
        assertTrue(error?.message?.contains("CRC verification") == true)
        assertFalse(transport.commands.contains("amiibo downloaded"))
    }

    @Test fun `generation race is not acknowledged`() = runTest {
        val transport = ScriptedTransport(validImage(), generationChanges = true)
        val repository = AdapterRepository(transport)
        val download = repository.downloadAmiibo()
        val error = runCatching { repository.acknowledgeDownloadedAmiibo(download) }.exceptionOrNull()
        assertTrue(error?.message?.contains("changed during Sync") == true)
        assertFalse(transport.commands.contains("amiibo downloaded"))
    }

    @Test fun `unsupported optional commands do not erase core snapshot`() = runTest {
        val repository = AdapterRepository(CompatibilityTransport(validImage()))
        repository.refreshAll()
        val snapshot = repository.snapshot.value
        assertEquals("2.0", snapshot.firmware.version)
        assertEquals(CapabilityState.Unsupported, snapshot.capabilities.personality)
        assertEquals(CapabilityState.Unsupported, snapshot.capabilities.managementGate)
        assertEquals(CapabilityState.Unsupported, snapshot.capabilities.bonds)
        assertEquals(CapabilityState.Available, snapshot.capabilities.amiibo)
    }

    @Test fun `versioned bond pages are aggregated and marked complete`() = runTest {
        val transport = BondTransport()
        val repository = AdapterRepository(transport)
        val bonds = repository.listBonds()
        assertEquals(listOf(0, 4, 7), bonds.map { it.index })
        assertTrue(repository.snapshot.value.bondsComplete == true)
        assertEquals(3, repository.snapshot.value.bondsTotal)
        assertEquals(CapabilityState.Available, repository.snapshot.value.capabilities.bonds)
        assertEquals(listOf("bonds list", "bonds list v2 4"), transport.commands)
    }

    @Test fun `legacy unversioned bond list is retained but never claimed complete`() = runTest {
        val transport = BondTransport(legacy = true)
        val repository = AdapterRepository(transport)
        assertEquals(listOf(0), repository.listBonds().map { it.index })
        assertFalse(repository.snapshot.value.bondsComplete == true)
        assertEquals(CapabilityState.Unknown, repository.snapshot.value.capabilities.bonds)
        val error = runCatching { repository.removeBond(0) }.exceptionOrNull()
        assertTrue(error?.message?.contains("completeness", true) == true)
        assertFalse(transport.commands.contains("bonds remove 0"))
    }

    // Removing a bond can revoke THIS phone's own authorization, and Android does
    // not expose our address to compare against the entry. The caller must learn
    // that the session died so the UI cannot keep showing "Connected" after the
    // relationship permitting it was deleted.
    @Test fun `bond removal reports a surviving session`() = runTest {
        val transport = BondTransport()
        val repository = AdapterRepository(transport)
        repository.listBonds()
        assertTrue(repository.removeBond(0))
        assertTrue(transport.commands.contains("bonds remove 0"))
    }

    @Test fun `bond removal that ends this phone's session reports it`() = runTest {
        val transport = BondTransport()
        val repository = AdapterRepository(transport)
        repository.listBonds()
        // The link stops answering once our own authorization is gone.
        transport.failBonds = true
        assertFalse(repository.removeBond(0))
        // The stale authoritative list must not survive an ambiguous mutation.
        assertFalse(repository.snapshot.value.bondsComplete == true)
    }

    @Test fun `response-too-large legacy reply switches to versioned pagination`() = runTest {
        val transport = BondTransport(oversizedLegacy = true)
        val repository = AdapterRepository(transport)
        assertEquals(3, repository.listBonds().size)
        assertTrue(repository.snapshot.value.bondsComplete == true)
        assertEquals("bonds list", transport.commands.first())
        assertTrue(transport.commands.contains("bonds list v2"))
    }

    @Test fun `failed bond refresh clears prior authoritative state`() = runTest {
        val transport = BondTransport()
        val repository = AdapterRepository(transport)
        repository.listBonds()
        transport.failBonds = true
        assertNotNull(runCatching { repository.listBonds() }.exceptionOrNull())
        assertTrue(repository.snapshot.value.bonds.isEmpty())
        assertNull(repository.snapshot.value.bondsComplete)
        assertEquals(CapabilityState.Unknown, repository.snapshot.value.capabilities.bonds)
    }

    @Test fun `known adapter reconnect uses direct address without scanning`() = runTest {
        val transport = CompatibilityTransport(validImage())
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(1, transport.knownConnects)
        assertEquals(0, transport.scans)
        assertEquals(listOf("info"), transport.commands)
    }

    @Test fun `failed known adapter reconnect falls back to bounded discovery`() = runTest {
        val transport = CompatibilityTransport(validImage(), failKnown = true)
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(1, transport.knownConnects)
        assertEquals(1, transport.scans)
    }

    @Test fun `wrong device at saved address falls back to service discovery`() = runTest {
        val transport = CompatibilityTransport(validImage(), wrongKnownIdentity = true)
        val repository = AdapterRepository(transport)
        repository.connectKnown("88:A2:9E:D1:77:78")
        assertEquals(1, transport.knownConnects)
        assertEquals(1, transport.scans)
        assertEquals("picoswitch", repository.snapshot.value.firmware.id)
    }

    @Test fun `GATT 133 gets one clean direct retry before discovery fallback`() = runTest {
        val transport = RecoveryTransport(failures = 1, status = 133)
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(2, transport.knownConnects)
        assertEquals(0, transport.scans)
        assertEquals(1, transport.disconnects)
    }

    @Test fun `second GATT 133 retires direct attempts and uses one fallback`() = runTest {
        val transport = RecoveryTransport(failures = 2, status = 133)
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(2, transport.knownConnects)
        assertEquals(1, transport.scans)
        assertEquals(2, transport.disconnects)
        assertEquals(listOf(0, 1, 1), transport.contexts.map { it.retry })
        assertEquals(listOf(false, true, true), transport.contexts.map { it.priorGattRetired })
    }

    @Test fun `repeated GATT failure terminates after retry and one fallback`() = runTest {
        val transport = RecoveryTransport(failures = 3, status = 133, failScan = true)
        val error = runCatching {
            AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        }.exceptionOrNull()
        assertNotNull(error)
        assertEquals(2, transport.knownConnects)
        assertEquals(1, transport.scans)
    }

    @Test fun `non retryable direct failure skips retry but retains bounded fallback`() = runTest {
        val transport = RecoveryTransport(failures = 1, status = 5)
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(1, transport.knownConnects)
        assertEquals(1, transport.scans)
    }

    @Test fun `disconnect clears all adapter derived state`() = runTest {
        val transport = CompatibilityTransport(validImage())
        val repository = AdapterRepository(transport)
        repository.refreshAll()
        assertEquals("2.0", repository.snapshot.value.firmware.version)
        repository.disconnect()
        assertEquals(AdapterSnapshot(), repository.snapshot.value)
    }

    @Test fun `unexpected mutation reply cannot become false success`() = runTest {
        val transport = ScriptedTransport(validImage(), unexpectedBegin = true)
        val error = runCatching { AdapterRepository(transport).uploadAmiibo(validImage()) }.exceptionOrNull()
        assertTrue(error is ManagementException)
        assertTrue(transport.commands.contains("amiibo cancel"))
    }

    @Test fun `USB identity apply requires explicit reenumeration acknowledgement`() = runTest {
        val transport = ScriptedTransport(validImage())
        AdapterRepository(transport).reenumerateUsb()
        assertEquals("reenumerate", transport.commands.last())
    }

    @Test fun `USB identity apply rejects a generic acknowledgement`() = runTest {
        val transport = ScriptedTransport(validImage(), unexpectedReenumerate = true)
        val error = runCatching { AdapterRepository(transport).reenumerateUsb() }.exceptionOrNull()
        assertTrue(error is ManagementException)
    }

    @Test fun `color mutation reads back then awaits persistence before USB apply`() = runTest {
        val transport = ColorTransport(modernPersistence = true)
        val repository = AdapterRepository(transport)
        assertTrue(repository.setColor(ColorTarget.Body, RgbColor(1, 2, 3)))
        repository.reenumerateUsb()
        assertEquals(listOf("body 1 2 3", "get", "save", "save status", "reenumerate"), transport.commands)
    }

    @Test fun `legacy persistence acknowledgement remains compatible with color apply`() = runTest {
        val transport = ColorTransport(modernPersistence = false)
        val repository = AdapterRepository(transport)
        assertFalse(repository.setColor(ColorTarget.LeftAccent, RgbColor(4, 5, 6)))
        repository.reenumerateUsb()
        assertEquals(listOf("jcl 4 5 6", "get", "save", "reenumerate"), transport.commands)
    }

    @Test fun `active input selection is acknowledged then refreshed`() = runTest {
        val transport = ScriptedTransport(validImage())
        val repository = AdapterRepository(transport)
        repository.setActiveInput(29)
        assertEquals(listOf("input active 29", "input sources"), transport.commands)
        assertEquals(29L, repository.snapshot.value.input.activeId)
        assertEquals("PicoSwitch A", repository.snapshot.value.input.activeSource?.name)
        assertEquals(CapabilityState.Available, repository.snapshot.value.capabilities.activeInput)
    }

    @Test fun `active input none uses explicit neutral command`() = runTest {
        val transport = ScriptedTransport(validImage(), activeInput = 0)
        val repository = AdapterRepository(transport)
        repository.setActiveInput(0)
        assertEquals("input active none", transport.commands.first())
        assertEquals(0L, repository.snapshot.value.input.activeId)
    }

    @Test fun `unsupported adapter size is rejected before download allocation`() = runTest {
        val transport = ScriptedTransport(validImage(), reportedSize = Int.MAX_VALUE)
        val error = runCatching { AdapterRepository(transport).downloadAmiibo() }.exceptionOrNull()
        assertTrue(error?.message?.contains("no memory was allocated") == true)
        assertFalse(transport.commands.any { it.startsWith("amiibo read") })
    }

    private class ScriptedTransport(
        private val data: ByteArray,
        private val dirty: Boolean = false,
        private val generationChanges: Boolean = false,
        private val unexpectedBegin: Boolean = false,
        private val unexpectedReenumerate: Boolean = false,
        private val reportedSize: Int = data.size,
        private val v3Loaded: Boolean = false,
        private val reportedPayloadCrc: String = AmiiboFiles.crc32(data),
        private val activeInput: Long = 29,
    ) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        val commands = mutableListOf<String>()
        override suspend fun scanAndConnect() = Unit
        override suspend fun disconnect() = Unit
        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            return when {
                command == "amiibo status" -> {
                    val statusCount = commands.count { it == "amiibo status" }
                    val generation = if (generationChanges && statusCount > 1) 2 else 1
                    """{"loaded":true,"dirty":$dirty,"presented":false,"v3loaded":$v3Loaded,"persisted":true,"persistPending":false,"size":$reportedSize,"signature":false,"hasSave2":false,"usingSave2":false,"generation":$generation,"payloadCrc":"$reportedPayloadCrc","uid":"04000000000000","figureId":"0100000000000000","upload":{"active":false,"received":0,"size":0}}"""
                }
                command.startsWith("amiibo read ") -> {
                    val parts = command.split(' ')
                    val offset = parts[2].toInt(); val count = parts[3].toInt()
                    """{"offset":$offset,"data":"${dev.picoswitch.management.ManagementCommands.hex(data.copyOfRange(offset, offset + count))}"}"""
                }
                command.startsWith("amiibo begin ") && unexpectedBegin -> "{}"
                command == "reenumerate" && !unexpectedReenumerate -> """{"ok":true,"reenumerating":true}"""
                command == "input sources" -> """{"active":$activeInput,"pending":0,"explicit":true,"fresh":false,"transitions":1,"sources":[{"id":29,"conn":1,"transport":2,"generation":9,"name":"PicoSwitch A"}],"more":false}"""
                else -> """{"ok":true}"""
            }
        }
    }

    private class CompatibilityTransport(
        private val data: ByteArray,
        private val failKnown: Boolean = false,
        private val wrongKnownIdentity: Boolean = false,
    ) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        var knownConnects = 0
        var scans = 0
        val commands = mutableListOf<String>()
        override suspend fun scanAndConnect() { scans++ }
        override suspend fun connectKnown(address: String) {
            knownConnects++
            if (failKnown) throw ManagementException("saved adapter unavailable")
        }
        override suspend fun disconnect() = Unit
        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            return when (command) {
                "info" -> if (wrongKnownIdentity && scans == 0) {
                    """{"id":"other-device","product":"Unrelated","version":"1.0"}"""
                } else {
                    """{"id":"picoswitch","product":"PicoSwitch Config","version":"2.0"}"""
                }
                "get" -> """{"body_color":[1,2,3],"joycon2_left_accent":[4,5,6],"joycon2_right_accent":[7,8,9]}"""
                "device" -> """{"name":"Controller","vid":1,"pid":2,"batteryValid":0,"battery":0,"charging":0}"""
                "amiibo status" -> """{"loaded":false,"dirty":false,"presented":false,"v3loaded":false,"persisted":false,"persistPending":false,"size":0,"signature":false,"hasSave2":false,"usingSave2":false,"generation":0,"payloadCrc":"00000000","uid":"","figureId":"","upload":{"active":false,"received":0,"size":0}}"""
                else -> """{"error":"unknown command"}"""
            }
        }
    }

    private class BondTransport(
        private val legacy: Boolean = false,
        private val oversizedLegacy: Boolean = false,
    ) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        val commands = mutableListOf<String>()
        var failBonds = false
        override suspend fun scanAndConnect() = Unit
        override suspend fun disconnect() = Unit
        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            if (failBonds && command.startsWith("bonds list")) {
                return """{"error":"transport unavailable","code":9}"""
            }
            if (command == "bonds list" && oversizedLegacy) {
                return """{"error":"response_too_large","code":413}"""
            }
            if (command == "bonds list" && legacy) {
                return """{"bonds":[{"i":0,"type":1,"addr":"010203040506"}]}"""
            }
            if (command.startsWith("bonds remove")) return """{"ok":true}"""
            return when (command) {
                "bonds list" -> """{"v":2,"total":3,"bonds":[{"i":0,"type":1,"addr":"010203040506"}],"next":4}"""
                "bonds list v2" -> """{"v":2,"total":3,"bonds":[{"i":0,"type":1,"addr":"010203040506"}],"next":4}"""
                "bonds list v2 4" -> """{"v":2,"total":3,"bonds":[{"i":4,"type":0,"addr":"AABBCCDDEEFF"},{"i":7,"type":1,"addr":"102030405060"}],"next":null}"""
                else -> """{"error":"unknown command"}"""
            }
        }
    }

    private class RecoveryTransport(
        private var failures: Int,
        private val status: Int,
        private val failScan: Boolean = false,
    ) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        var knownConnects = 0
        var scans = 0
        val commands = mutableListOf<String>()
        var disconnects = 0
        val contexts = mutableListOf<ManagementConnectionContext>()
        override fun prepareConnection(context: ManagementConnectionContext) { contexts += context }
        override suspend fun scanAndConnect() { scans++; if (failScan) throw connectFailure() }
        override suspend fun scanAndConnect(expectedAddress: String) { scans++; if (failScan) throw connectFailure() }
        override suspend fun connectKnown(address: String) {
            knownConnects++
            if (failures-- > 0) throw connectFailure()
        }
        override suspend fun disconnect() { disconnects++ }
        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            return when (command) {
            "info" -> """{"id":"picoswitch","product":"PicoSwitch Config","version":"2.0"}"""
            "get" -> """{"body_color":[1,2,3],"joycon2_left_accent":[4,5,6],"joycon2_right_accent":[7,8,9]}"""
            "device" -> """{"name":"Controller","vid":1,"pid":2,"batteryValid":0,"battery":0,"charging":0}"""
            "amiibo status" -> """{"loaded":false,"dirty":false,"presented":false,"v3loaded":false,"persisted":false,"persistPending":false,"size":0,"signature":false,"hasSave2":false,"usingSave2":false,"generation":0,"payloadCrc":"00000000","uid":"","figureId":"","upload":{"active":false,"received":0,"size":0}}"""
                else -> """{"error":"unknown command"}"""
            }
        }

        private fun connectFailure() = GattTransportException(
            "Bluetooth connection failed ($status)", status, GattFailureStage.Connect,
        )
    }

    private class ColorTransport(private val modernPersistence: Boolean) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        val commands = mutableListOf<String>()
        override suspend fun scanAndConnect() = Unit
        override suspend fun disconnect() = Unit
        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            return when {
                command.startsWith("body ") || command.startsWith("jcl ") || command.startsWith("jcr ") -> """{"ok":true}"""
                command == "get" -> """{"body_color":[1,2,3],"joycon2_left_accent":[4,5,6],"joycon2_right_accent":[7,8,9]}"""
                command == "save" && modernPersistence -> """{"ok":true,"queued":true,"requested":9}"""
                command == "save" -> """{"ok":true}"""
                command == "save status" -> """{"pending":false,"requested":9,"completed":9}"""
                command == "reenumerate" -> """{"ok":true,"reenumerating":true}"""
                else -> """{"error":"unknown command"}"""
            }
        }
    }

    private fun validImage() = ByteArray(540).apply {
        this[0] = 4; this[3] = (0x88 xor 4).toByte(); this[8] = 0; this[0x54] = 1
    }

    private fun validV3Image() = ByteArray(2048).apply {
        this[0] = 4; this[7] = 0; this[8] = 0x44; this[0x54] = 1
    }
}
