package dev.picoswitch.management

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ManagementWorkflowTest {
    @Test fun `KBM pagination assembles one complete mapping`() = runTest {
        val channel = ScriptedChannel(
            "kbm map kb 0" to """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":1}""",
            "kbm map kb 1" to """{"profile":"kb","profileId":1,"cursor":1,"total":2,"bindings":[{"src":"key:05","dst":"b","custom":true}],"next":null}""",
        )
        val mapping = ManagementClient(channel).loadKbmMapping(KbmProfile.Keyboard)
        assertEquals(2, mapping.bindings.size)
        assertEquals(1, mapping.customCount)
        channel.assertDrained()
    }

    @Test fun `KBM pagination rejects a changing total`() = runTest {
        val channel = ScriptedChannel(
            "kbm map kb 0" to """{"profile":"kb","profileId":1,"cursor":0,"total":2,"bindings":[{"src":"key:04","dst":"a","custom":false}],"next":1}""",
            "kbm map kb 1" to """{"profile":"kb","profileId":1,"cursor":1,"total":3,"bindings":[{"src":"key:05","dst":"b","custom":false}],"next":null}""",
        )
        val error = runCatching { ManagementClient(channel).loadKbmMapping(KbmProfile.Keyboard) }.exceptionOrNull()
        assertTrue(error is ManagementPaginationException)
    }

    @Test fun `mutation uses authoritative KBM readback`() = runTest {
        val channel = ScriptedChannel(
            "kbm mode keyboard" to """{"ok":true,"mode":"keyboard"}""",
            "kbm status" to kbmStatus("keyboard", "keyboard", "kb"),
            "kbm counters" to kbmCounters(),
        )
        val status = ManagementClient(channel).setKbmMode(KbmMode.Keyboard)
        assertEquals(KbmMode.Keyboard, status.mode)
        channel.assertDrained()
    }

    @Test fun `bond paging follows firmware cursors and checks completeness`() = runTest {
        val channel = ScriptedChannel(
            "bonds list" to """{"error":"response_too_large","code":413}""",
            "bonds list v2" to """{"v":2,"total":2,"bonds":[{"i":0,"type":0,"addr":"001122334455"}],"next":3}""",
            "bonds list v2 3" to """{"v":2,"total":2,"bonds":[{"i":3,"type":1,"addr":"AABBCCDDEEFF"}],"next":null}""",
        )
        val bonds = ManagementClient(channel).listBonds()
        assertTrue(bonds.complete)
        assertEquals(listOf(0, 3), bonds.entries.map { it.index })
        channel.assertDrained()
    }

    @Test fun `legacy bond reply is explicitly incomplete`() = runTest {
        val channel = ScriptedChannel(
            "bonds list" to """{"bonds":[{"index":0,"address":"001122334455"}]}""",
        )
        val bonds = ManagementClient(channel).listBonds()
        assertFalse(bonds.complete)
        assertEquals(null, bonds.total)
    }

    @Test fun `wireless save reports queued not durable`() = runTest {
        val channel = ScriptedChannel("save" to """{"ok":true,"queued":true}""")
        assertEquals(PersistenceState.Queued, ManagementClient(channel).save().state)
    }

    @Test fun `color mutation saves then reads authoritative configuration`() = runTest {
        val channel = ScriptedChannel(
            "body 1 2 3" to """{"ok":true}""",
            "save" to """{"ok":true,"queued":true}""",
            "get" to """{"body_color":[1,2,3],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}""",
        )
        val (config, save) = ManagementClient(channel).setColor(ColorTarget.Body, RgbColor(1, 2, 3))
        assertEquals(RgbColor(1, 2, 3), config.bodyColor)
        assertEquals(PersistenceState.Queued, save!!.state)
    }

    @Test fun `Amiibo upload failure cancels transaction`() = runTest {
        val data = ByteArray(540)
        val channel = ScriptedChannel(
            "amiibo status" to amiiboStatus(loaded = false),
            "amiibo begin 540 CAB5ECE6" to """{"ok":true}""",
            "amiibo chunk 0 ${"00".repeat(32)}" to """{"error":"bad chunk","code":4}""",
            "amiibo cancel" to """{"ok":true}""",
        )
        val error = runCatching { ManagementClient(channel).uploadAmiibo(data) }.exceptionOrNull()
        assertTrue(error.toString(), error is AdapterCommandException)
        channel.assertDrained()
    }

    // Product state and counters are two replies: one reply carrying both is 729
    // bytes worst case against the 512-byte wireless slot, and an oversized reply
    // is refused whole with `response_too_large` rather than truncated.
    private fun kbmStatus(mode: String, override: String, profile: String) =
        """{"mode":"$mode","override":"$override","profile":"$profile","keyboard":true,"mouse":false,"nativeMouse":false,"keyboardConn":1,"mouseConn":0}"""

    private fun kbmCounters() =
        """{"keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,"rejectedNotOwner":0,"rejectedNoPeerKey":0,"rejectedUnclassified":0,"rejectedNoRole":0,"undecodedReports":0,"rollover":0,"roleLosses":0,"mapGeneration":0,"neutralizations":0,"publishes":0,"recenters":0}"""

    private fun amiiboStatus(loaded: Boolean) =
        """{"loaded":$loaded,"dirty":false,"presented":false,"v3loaded":false,"persisted":false,"persistPending":false,"size":0,"signature":false,"hasSave2":false,"usingSave2":false,"generation":0,"payloadCrc":"00000000","uid":"","figureId":"","upload":{"active":false,"received":0,"size":0}}"""
}

private class ScriptedChannel(vararg exchanges: Pair<String, String>) : ManagementChannel {
    private val pending = ArrayDeque(exchanges.toList())

    override suspend fun transact(command: String, timeoutMillis: Long): String {
        val next = pending.removeFirstOrNull() ?: error("Unexpected command: $command")
        assertEquals(next.first, command)
        return next.second
    }

    fun assertDrained() = assertTrue("Unconsumed exchanges: $pending", pending.isEmpty())
}
