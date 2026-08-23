package dev.picoswitch.companion.bridge

import android.bluetooth.BluetoothProfile
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Only a state that concludes a connect attempt may disarm its watchdog.
 *
 * Regression, 2026-08-21. Measured on an Android 16 tablet: the HID Device stack
 * reported `connecting`, then `disconnecting`, and never a final `disconnected`.
 * The handler cancelled the watchdog before checking whether it handled the
 * state, so the bridge sat on "Connecting" forever with nothing left that could
 * resolve it — an interface lying indefinitely instead of failing in eight
 * seconds, with no way out of the screen.
 */
class HidConnectionStateTest {

    @Test fun `only connected and disconnected end an attempt`() {
        assertTrue(HidConnectionState.isTerminal(BluetoothProfile.STATE_CONNECTED))
        assertTrue(HidConnectionState.isTerminal(BluetoothProfile.STATE_DISCONNECTED))
    }

    /** The exact state that caused the hang. */
    @Test fun `disconnecting does not end an attempt`() {
        assertFalse(
            "STATE_DISCONNECTING is a transition, not a conclusion; disarming on it " +
                "is what pinned the bridge on Connecting forever",
            HidConnectionState.isTerminal(BluetoothProfile.STATE_DISCONNECTING),
        )
    }

    @Test fun `connecting does not end an attempt`() {
        assertFalse(HidConnectionState.isTerminal(BluetoothProfile.STATE_CONNECTING))
    }

    /**
     * A state this build has never heard of is not a conclusion either. Assuming
     * otherwise is the same mistake with a different number.
     */
    @Test fun `an unrecognized state does not end an attempt`() {
        listOf(-1, 4, 7, 99).forEach {
            assertFalse("state $it", HidConnectionState.isTerminal(it))
        }
    }

    @Test fun `every state is named, including one this build does not know`() {
        assertEquals("disconnected", HidConnectionState.name(BluetoothProfile.STATE_DISCONNECTED))
        assertEquals("connecting", HidConnectionState.name(BluetoothProfile.STATE_CONNECTING))
        assertEquals("connected", HidConnectionState.name(BluetoothProfile.STATE_CONNECTED))
        assertEquals("disconnecting", HidConnectionState.name(BluetoothProfile.STATE_DISCONNECTING))
        assertEquals("unknown(42)", HidConnectionState.name(42))
    }
}
