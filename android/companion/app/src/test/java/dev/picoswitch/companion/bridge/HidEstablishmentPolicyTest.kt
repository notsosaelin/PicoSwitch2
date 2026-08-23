package dev.picoswitch.companion.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * One Controller Link establishment generation at a time.
 *
 * Regression, 2026-08-22. Android rebinds the HID Device profile service on its
 * own. The transport used to register and connect from that callback guarded
 * only by `stopped`, which just `stop()` writes -- so after a FAILED attempt
 * (`stopped` still false, `requestedHost` still set) a spurious rebind started a
 * second establishment nobody asked for:
 *
 * ```text
 * Registering -> connecting -> Idle -> Unsupported
 * HID profile: service disconnected
 * Registering -> connecting -> Failed      <- unrequested second attempt
 * ```
 */
class HidEstablishmentPolicyTest {

    @Test fun `an explicit request grants authority`() {
        assertTrue(
            HidEstablishmentPolicy.mayAutoRegister(HidEstablishmentIntent.Wanted, stopped = false),
        )
    }

    /** The exact condition that produced the second attempt. */
    @Test fun `a rebind with nothing requested stays passive`() {
        assertFalse(
            "\"not stopped\" is not \"wants a link\"; conflating them is the defect",
            HidEstablishmentPolicy.mayAutoRegister(HidEstablishmentIntent.Idle, stopped = false),
        )
    }

    @Test fun `a stopped transport never auto-registers`() {
        HidEstablishmentIntent.entries.forEach { intent ->
            assertFalse(
                "intent=$intent",
                HidEstablishmentPolicy.mayAutoRegister(intent, stopped = true),
            )
        }
    }

    // ------------------------------------------------------------- outcomes

    @Test fun `a terminal outcome ends the authority`() {
        assertEquals(
            HidEstablishmentIntent.Idle,
            HidEstablishmentPolicy.afterOutcome(HidEstablishmentIntent.Wanted, terminal = true),
        )
    }

    /**
     * A transition is not a conclusion. STATE_DISCONNECTING in particular is not
     * terminal (see [HidConnectionStateTest]), and an in-flight attempt must
     * keep its authority across it.
     */
    @Test fun `a non-terminal transition preserves the authority`() {
        assertEquals(
            HidEstablishmentIntent.Wanted,
            HidEstablishmentPolicy.afterOutcome(HidEstablishmentIntent.Wanted, terminal = false),
        )
    }

    @Test fun `an ended establishment is not revived by a non-terminal transition`() {
        assertEquals(
            HidEstablishmentIntent.Idle,
            HidEstablishmentPolicy.afterOutcome(HidEstablishmentIntent.Idle, terminal = false),
        )
    }

    /**
     * The two policies compose: only a terminal HID connection state may end an
     * establishment, and only an ended establishment stops a rebind connecting.
     */
    @Test fun `disconnecting does not end an establishment`() {
        val terminal = HidConnectionState.isTerminal(android.bluetooth.BluetoothProfile.STATE_DISCONNECTING)
        assertEquals(
            HidEstablishmentIntent.Wanted,
            HidEstablishmentPolicy.afterOutcome(HidEstablishmentIntent.Wanted, terminal),
        )
    }
}
