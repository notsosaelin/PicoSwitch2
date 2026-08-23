package dev.picoswitch.companion.transport

import org.junit.Assert.*
import org.junit.Test

/**
 * Telling "the adapter was reset" apart from "not paired" and from noise.
 *
 * The adapter erases its Bluetooth bonds on every firmware install, on purpose,
 * so a stale controller cannot reconnect after a flash and quietly invalidate a
 * test. The cost is that Android and the app both keep believing in a pairing
 * the adapter has forgotten.
 *
 * Captured on hardware 2026-08-23: connect-stage status 0x05 against a device
 * Android still reported as BOND_BONDED, with `encrypt failure status 0x6`
 * (PIN_OR_KEY_MISSING) in the platform log underneath. Six attempts across
 * fourteen minutes all failed identically before Android dropped its own bond
 * and the app finally offered repair.
 */
class AdapterResetSignatureTest {

    private fun connectFailure(status: Int) =
        GattTransportException("connect failed", status, GattFailureStage.Connect)

    @Test fun `key failure against a live Android bond means the adapter was reset`() {
        assertTrue(
            AdapterResetSignature.isBondMismatch(
                GattFailureStage.Connect, 0x05, androidStillBonded = true,
            ),
        )
        assertTrue(
            AdapterResetSignature.isBondMismatch(
                GattFailureStage.Connect, 0x06, androidStillBonded = true,
            ),
        )
    }

    @Test fun `without an Android bond this is simply not paired`() {
        // A different situation, a different message, and a different flow: the
        // user has nothing to forget, so sending them to Bluetooth settings
        // would be wrong.
        assertFalse(
            AdapterResetSignature.isBondMismatch(
                GattFailureStage.Connect, 0x05, androidStillBonded = false,
            ),
        )
        assertFalse(
            AdapterResetSignature.isBondMismatch(
                GattFailureStage.Connect, 0x06, androidStillBonded = false,
            ),
        )
    }

    @Test fun `ordinary transient failures keep their retry behaviour`() {
        for (status in listOf(GattRecoveryPolicy.GENERIC_STACK_ERROR,
                              GattRecoveryPolicy.CONNECTION_TIMEOUT,
                              GattRecoveryPolicy.CONNECTION_CONGESTED,
                              0x08, 0x13, 0x16, 0x3e)) {
            assertFalse(
                "status 0x${status.toString(16)} must not be read as a bond mismatch",
                AdapterResetSignature.isBondMismatch(
                    GattFailureStage.Connect, status, androidStillBonded = true,
                ),
            )
        }
        // And the two signature statuses must stay OUT of the retry set: no
        // number of retries can conjure a link key on the adapter.
        assertFalse(AdapterResetSignature.isBondMismatch(connectFailure(133), true))
        assertFalse(GattRecoveryPolicy.isRetryable(connectFailure(0x05)))
        assertFalse(GattRecoveryPolicy.isRetryable(connectFailure(0x06)))
    }

    @Test fun `ATT-layer authentication is a different namespace and must not match`() {
        // GATT 0x05 means one attribute needed authentication. That is not a
        // link that cannot be encrypted, and treating it as a reset would send
        // the user to forget a perfectly good pairing.
        for (stage in listOf(GattFailureStage.Services,
                             GattFailureStage.Subscribe,
                             GattFailureStage.Command)) {
            assertFalse(
                AdapterResetSignature.isBondMismatch(stage, 0x05, androidStillBonded = true),
            )
        }
    }

    @Test fun `the signature is recovered from a wrapped transport failure`() {
        val wrapped = IllegalStateException("outer", connectFailure(0x05))
        assertTrue(AdapterResetSignature.isBondMismatch(wrapped, androidStillBonded = true))
        assertFalse(AdapterResetSignature.isBondMismatch(wrapped, androidStillBonded = false))
        // A failure carrying no transport cause tells us nothing.
        assertFalse(AdapterResetSignature.isBondMismatch(IllegalStateException("bare"), true))
    }

    @Test fun `both key statuses are named rather than reported as UNKNOWN`() {
        // The field report read "HCI status=0x05 UNKNOWN", which is precisely
        // why it took a log dive to diagnose.
        assertTrue(GattStatusFormatter.describe(GattFailureStage.Connect, 0x05)
            .contains("AUTHENTICATION_FAILURE"))
        assertTrue(GattStatusFormatter.describe(GattFailureStage.Connect, 0x06)
            .contains("PIN_OR_KEY_MISSING"))
        assertTrue(GattStatusFormatter.describe(GattFailureStage.Connect, 0x05)
            .startsWith("HCI"))
        // The ATT namespace keeps its own meaning for 0x05.
        assertTrue(GattStatusFormatter.describe(GattFailureStage.Subscribe, 0x05)
            .contains("INSUFFICIENT_AUTHENTICATION"))
    }

    @Test fun `the repair message names the action the app cannot perform itself`() {
        val message = AdapterResetSignature.REPAIR_MESSAGE
        assertTrue(message.contains("reset"))
        assertTrue(message.contains("Forget"))
        assertTrue(message.contains("Bluetooth settings"))
    }
}
