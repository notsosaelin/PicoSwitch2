package dev.picoswitch.companion.bluetooth

import java.nio.charset.StandardCharsets
import java.util.regex.Pattern
import org.junit.Assert.*
import org.junit.Test

class AdapterBluetoothIdentityTest {
    @Test fun `current identity is the advertised product name and has eleven ASCII bytes`() {
        assertEquals("PicoSwitch2", AdapterBluetoothIdentity.CURRENT_NAME)
        assertEquals(11, AdapterBluetoothIdentity.CURRENT_NAME.toByteArray(StandardCharsets.US_ASCII).size)
        assertTrue(AdapterBluetoothIdentity.isCurrentName("picoswitch2"))
    }

    @Test fun `legacy identity remains discoverable but unrelated names do not`() {
        assertTrue(AdapterBluetoothIdentity.isKnownAdapterName("Joypad Adapter"))
        assertTrue(AdapterBluetoothIdentity.isKnownAdapterName("joypad adapter"))
        assertTrue(AdapterBluetoothIdentity.isKnownAdapterName(" PicoSwitch2 "))
        assertFalse(AdapterBluetoothIdentity.isKnownAdapterName("PicoSwitch"))
        assertFalse(AdapterBluetoothIdentity.isKnownAdapterName("PicoSwitch2 Android Bridge"))
        assertFalse(AdapterBluetoothIdentity.isKnownAdapterName(null))
    }

    @Test fun `chooser pattern accepts current and legacy names only`() {
        val pattern = Pattern.compile(
            AdapterBluetoothIdentity.CHOOSER_NAME_PATTERN,
            Pattern.CASE_INSENSITIVE,
        )
        assertTrue(pattern.matcher(AdapterBluetoothIdentity.CURRENT_NAME).matches())
        assertTrue(pattern.matcher(AdapterBluetoothIdentity.LEGACY_NAME).matches())
        assertFalse(pattern.matcher("PicoSwitch2 Android Bridge").matches())
    }
}
