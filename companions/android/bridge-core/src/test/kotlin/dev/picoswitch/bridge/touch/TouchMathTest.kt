package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.AxisRange
import dev.picoswitch.bridge.core.DpadState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.hypot

/**
 * The one conversion between touch geometry and bridge wire units.
 *
 * The endpoints are asserted exactly rather than approximately: a rounding
 * difference here is a whole axis unit on the wire, and the pairing test below
 * is the actual guard against a second scaling appearing.
 */
class TouchAxisTest {

    @Test fun `centre and full deflection are exact`() {
        assertEquals(128, TouchAxis.toBridge(0f))
        assertEquals(255, TouchAxis.toBridge(1f))
        assertEquals(1, TouchAxis.toBridge(-1f))
    }

    /**
     * The asymmetric low endpoint is INHERITED, not invented.
     *
     * `AxisRange.stick` is the physical path and has always produced 1 at full
     * negative deflection. A touch stick that reached 0 there would be a second
     * contract for the same axis byte, which is the exact class of divergence
     * the shared model exists to prevent.
     */
    @Test fun `touch conversion agrees with the physical axis conversion at the endpoints`() {
        val physical = AxisRange(-1f, 1f)
        listOf(-1f, 0f, 1f).forEach { value ->
            assertEquals(
                "touch and physical disagree at $value",
                physical.stick(value),
                TouchAxis.toBridge(value),
            )
        }
    }

    @Test fun `values beyond the domain clamp rather than wrapping`() {
        assertEquals(255, TouchAxis.toBridge(4f))
        assertEquals(1, TouchAxis.toBridge(-4f))
        assertEquals(255, TouchAxis.triggerToBridge(2f))
        assertEquals(0, TouchAxis.triggerToBridge(-2f))
    }

    /** A garbage axis byte on the wire is worse than an honest centre. */
    @Test fun `non-finite input resolves to rest`() {
        assertEquals(128, TouchAxis.toBridge(Float.NaN))
        assertEquals(128, TouchAxis.toBridge(Float.POSITIVE_INFINITY))
        assertEquals(0, TouchAxis.triggerToBridge(Float.NaN))
    }

    @Test fun `triggers rest at zero and reach full scale`() {
        assertEquals(0, TouchAxis.triggerToBridge(0f))
        assertEquals(255, TouchAxis.triggerToBridge(1f))
    }
}

class TouchStickTest {

    private val radius = 100f
    private val deadzone = 0.05f

    private fun resolve(dx: Float, dy: Float) = TouchStick.resolve(dx, dy, radius, deadzone)

    @Test fun `exact centre is neutral`() {
        assertEquals(TouchVector.Zero, resolve(0f, 0f))
    }

    @Test fun `cardinals reach full scale with the expected signs`() {
        // Screen coordinates: y grows downward, so a negative dy is UP, which is
        // a negative axis value -- the same convention the physical path uses.
        assertVector(-1f, 0f, resolve(-100f, 0f))
        assertVector(1f, 0f, resolve(100f, 0f))
        assertVector(0f, -1f, resolve(0f, -100f))
        assertVector(0f, 1f, resolve(0f, 100f))
    }

    @Test fun `up is a smaller bridge value than centre`() {
        val up = resolve(0f, -100f)
        assertTrue(TouchAxis.toBridge(up.y) < TouchAxis.NEUTRAL)
        val down = resolve(0f, 100f)
        assertTrue(TouchAxis.toBridge(down.y) > TouchAxis.NEUTRAL)
    }

    /**
     * A square gate would let a diagonal reach magnitude sqrt(2) before anything
     * clamped it, so the stick would feel faster on the diagonals than on the
     * cardinals. Every diagonal must have magnitude 1.
     */
    @Test fun `diagonals are circularly clamped, not squared`() {
        listOf(
            -100f to -100f, 100f to -100f, -100f to 100f, 100f to 100f,
        ).forEach { (dx, dy) ->
            val vector = resolve(dx, dy)
            val magnitude = hypot(vector.x.toDouble(), vector.y.toDouble()).toFloat()
            assertEquals("diagonal $dx/$dy", 1f, magnitude, 1e-4f)
            assertEquals(abs(vector.x), abs(vector.y), 1e-4f)
        }
    }

    @Test fun `travel beyond the radius clamps magnitude and preserves direction`() {
        val far = resolve(600f, 0f)
        assertVector(1f, 0f, far)
        val diagonalFar = resolve(900f, -300f)
        val magnitude = hypot(diagonalFar.x.toDouble(), diagonalFar.y.toDouble()).toFloat()
        assertEquals(1f, magnitude, 1e-4f)
        // Direction survives: three times as much x as y, still.
        assertEquals(-3f, diagonalFar.x / diagonalFar.y, 1e-3f)
    }

    @Test fun `inside the deadzone is exact centre and just outside is not`() {
        assertEquals(TouchVector.Zero, resolve(4f, 0f))
        // Exactly at the threshold is still centre; the gate is inclusive.
        assertEquals(TouchVector.Zero, resolve(5f, 0f))
        assertNotEquals(TouchVector.Zero, resolve(5.5f, 0f))
    }

    /**
     * The deadzone must REMAP the remaining travel, not merely discard the inner
     * radius. Discarding it would leave full deflection unreachable at the rim by
     * exactly the deadzone fraction.
     */
    @Test fun `the range above the deadzone is rescaled to reach full magnitude`() {
        assertEquals(1f, resolve(100f, 0f).x, 1e-4f)
        // Half of the travel ABOVE the gate is half scale, not 0.5 minus the gate.
        val half = resolve(52.5f, 0f)
        assertEquals(0.5f, half.x, 1e-3f)
    }

    @Test fun `a degenerate radius is neutral rather than infinite`() {
        assertEquals(TouchVector.Zero, TouchStick.resolve(10f, 10f, 0f, deadzone))
        assertEquals(TouchVector.Zero, TouchStick.resolve(Float.NaN, 0f, radius, deadzone))
    }

    private fun assertVector(x: Float, y: Float, actual: TouchVector) {
        assertEquals("x", x, actual.x, 1e-4f)
        assertEquals("y", y, actual.y, 1e-4f)
    }
}

class TouchDpadTest {

    private val config = TouchControlConfig.Default
    private val radius = 100f

    private fun resolve(dx: Float, dy: Float, previous: DpadState = DpadState.None) =
        TouchDpad.resolve(dx, dy, radius, config, previous)

    @Test fun `centre is neutral`() {
        assertEquals(DpadState.None, resolve(0f, 0f))
    }

    @Test fun `all four cardinals`() {
        assertEquals(DpadState(right = true), resolve(80f, 0f))
        assertEquals(DpadState(left = true), resolve(-80f, 0f))
        assertEquals(DpadState(up = true), resolve(0f, -80f))
        assertEquals(DpadState(down = true), resolve(0f, 80f))
    }

    @Test fun `all four diagonals raise two directions`() {
        assertEquals(DpadState(up = true, right = true), resolve(60f, -60f))
        assertEquals(DpadState(up = true, left = true), resolve(-60f, -60f))
        assertEquals(DpadState(down = true, right = true), resolve(60f, 60f))
        assertEquals(DpadState(down = true, left = true), resolve(-60f, 60f))
    }

    @Test fun `a contact inside the engage radius publishes nothing`() {
        assertEquals(DpadState.None, resolve(20f, 0f))
    }

    /**
     * Two radial thresholds, not one. With a single threshold a thumb resting at
     * exactly that distance alternates neutral and engaged on sub-pixel noise.
     */
    @Test fun `an engaged direction survives below the engage threshold`() {
        val engaged = resolve(40f, 0f)
        assertEquals(DpadState(right = true), engaged)
        // Between exit (0.20) and enter (0.30): still held.
        assertEquals(DpadState(right = true), resolve(25f, 0f, previous = engaged))
        // Below exit: released.
        assertEquals(DpadState.None, resolve(15f, 0f, previous = engaged))
        // And from neutral, that same 25 units does not re-engage.
        assertEquals(DpadState.None, resolve(25f, 0f))
    }

    /**
     * Angular hysteresis. A thumb parked on the Up / UpRight boundary must not
     * alternate; a deliberate turn must still land.
     */
    @Test fun `a held direction survives a small drift across a sector boundary`() {
        val up = resolve(0f, -80f)
        assertEquals(DpadState(up = true), up)

        // Up owns 67.5..112.5 degrees, so 65 already belongs to UpRight and a
        // stateless resolver would switch there.
        assertEquals(DpadState(up = true, right = true), at(65f, 80f))
        // Held, it stays Up: 25 degrees from Up's centre is inside the half
        // sector plus the margin.
        assertEquals(DpadState(up = true), at(65f, 80f, previous = up))
        // 30 degrees away is outside the margin, so a deliberate turn lands.
        assertEquals(DpadState(up = true, right = true), at(60f, 80f, previous = up))
        assertEquals(DpadState(up = true, right = true), at(45f, 80f, previous = up))
    }

    @Test fun `sliding a full circle visits all eight directions without opposites`() {
        var state = DpadState.None
        val seen = mutableSetOf<DpadState>()
        for (degrees in 0 until 360 step 5) {
            state = at(degrees.toFloat(), 80f, previous = state)
            assertTrue(
                "opposites at $degrees: $state",
                !(state.up && state.down) && !(state.left && state.right),
            )
            seen += state
        }
        assertEquals("every sector should be reachable", 8, seen.size)
    }

    @Test fun `one contact can never produce opposite directions`() {
        for (degrees in 0 until 360) {
            val state = at(degrees.toFloat(), 90f)
            assertTrue(!(state.up && state.down))
            assertTrue(!(state.left && state.right))
        }
    }

    @Test fun `a degenerate radius is neutral`() {
        assertEquals(DpadState.None, TouchDpad.resolve(50f, 50f, 0f, config, DpadState.None))
    }

    /** Places a contact at a compass angle, where 0 is right and 90 is up. */
    private fun at(degrees: Float, distance: Float, previous: DpadState = DpadState.None): DpadState {
        val radians = Math.toRadians(degrees.toDouble())
        return resolve(
            dx = (distance * kotlin.math.cos(radians)).toFloat(),
            // Negated: the input frame has y growing downward.
            dy = (-distance * kotlin.math.sin(radians)).toFloat(),
            previous = previous,
        )
    }
}
