package dev.picoswitch.management

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ManagementCorrectiveCloseoutTest {
    @Test fun `unknown optional firmware families are unsupported without failing refresh`() = runTest {
        val channel = refreshChannel(
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
        )

        val refresh = ManagementClient(channel).refreshAll()
        assertEquals(CapabilityState.Unsupported, refresh.snapshot.capabilities.personality)
        assertEquals(CapabilityState.Unsupported, refresh.snapshot.capabilities.amiibo)
        assertEquals(CapabilityState.Unsupported, refresh.snapshot.capabilities.bonds)
        assertEquals(CapabilityState.Unsupported, refresh.snapshot.capabilities.activeInput)
        assertEquals(CapabilityState.Unsupported, refresh.snapshot.capabilities.kbm)
        channel.assertDrained()
    }

    @Test fun `explicit firmware unavailable is an unsupported optional capability`() = runTest {
        val channel = refreshChannel(
            optionalUnsupported("unavailable"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
        )
        val refresh = ManagementClient(channel).refreshAll()
        assertEquals(CapabilityState.Unsupported, refresh.snapshot.capabilities.personality)
    }

    @Test fun `transport timeout channel loss and invalid session propagate`() = runTest {
        listOf(
            ManagementException("timed out"),
            ManagementException("channel failed"),
            ManagementException("adapter disconnected"),
            ManagementException("session invalidated"),
        ).forEach { expected ->
            val channel = ScriptedCorrectiveChannel(
                *requiredRefresh(),
                "personality" to expected,
            )
            val actual = runCatching { ManagementClient(channel).refreshAll() }.exceptionOrNull()
            assertTrue("Expected propagation for ${expected.message}, got $actual", actual === expected)
        }
    }

    @Test fun `malformed and oversized optional replies propagate`() = runTest {
        val malformed = ScriptedCorrectiveChannel(*requiredRefresh(), "personality" to "{broken")
        assertTrue(runCatching { ManagementClient(malformed).refreshAll() }.exceptionOrNull() is ManagementProtocolException)

        val tooLarge = ManagementReplyTooLargeException("reply too large")
        val oversized = ScriptedCorrectiveChannel(*requiredRefresh(), "personality" to tooLarge)
        assertTrue(runCatching { ManagementClient(oversized).refreshAll() }.exceptionOrNull() === tooLarge)
    }

    @Test fun `optional bond pagination failure propagates`() = runTest {
        val channel = ScriptedCorrectiveChannel(
            *requiredRefresh(),
            "personality" to optionalUnsupported("unknown command"),
            "amiibo status" to optionalUnsupported("unknown command"),
            "mgmt status" to optionalUnsupported("unknown command"),
            "bonds list" to """{"v":2,"total":2,"bonds":[{"i":0,"addr":"00"}],"next":1}""",
            "bonds list v2 1" to """{"v":2,"total":3,"bonds":[{"i":1,"addr":"11"}],"next":null}""",
        )
        assertTrue(
            runCatching { ManagementClient(channel).refreshAll() }.exceptionOrNull() is ManagementPaginationException,
        )
    }

    @Test fun `supported KBM is available and a KBM transaction failure propagates`() = runTest {
        val supported = refreshChannel(
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            kbmStatus(),
            kbmCounters(),
            kbmMouse(),
        )
        val refresh = ManagementClient(supported).refreshAll()
        assertEquals(CapabilityState.Available, refresh.snapshot.capabilities.kbm)
        assertEquals(KbmMode.KeyboardMouse, refresh.kbmStatus?.mode)

        val failure = ManagementException("KBM session failed")
        val failed = refreshChannel(
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            optionalUnsupported("unknown command"),
            failure,
        )
        assertTrue(runCatching { ManagementClient(failed).refreshAll() }.exceptionOrNull() === failure)
    }

    @Test fun `wake workflow retains firmware last attempt timestamp`() = runTest {
        val channel = ScriptedCorrectiveChannel(
            "wake" to """{"ok":true}""",
            "wake status" to """{"result":"advertised","consoleAsleep":true,"identityValid":true,"attempts":2,"lastAttemptMs":1234}""",
        )
        assertEquals(1234L, ManagementClient(channel).wakeConsole().lastAttemptMs)
    }

    @Test fun `save completion polling follows the acknowledged request identity`() = runTest {
        val channel = ScriptedCorrectiveChannel(
            "save" to """{"ok":true,"queued":true,"requested":7}""",
            "save status" to """{"pending":true,"requested":7,"completed":6}""",
            "save status" to """{"pending":false,"requested":7,"completed":7}""",
        )
        val status = ManagementClient(channel).saveAndAwait()
        assertFalse(status.pending)
        assertEquals(7L, status.completed)
        channel.assertDrained()
    }

    @Test fun `later automatic save does not hide completion of the requested save`() = runTest {
        val channel = ScriptedCorrectiveChannel(
            "save status" to """{"pending":true,"requested":8,"completed":7}""",
        )
        val status = ManagementClient(channel).awaitPersistence(
            PersistenceAcknowledgement(PersistenceState.Queued, requestId = 7),
        )
        assertTrue(status.pending)
        assertEquals(8L, status.requested)
    }

    @Test fun `legacy save acknowledgement cannot claim authoritative completion`() = runTest {
        val channel = ScriptedCorrectiveChannel("save" to """{"ok":true,"queued":true}""")
        val error = runCatching { ManagementClient(channel).saveAndAwait() }.exceptionOrNull()
        assertTrue(error is ManagementProtocolException)
    }

    @Test fun `save completion polling has a bounded timeout`() = runTest {
        val error = runCatching {
            ManagementClient(PendingPersistenceChannel()).awaitPersistence(
                PersistenceAcknowledgement(PersistenceState.Queued, requestId = 1),
                timeoutMillis = 250,
            )
        }.exceptionOrNull()
        assertTrue(error is ManagementException)
        assertTrue(error?.cause is kotlinx.coroutines.TimeoutCancellationException)
    }

    private fun refreshChannel(vararg optionalReplies: Any): ScriptedCorrectiveChannel {
        val commands = listOf(
            "personality",
            "amiibo status",
            "mgmt status",
            "bonds list",
            "input sources",
            "kbm status",
            "kbm counters",
            "kbm mouse",
        )
        return ScriptedCorrectiveChannel(
            *requiredRefresh(),
            *optionalReplies.mapIndexed { index, reply -> commands[index] to reply }.toTypedArray(),
        )
    }

    private fun requiredRefresh(): Array<Pair<String, Any>> = arrayOf(
        "info" to """{"id":"picoswitch","version":"2.0"}""",
        "get" to """{"body_color":[0,0,0],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}""",
        "device" to "{}",
    )

    private fun optionalUnsupported(message: String) = """{"error":"$message"}"""

    private fun kbmStatus() =
        """{"mode":"kbmouse","override":"auto","profile":"kbm","keyboard":true,"mouse":true,"nativeMouse":false,"keyboardConn":1,"mouseConn":2}"""

    private fun kbmCounters() =
        """{"keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,"rejectedNotOwner":0,"rejectedNoPeerKey":0,"rejectedUnclassified":0,"rejectedNoRole":0,"undecodedReports":0,"rollover":0,"roleLosses":0,"mapGeneration":0,"neutralizations":0,"publishes":0,"recenters":0}"""

    private fun kbmMouse() =
        """{"sensitivityX":512,"sensitivityY":512,"recenterMs":12,"invertX":false,"invertY":false,"antiDeadzone":0,"sensitivityMin":16,"sensitivityMax":8192,"recenterMinMs":1,"recenterMaxMs":100,"antiDeadzoneMax":50}"""
}

private class ScriptedCorrectiveChannel(vararg exchanges: Pair<String, Any>) : ManagementChannel {
    private val pending = ArrayDeque(exchanges.toList())

    override suspend fun transact(command: String, timeoutMillis: Long): String {
        val next = pending.removeFirstOrNull() ?: error("Unexpected command: $command")
        assertEquals(next.first, command)
        val result = next.second
        if (result is Throwable) throw result
        return result as String
    }

    fun assertDrained() = assertTrue("Unconsumed exchanges: $pending", pending.isEmpty())
}

private class PendingPersistenceChannel : ManagementChannel {
    override suspend fun transact(command: String, timeoutMillis: Long): String {
        assertEquals("save status", command)
        return """{"pending":true,"requested":1,"completed":0}"""
    }
}
