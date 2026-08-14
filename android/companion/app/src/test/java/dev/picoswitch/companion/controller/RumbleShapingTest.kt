package dev.picoswitch.companion.controller

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Amplitude shaping between the console's 0..255 and one phone actuator.
 *
 * Kept pure precisely so these rules can be pinned without a vibrator, a device,
 * or any hardware ritual.
 */
class RumbleShapingTest {

    @Test
    fun `silence stays silent`() {
        assertEquals(0, RumbleShaping.shape(0, 0))
        assertEquals(0, RumbleShaping.shape(0, 200))
    }

    @Test
    fun `amplitudes below the gate are silenced rather than buzzing`() {
        // An LRA below its start threshold makes driver noise and no movement.
        assertEquals(0, RumbleShaping.shape(1, 0))
        assertEquals(0, RumbleShaping.shape(RumbleShaping.GATE_OFF, 0))
    }

    @Test
    fun `full scale survives shaping`() {
        assertEquals(255, RumbleShaping.shape(255, 0))
    }

    @Test
    fun `hysteresis keeps a value parked on the boundary from chattering`() {
        val between = (RumbleShaping.GATE_OFF + RumbleShaping.GATE_ON) / 2

        // From silence, a value in the dead band is not enough to start.
        assertEquals(0, RumbleShaping.shape(between, 0))

        // Already running, the same value keeps it running rather than producing
        // a stream of stop/start cycles around the threshold.
        assertTrue(RumbleShaping.shape(between, 100) > 0)
    }

    @Test
    fun `quantisation collapses imperceptible changes to the same level`() {
        // Android cannot alter an effect's amplitude in flight, so every distinct
        // value costs an actuator restart. Neighbouring values must not.
        val a = RumbleShaping.shape(200, 0)
        val b = RumbleShaping.shape(201, a)
        assertEquals(a, b)
    }

    @Test
    fun `a live effect never quantises down into silence`() {
        // A value above the start threshold must keep driving the actuator even
        // when the quantiser would otherwise round it below the gate.
        val shaped = RumbleShaping.shape(RumbleShaping.GATE_ON, 0)
        assertTrue(shaped >= RumbleShaping.GATE_ON)
    }

    @Test
    fun `shaping is stable so an unchanged console value causes no retrigger`() {
        // Feeding a shaped value back in must be a fixed point, otherwise a
        // constant rumble would restart the actuator forever.
        var value = RumbleShaping.shape(180, 0)
        repeat(5) { value = RumbleShaping.shape(value, value) }
        assertEquals(RumbleShaping.shape(180, 0), value)
    }

    @Test
    fun `out of range input is clamped`() {
        assertEquals(255, RumbleShaping.shape(4000, 0))
        assertEquals(0, RumbleShaping.shape(-50, 0))
    }
}
