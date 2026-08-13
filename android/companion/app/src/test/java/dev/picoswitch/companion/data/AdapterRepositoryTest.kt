package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.protocol.ManagementException
import dev.picoswitch.companion.protocol.ManagementProtocol
import dev.picoswitch.companion.protocol.ManagementTransport
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

    @Test fun `known adapter reconnect uses direct address without scanning`() = runTest {
        val transport = CompatibilityTransport(validImage())
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(1, transport.knownConnects)
        assertEquals(0, transport.scans)
    }

    @Test fun `failed known adapter reconnect falls back to bounded discovery`() = runTest {
        val transport = CompatibilityTransport(validImage(), failKnown = true)
        AdapterRepository(transport).connectKnown("88:A2:9E:D1:77:78")
        assertEquals(1, transport.knownConnects)
        assertEquals(1, transport.scans)
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
                    """{"offset":$offset,"data":"${ManagementProtocol.hex(data.copyOfRange(offset, offset + count))}"}"""
                }
                command.startsWith("amiibo begin ") && unexpectedBegin -> "{}"
                command == "reenumerate" && !unexpectedReenumerate -> """{"ok":true,"reenumerating":true}"""
                else -> """{"ok":true}"""
            }
        }
    }

    private class CompatibilityTransport(private val data: ByteArray, private val failKnown: Boolean = false) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        var knownConnects = 0
        var scans = 0
        override suspend fun scanAndConnect() { scans++ }
        override suspend fun connectKnown(address: String) {
            knownConnects++
            if (failKnown) throw ManagementException("saved adapter unavailable")
        }
        override suspend fun disconnect() = Unit
        override suspend fun transact(command: String, timeoutMillis: Long): String = when (command) {
            "info" -> """{"id":"picoswitch","product":"PicoSwitch Config","version":"2.0"}"""
            "get" -> """{"body_color":[1,2,3],"joycon2_left_accent":[4,5,6],"joycon2_right_accent":[7,8,9]}"""
            "device" -> """{"name":"Controller","vid":1,"pid":2,"batteryValid":0,"battery":0,"charging":0}"""
            "amiibo status" -> """{"loaded":false,"dirty":false,"presented":false,"v3loaded":false,"persisted":false,"persistPending":false,"size":0,"signature":false,"hasSave2":false,"usingSave2":false,"generation":0,"payloadCrc":"00000000","uid":"","figureId":"","upload":{"active":false,"received":0,"size":0}}"""
            else -> """{"error":"unknown command"}"""
        }
    }

    private fun validImage() = ByteArray(540).apply {
        this[0] = 4; this[3] = (0x88 xor 4).toByte(); this[8] = 0; this[0x54] = 1
    }

    private fun validV3Image() = ByteArray(2048).apply {
        this[0] = 4; this[7] = 0; this[8] = 0x44; this[0x54] = 1
    }
}
