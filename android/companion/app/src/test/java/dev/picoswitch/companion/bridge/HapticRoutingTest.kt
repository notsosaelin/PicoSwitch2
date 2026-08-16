package dev.picoswitch.companion.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Vibrator selection is the whole rumble bug, so it is pinned here rather than
 * discovered on hardware.
 *
 * The app used to drive the phone's system vibrator unconditionally. A gamepad's
 * motors live on its InputDevice and are reachable through a disjoint AOSP stack,
 * so a controller could never be driven at all. These cases fix the cascade.
 */
class HapticRoutingTest {
    private val s = android.os.Build.VERSION_CODES.S

    @Test
    fun `device vibrator ids win on api 31 and above`() {
        val probe = HapticProbe(
            deviceVibratorIds = listOf(0, 1),
            deviceHasVibrator = true,
            deviceIsExternal = true,
            systemHasVibrator = true,
            devicePresent = true,
        )
        assertEquals(HapticStage.DeviceMulti, HapticRouting.choose(probe, s))
    }

    @Test
    fun `below api 31 falls to the device's single vibrator`() {
        val probe = HapticProbe(
            deviceVibratorIds = listOf(0, 1),
            deviceHasVibrator = true,
            deviceIsExternal = true,
            devicePresent = true,
        )
        assertEquals(HapticStage.DeviceSingle, HapticRouting.choose(probe, 30))
    }

    @Test
    fun `a built-in controller with no actuator may use the system vibrator`() {
        val probe = HapticProbe(
            deviceIsExternal = false,
            systemHasVibrator = true,
            devicePresent = true,
        )
        assertEquals(HapticStage.System, HapticRouting.choose(probe, s))
    }

    /**
     * Regression: Android marks the AYN Thor's BUILT-IN "Odin Controller" as
     * EXTERNAL (measured 2026-08-14: `Classes: KEYBOARD | GAMEPAD | JOYSTICK |
     * EXTERNAL`, `ids=[] hasVibrator=false`). An isExternal veto therefore cut off
     * the only actuator on the device. The project's own ADB audit already said
     * "do not reject isExternal == true".
     */
    @Test
    fun `an external-classified controller with no actuator still reaches the system vibrator`() {
        val probe = HapticProbe(
            deviceIsExternal = true,
            systemHasVibrator = true,
            devicePresent = true,
        )
        assertEquals(HapticStage.System, HapticRouting.choose(probe, s))
    }

    @Test
    fun `the vibrate_on setting is reported but never changes routing`() {
        val on = HapticProbe(systemHasVibrator = true, devicePresent = true)
        val off = on.copy(systemVibrationSettingOn = false)
        assertEquals(HapticRouting.choose(on, s), HapticRouting.choose(off, s))
        // The setting is reported in the route line for the record...
        assertTrue(HapticRouting.describe(off, HapticStage.System).contains("vibrateOnSetting=false"))
        // ...and surfaced separately as the actionable warning, because "where did
        // we route it" and "will anything play it" are different questions.
        assertTrue(
            HapticRouting.warning(off, HapticStage.System)!!.contains("System vibration is off"),
        )
        assertNull(HapticRouting.warning(on, HapticStage.System))
    }

    @Test
    fun `an unbindable actuator is reported as a warning rather than looking healthy`() {
        val probe = HapticProbe(devicePresent = true)
        assertEquals(HapticStage.None, HapticRouting.choose(probe, s))
        assertTrue(
            HapticRouting.warning(probe, HapticStage.None)!!.contains("No actuator"),
        )
    }

    @Test
    fun `no device and no system actuator resolves to nothing`() {
        assertEquals(HapticStage.None, HapticRouting.choose(HapticProbe(), s))
    }

    @Test
    fun `empty id list does not select the multi path`() {
        val probe = HapticProbe(
            deviceVibratorIds = emptyList(),
            deviceHasVibrator = true,
            devicePresent = true,
        )
        assertEquals(HapticStage.DeviceSingle, HapticRouting.choose(probe, s))
    }

    @Test
    fun `description names every stage that was probed`() {
        val probe = HapticProbe(
            deviceVibratorIds = listOf(3),
            deviceHasVibrator = true,
            deviceIsExternal = false,
            systemHasVibrator = true,
            devicePresent = true,
        )
        val text = HapticRouting.describe(probe, HapticRouting.choose(probe, s))
        assertTrue(text, text.contains("DeviceMulti"))
        assertTrue(text, text.contains("ids=[3]"))
        assertTrue(text, text.contains("external=false"))
        assertTrue(text, text.contains("system=true"))
    }
}
