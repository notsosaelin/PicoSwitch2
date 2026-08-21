package dev.picoswitch.companion.transport

import org.junit.Assert.assertEquals
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
}
