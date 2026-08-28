package dev.picoswitch.management

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Remote controller pairing, from the client's side.
 *
 * The properties worth pinning are the ones that decide whether the app can
 * mislead the user about their hardware: an operation generation that stops a
 * stale status describing the current attempt, a vocabulary that degrades
 * rather than failing, and a failure that names itself instead of collapsing
 * into "pairing failed".
 */
class RemotePairingTest {
    private fun status(
        op: Long = 1,
        state: String = "discovering",
        reason: String = "none",
        remaining: Long = 24000,
        candidates: Int = 0,
        ok: Boolean = true,
    ) = """{"ok":$ok,"op":$op,"state":"$state","reason":"$reason","remaining_ms":$remaining,"candidates":$candidates}"""

    @Test fun `a running operation decodes with its generation and countdown`() {
        val s = ManagementProtocol.pairingStatus("pairing start", status(op = 7, remaining = 24000, candidates = 2))
        assertEquals(7L, s.operation)
        assertEquals(PairingState.Discovering, s.state)
        assertEquals(PairingReason.None, s.reason)
        assertEquals(24000L, s.remainingMillis)
        assertEquals(2, s.candidates)
        assertTrue(s.active)
    }

    @Test fun `every terminal state is inactive so the app stops polling`() {
        listOf("paired", "timed_out", "cancelled", "blocked", "idle").forEach { wire ->
            val s = ManagementProtocol.pairingStatus("pairing status", status(state = wire, remaining = 0))
            assertFalse("$wire must not read as active", s.active)
        }
        // And the two running states must, or the app would stop following a
        // pairing that is still in progress.
        listOf("discovering", "connecting").forEach { wire ->
            assertTrue(ManagementProtocol.pairingStatus("pairing status", status(state = wire)).active)
        }
    }

    @Test fun `a refusal names its cause rather than collapsing to failure`() {
        val busy = ManagementProtocol.pairingStatus(
            "pairing start",
            status(state = "blocked", reason = "busy", remaining = 0, ok = false),
        )
        assertEquals(PairingState.Blocked, busy.state)
        assertEquals(PairingReason.Busy, busy.reason)

        val disabled = ManagementProtocol.pairingStatus(
            "pairing start",
            status(state = "blocked", reason = "management_disabled", remaining = 0, ok = false),
        )
        assertEquals(PairingReason.ManagementDisabled, disabled.reason)

        val timedOut = ManagementProtocol.pairingStatus(
            "pairing status",
            status(state = "timed_out", reason = "no_controller", remaining = 0),
        )
        assertEquals(PairingReason.NoController, timedOut.reason)
    }

    @Test fun `an unknown state or reason degrades instead of failing the reply`() {
        // A newer adapter may know words this build does not. Refusing the reply
        // would leave the app unable to read the generation or the countdown,
        // which are the parts that keep it honest.
        val s = ManagementProtocol.pairingStatus(
            "pairing status",
            status(state = "negotiating", reason = "solar_flare", remaining = 5000),
        )
        assertEquals(PairingState.Unknown, s.state)
        assertEquals(PairingReason.Unknown, s.reason)
        assertFalse(s.active)
        assertEquals(5000L, s.remainingMillis)
    }

    @Test fun `a status without an operation generation is rejected`() {
        // Without it a late reply cannot be told from a current one, and the
        // whole staleness guard is gone.
        val error = runCatching {
            ManagementProtocol.pairingStatus(
                "pairing status",
                """{"ok":true,"state":"discovering","reason":"none","remaining_ms":1000,"candidates":0}""",
            )
        }.exceptionOrNull()
        assertTrue("expected a protocol rejection, got $error", error is ManagementException)
    }

    @Test fun `the commands carry no duration argument`() {
        // The window belongs to the firmware and is the same one the adapter's
        // own pairing button opens. A client-supplied duration would make the
        // physical gesture depend on what an app asked for earlier.
        assertEquals("pairing start", ManagementCommands.PAIRING_START)
        assertEquals("pairing status", ManagementCommands.PAIRING_STATUS)
        assertEquals("pairing cancel", ManagementCommands.PAIRING_CANCEL)
    }

    @Test fun `missing optional fields default rather than failing`() {
        val s = ManagementProtocol.pairingStatus("pairing status", """{"ok":true,"op":3}""")
        assertEquals(3L, s.operation)
        assertEquals(PairingState.Unknown, s.state)
        assertEquals(0L, s.remainingMillis)
        assertEquals(0, s.candidates)
    }
}
