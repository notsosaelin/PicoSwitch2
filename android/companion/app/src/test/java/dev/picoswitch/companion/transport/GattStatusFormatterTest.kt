package dev.picoswitch.companion.transport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GattStatusFormatterTest {
    @Test
    fun connectionStatusUsesHciNamespace() {
        assertEquals(
            "HCI status=0x16 LOCAL_HOST_TERMINATION",
            GattStatusFormatter.describe(GattFailureStage.Connect, 0x16),
        )
    }

    @Test
    fun commandStatusUsesGattNamespace() {
        assertEquals(
            "GATT status=0x0D INVALID_ATTRIBUTE_LENGTH",
            GattStatusFormatter.describe(GattFailureStage.Command, 0x0d),
        )
    }

    @Test
    fun unknownStatusStillPreservesNamespaceAndHex() {
        assertEquals(
            "HCI status=0x2A UNKNOWN",
            GattStatusFormatter.describe(GattFailureStage.Connect, 0x2a),
        )
    }

    /**
     * The firmware-update case, which is the one users actually hit: every
     * install erases the adapter's bonds, Android keeps its own, and the
     * connection then fails with a key mismatch that no amount of retrying
     * fixes.
     */
    @Test
    fun aStaleBondTellsTheUserHowToRecover() {
        for (status in listOf(0x05, 0x06)) {
            val advice = GattStatusFormatter.recovery(GattFailureStage.Connect, status)
            assertNotNull("status 0x${status.toString(16)} has no recovery", advice)
            assertTrue(advice!!.contains("Bluetooth settings"))
            assertTrue(advice.contains("Forget"))
        }
    }

    @Test
    fun anUnrelatedFailureOffersNoAdvice() {
        // Advice attached to a failure it does not explain sends people to
        // Settings to fix something that is not wrong, and teaches them to
        // ignore it the time it is right.
        assertNull(GattStatusFormatter.recovery(GattFailureStage.Connect, 0x08))
        assertNull(GattStatusFormatter.recovery(GattFailureStage.Connect, 0x3e))
    }

    @Test
    fun theSameCodeInTheGattNamespaceIsNotAStaleBond() {
        // 0x05 is INSUFFICIENT_AUTHENTICATION on an ATT operation, which is a
        // different fact about an already-established link. Reusing one integer
        // across two namespaces is exactly the trap `describe` exists to avoid,
        // and the advice must not fall into it.
        assertNull(GattStatusFormatter.recovery(GattFailureStage.Command, 0x05))
        assertNull(GattStatusFormatter.recovery(GattFailureStage.Services, 0x06))
    }
}
