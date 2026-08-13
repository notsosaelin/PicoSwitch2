package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.ConnectionState
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

    private class ScriptedTransport(private val data: ByteArray, private val dirty: Boolean = false) : ManagementTransport {
        override val connection = MutableStateFlow(ConnectionState())
        val commands = mutableListOf<String>()
        override suspend fun scanAndConnect() = Unit
        override suspend fun disconnect() = Unit
        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            return when {
                command == "amiibo status" -> """{"loaded":true,"dirty":$dirty,"presented":false,"v3loaded":false,"persisted":true,"persistPending":false,"size":540,"signature":false,"hasSave2":false,"usingSave2":false,"generation":1,"payloadCrc":"${AmiiboFiles.crc32(data)}","uid":"04000000000000","figureId":"0100000000000000","upload":{"active":false,"received":0,"size":0}}"""
                else -> """{"ok":true}"""
            }
        }
    }

    private fun validImage() = ByteArray(540).apply {
        this[0] = 4; this[3] = (0x88 xor 4).toByte(); this[8] = 0; this[0x54] = 1
    }
}
