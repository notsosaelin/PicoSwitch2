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

    @Test fun `unexpected mutation reply cannot become false success`() = runTest {
        val transport = ScriptedTransport(validImage(), unexpectedBegin = true)
        val error = runCatching { AdapterRepository(transport).uploadAmiibo(validImage()) }.exceptionOrNull()
        assertTrue(error is ManagementException)
        assertTrue(transport.commands.contains("amiibo cancel"))
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
        private val reportedSize: Int = data.size,
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
                    """{"loaded":true,"dirty":$dirty,"presented":false,"v3loaded":false,"persisted":true,"persistPending":false,"size":$reportedSize,"signature":false,"hasSave2":false,"usingSave2":false,"generation":$generation,"payloadCrc":"${AmiiboFiles.crc32(data)}","uid":"04000000000000","figureId":"0100000000000000","upload":{"active":false,"received":0,"size":0}}"""
                }
                command.startsWith("amiibo read ") -> {
                    val parts = command.split(' ')
                    val offset = parts[2].toInt(); val count = parts[3].toInt()
                    """{"offset":$offset,"data":"${ManagementProtocol.hex(data.copyOfRange(offset, offset + count))}"}"""
                }
                command.startsWith("amiibo begin ") && unexpectedBegin -> "{}"
                else -> """{"ok":true}"""
            }
        }
    }

    private class CompatibilityTransport(private val data: ByteArray) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        override suspend fun scanAndConnect() = Unit
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
}
