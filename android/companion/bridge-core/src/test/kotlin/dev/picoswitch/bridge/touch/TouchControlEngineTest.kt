package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.DpadState
import dev.picoswitch.bridge.core.TouchContribution
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * Contact ownership and control state, with no platform in sight.
 *
 * Every scenario here is written against contact IDs that are deliberately
 * non-contiguous and delivered out of order, because that is what real touch
 * platforms do and because an implementation that keys on delivery position
 * passes a two-finger test and fails a five-finger one.
 */
class TouchControlEngineTest {

    private lateinit var engine: TouchControlEngine
    private lateinit var resolved: ResolvedTouchLayout
    private val published = mutableListOf<TouchContribution>()
    private var menuTaps = 0
    private val feedback = mutableListOf<TouchFeedbackEvent>()

    @Before fun setUp() {
        published.clear()
        feedback.clear()
        menuTaps = 0
        engine = TouchControlEngine(
            onContribution = { published += it },
            onMenu = { menuTaps++ },
            feedback = { feedback += it },
        )
        resolved = TouchLayoutResolver.resolve(
            TouchLayoutV1.layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        engine.setLayout(resolved)
        published.clear()
    }

    // ------------------------------------------------------------- claim rules

    @Test fun `a contact claims exactly the control it landed on`() {
        down(id = 9_001, control = TouchLayoutV1.FACE_SOUTH)
        assertEquals(TouchLayoutV1.FACE_SOUTH, engine.ownerOf(9_001))
        assertEquals(setOf(ControllerButton.A), latest().positionalButtons)
        assertEquals(1, engine.diagnostics().ownedControls)
    }

    @Test fun `a contact that lands on nothing owns nothing`() {
        // The quiet centre of the layout, which is quiet on purpose.
        engine.onContact(TouchContact(id = 5, phase = TouchPhase.Down, x = 400f, y = 220f))
        assertNull(engine.ownerOf(5))
        assertEquals(1, engine.diagnostics().contactsUnclaimed.toInt())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    /**
     * An unowned contact must not wander into a control and seize it later. A
     * palm resting in the middle of the screen would otherwise take the stick
     * away from the thumb the moment it shifted.
     */
    @Test fun `an unowned contact cannot claim a control by moving onto one`() {
        engine.onContact(TouchContact(id = 5, phase = TouchPhase.Down, x = 400f, y = 220f))
        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        engine.onContact(TouchContact(id = 5, phase = TouchPhase.Move, x = stick.centerX, y = stick.centerY))
        assertNull(engine.ownerOf(5))
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a second contact cannot steal an owned stick`() {
        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        down(id = 11, control = TouchLayoutV1.STICK_LEFT)
        move(id = 11, x = stick.centerX - stick.trackingRadius, y = stick.centerY)
        val held = latest()

        engine.onContact(TouchContact(id = 12, phase = TouchPhase.Down, x = stick.centerX, y = stick.centerY))
        assertEquals(11L, engine.contactOn(TouchLayoutV1.STICK_LEFT))
        assertNull(engine.ownerOf(12))
        assertEquals(1, engine.diagnostics().contactsContested.toInt())
        assertEquals("the interloper must not recentre the stick", held, engine.contribution)
    }

    /**
     * Leaving the visual circle clamps the stick; it does NOT hand the contact to
     * whatever is under the thumb now. Releasing here would make a wide turn end
     * with the thumb pressing a face button.
     */
    @Test fun `a stick keeps its contact outside its own bounds and clamps`() {
        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        down(id = 3, control = TouchLayoutV1.STICK_LEFT)
        move(id = 3, x = stick.centerX + stick.trackingRadius * 5f, y = stick.centerY)
        assertEquals(TouchLayoutV1.STICK_LEFT, engine.ownerOf(3))
        assertEquals(255, latest().leftX)

        // Straight across the whole layout, over other controls, still owned.
        val face = resolved.control(TouchLayoutV1.FACE_EAST)!!
        move(id = 3, x = face.centerX, y = face.centerY)
        assertEquals(TouchLayoutV1.STICK_LEFT, engine.ownerOf(3))
        assertTrue(latest().positionalButtons.isEmpty())
    }

    @Test fun `up releases only its own control`() {
        down(id = 21, control = TouchLayoutV1.FACE_SOUTH)
        down(id = 22, control = TouchLayoutV1.SHOULDER_RIGHT)
        assertEquals(setOf(ControllerButton.A), latest().positionalButtons)
        assertEquals(setOf(ControllerButton.R1), latest().logicalButtons)

        up(id = 21, control = TouchLayoutV1.FACE_SOUTH)
        assertTrue(latest().positionalButtons.isEmpty())
        assertEquals(setOf(ControllerButton.R1), latest().logicalButtons)
        assertEquals(TouchLayoutV1.SHOULDER_RIGHT, engine.ownerOf(22))
    }

    @Test fun `cancel releases ownership and is counted separately from a lift`() {
        down(id = 31, control = TouchLayoutV1.STICK_RIGHT)
        cancel(id = 31)
        assertNull(engine.ownerOf(31))
        assertEquals(1, engine.diagnostics().contactsCancelled.toInt())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    // ------------------------------------------------------------- multi-touch

    /**
     * Platform contact IDs are stable but the collection order is not. The engine
     * must be indifferent to the order events arrive in.
     */
    @Test fun `ownership survives reordered, non-contiguous contact ids`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        val right = resolved.control(TouchLayoutV1.STICK_RIGHT)!!
        down(id = 907, control = TouchLayoutV1.STICK_LEFT)
        down(id = 12, control = TouchLayoutV1.STICK_RIGHT)
        down(id = 4_100, control = TouchLayoutV1.FACE_SOUTH)

        // Deliver the two stick moves in the opposite order to the downs.
        move(id = 12, x = right.centerX, y = right.centerY - right.trackingRadius)
        move(id = 907, x = left.centerX - left.trackingRadius, y = left.centerY)

        val state = latest()
        assertEquals(1, state.leftX)
        assertEquals(128, state.leftY)
        assertEquals(128, state.rightX)
        assertEquals(1, state.rightY)
        assertEquals(setOf(ControllerButton.A), state.positionalButtons)
        assertEquals(TouchLayoutV1.STICK_LEFT, engine.ownerOf(907))
        assertEquals(TouchLayoutV1.STICK_RIGHT, engine.ownerOf(12))
        assertEquals(TouchLayoutV1.FACE_SOUTH, engine.ownerOf(4_100))
    }

    @Test fun `both sticks move at once and independently`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        val right = resolved.control(TouchLayoutV1.STICK_RIGHT)!!
        down(id = 1, control = TouchLayoutV1.STICK_LEFT)
        down(id = 2, control = TouchLayoutV1.STICK_RIGHT)
        move(id = 1, x = left.centerX + left.trackingRadius, y = left.centerY)
        move(id = 2, x = right.centerX, y = right.centerY + right.trackingRadius)

        assertEquals(255, latest().leftX)
        assertEquals(255, latest().rightY)

        up(id = 1, control = TouchLayoutV1.STICK_LEFT)
        assertEquals(128, latest().leftX)
        assertEquals("the other stick must be untouched", 255, latest().rightY)
    }

    @Test fun `a seven contact chord holds every control at once`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        val right = resolved.control(TouchLayoutV1.STICK_RIGHT)!!
        down(id = 41, control = TouchLayoutV1.STICK_LEFT)
        down(id = 57, control = TouchLayoutV1.STICK_RIGHT)
        down(id = 63, control = TouchLayoutV1.FACE_SOUTH)
        down(id = 64, control = TouchLayoutV1.FACE_EAST)
        down(id = 88, control = TouchLayoutV1.TRIGGER_RIGHT)
        down(id = 90, control = TouchLayoutV1.SHOULDER_LEFT)
        down(id = 91, control = TouchLayoutV1.HOME)
        move(id = 41, x = left.centerX - left.trackingRadius, y = left.centerY)
        move(id = 57, x = right.centerX + right.trackingRadius, y = right.centerY)

        val state = latest()
        assertEquals(setOf(ControllerButton.A, ControllerButton.B), state.positionalButtons)
        assertEquals(
            setOf(ControllerButton.R2, ControllerButton.L1, ControllerButton.Home),
            state.logicalButtons,
        )
        assertEquals(1, state.leftX)
        assertEquals(255, state.rightX)
        assertEquals(255, state.rightTrigger)
        assertEquals(7, engine.diagnostics().ownedControls)
    }

    @Test fun `stick plus D-pad plus face plus shoulder coexist`() {
        val dpad = resolved.control(TouchLayoutV1.DPAD)!!
        down(id = 1, control = TouchLayoutV1.DPAD)
        move(id = 1, x = dpad.centerX, y = dpad.centerY - dpad.trackingRadius)
        down(id = 2, control = TouchLayoutV1.FACE_NORTH)
        down(id = 3, control = TouchLayoutV1.SHOULDER_RIGHT)

        val state = latest()
        assertEquals(DpadState(up = true), state.dpad)
        assertEquals(setOf(ControllerButton.Y), state.positionalButtons)
        assertEquals(setOf(ControllerButton.R1), state.logicalButtons)
    }

    // ------------------------------------------------------------ control kinds

    /**
     * A physical trigger publishes both a digital bit and an analog value, and
     * the adapter's seam reads either. A touch trigger publishing only one would
     * be a second contract for the same control.
     */
    @Test fun `a trigger publishes the digital bit and full analog together`() {
        down(id = 1, control = TouchLayoutV1.TRIGGER_LEFT)
        assertEquals(255, latest().leftTrigger)
        assertTrue(ControllerButton.L2 in latest().logicalButtons)

        up(id = 1, control = TouchLayoutV1.TRIGGER_LEFT)
        assertEquals(0, latest().leftTrigger)
        assertFalse(ControllerButton.L2 in latest().logicalButtons)
    }

    @Test fun `a stick returns to exact centre the instant its contact ends`() {
        val stick = resolved.control(TouchLayoutV1.STICK_RIGHT)!!
        down(id = 1, control = TouchLayoutV1.STICK_RIGHT)
        move(id = 1, x = stick.centerX + stick.trackingRadius, y = stick.centerY - stick.trackingRadius)
        assertNotEquals(128, latest().rightX)

        up(id = 1, control = TouchLayoutV1.STICK_RIGHT)
        assertEquals(128, latest().rightX)
        assertEquals(128, latest().rightY)
    }

    @Test fun `the D-pad slides between directions without lifting`() {
        val dpad = resolved.control(TouchLayoutV1.DPAD)!!
        val reach = dpad.trackingRadius * 0.8f
        down(id = 1, control = TouchLayoutV1.DPAD)
        move(id = 1, x = dpad.centerX, y = dpad.centerY - reach)
        assertEquals(DpadState(up = true), latest().dpad)

        move(id = 1, x = dpad.centerX + reach, y = dpad.centerY - reach)
        assertEquals(DpadState(up = true, right = true), latest().dpad)

        move(id = 1, x = dpad.centerX + reach, y = dpad.centerY)
        assertEquals(DpadState(right = true), latest().dpad)

        move(id = 1, x = dpad.centerX + reach, y = dpad.centerY + reach)
        assertEquals(DpadState(down = true, right = true), latest().dpad)

        up(id = 1, control = TouchLayoutV1.DPAD)
        assertEquals(DpadState.None, latest().dpad)
    }

    @Test fun `the face positions map to positional buttons, not to letters`() {
        // The engine reports POSITIONS. Turning those into A/B/X/Y is the shared
        // layout resolver's job and happens later, once.
        down(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        assertEquals(setOf(ControllerButton.A), latest().positionalButtons)
        up(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        down(id = 2, control = TouchLayoutV1.FACE_EAST)
        assertEquals(setOf(ControllerButton.B), latest().positionalButtons)
        up(id = 2, control = TouchLayoutV1.FACE_EAST)
        down(id = 3, control = TouchLayoutV1.FACE_WEST)
        assertEquals(setOf(ControllerButton.X), latest().positionalButtons)
        up(id = 3, control = TouchLayoutV1.FACE_WEST)
        down(id = 4, control = TouchLayoutV1.FACE_NORTH)
        assertEquals(setOf(ControllerButton.Y), latest().positionalButtons)
    }

    @Test fun `the menu control never reaches the controller state`() {
        down(id = 1, control = TouchLayoutV1.MENU)
        assertEquals(TouchContribution.Neutral, engine.contribution)
        up(id = 1, control = TouchLayoutV1.MENU)
        assertEquals(1, menuTaps)
    }

    @Test fun `a cancelled menu contact does not open the menu`() {
        down(id = 1, control = TouchLayoutV1.MENU)
        cancel(id = 1)
        assertEquals(0, menuTaps)
    }

    // --------------------------------------------------------------- publishing

    /** One event, one snapshot. A half-applied controller must never be visible. */
    @Test fun `one contact event produces at most one published contribution`() {
        published.clear()
        down(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        assertEquals(1, published.size)
        down(id = 2, control = TouchLayoutV1.TRIGGER_RIGHT)
        // A trigger changes an analog value AND a button bit; still one snapshot.
        assertEquals(2, published.size)
    }

    @Test fun `a move that changes nothing publishes nothing`() {
        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        down(id = 1, control = TouchLayoutV1.STICK_LEFT)
        published.clear()
        // Inside the deadzone in both cases, so the published axes are identical.
        move(id = 1, x = stick.centerX + 1f, y = stick.centerY)
        move(id = 1, x = stick.centerX + 2f, y = stick.centerY)
        assertEquals(0, published.size)
    }

    // ---------------------------------------------------------- release-all

    @Test fun `release-all clears every control and every ownership`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        val dpad = resolved.control(TouchLayoutV1.DPAD)!!
        down(id = 1, control = TouchLayoutV1.STICK_LEFT)
        move(id = 1, x = left.centerX - left.trackingRadius, y = left.centerY)
        down(id = 2, control = TouchLayoutV1.DPAD)
        move(id = 2, x = dpad.centerX, y = dpad.centerY - dpad.trackingRadius)
        down(id = 3, control = TouchLayoutV1.TRIGGER_LEFT)
        down(id = 4, control = TouchLayoutV1.FACE_SOUTH)
        assertNotEquals(TouchContribution.Neutral, engine.contribution)

        engine.releaseAll(TouchReleaseReason.ModeExit)

        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(0, engine.diagnostics().ownedControls)
        assertEquals(0, engine.diagnostics().activeContacts)
        assertNull(engine.ownerOf(1))
        assertEquals(TouchReleaseReason.ModeExit, engine.diagnostics().lastReleaseReason)
    }

    @Test fun `release-all is idempotent`() {
        down(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        engine.releaseAll(TouchReleaseReason.LinkEnded)
        published.clear()
        engine.releaseAll(TouchReleaseReason.LinkEnded)
        engine.releaseAll(TouchReleaseReason.HostInactive)
        assertEquals("a repeated release must not publish anything new", 0, published.size)
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(0, engine.diagnostics().ownedControls)
    }

    /**
     * After a release, a contact that was already down must not resume. Its Up
     * still has to be tolerated, and it must not resurrect the control.
     */
    @Test fun `a contact held across a release-all cannot resume`() {
        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        down(id = 1, control = TouchLayoutV1.STICK_LEFT)
        engine.releaseAll(TouchReleaseReason.HostInactive)

        move(id = 1, x = stick.centerX - stick.trackingRadius, y = stick.centerY)
        assertEquals(TouchContribution.Neutral, engine.contribution)
        up(id = 1, control = TouchLayoutV1.STICK_LEFT)
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    /** New geometry invalidates every retained contact position. */
    @Test fun `a new layout releases everything first`() {
        down(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        assertNotEquals(TouchContribution.Neutral, engine.contribution)

        engine.setLayout(
            TouchLayoutResolver.resolve(
                TouchLayoutV1.layout,
                TouchLayoutRegion(0f, 0f, 1024f, 600f, 1f),
            ),
        )
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(TouchReleaseReason.GeometryInvalidated, engine.diagnostics().lastReleaseReason)
        assertNull(engine.ownerOf(1))
    }

    @Test fun `an unusable layout claims nothing`() {
        engine.setLayout(
            TouchLayoutResolver.resolve(
                TouchLayoutV1.layout,
                TouchLayoutRegion(0f, 0f, 412f, 915f, 1f),
            ),
        )
        engine.onContact(TouchContact(id = 1, phase = TouchPhase.Down, x = 200f, y = 400f))
        assertNull(engine.ownerOf(1))
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    // ------------------------------------------------------------------ haptics

    @Test fun `buttons feel a press and a release, sticks do not`() {
        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        down(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        up(id = 1, control = TouchLayoutV1.FACE_SOUTH)
        assertEquals(
            listOf(TouchFeedbackEvent.Press, TouchFeedbackEvent.Release),
            feedback,
        )

        feedback.clear()
        down(id = 2, control = TouchLayoutV1.STICK_LEFT)
        repeat(20) { step ->
            move(id = 2, x = stick.centerX + step * 3f, y = stick.centerY)
        }
        up(id = 2, control = TouchLayoutV1.STICK_LEFT)
        assertEquals("dragging a stick must not buzz", emptyList<TouchFeedbackEvent>(), feedback)
    }

    // ------------------------------------------------------------------ helpers

    private fun latest(): TouchContribution = engine.contribution

    private fun down(id: Long, control: String) {
        val target = resolved.control(control)!!
        engine.onContact(TouchContact(id, TouchPhase.Down, target.centerX, target.centerY, id))
    }

    private fun move(id: Long, x: Float, y: Float) {
        engine.onContact(TouchContact(id, TouchPhase.Move, x, y, id))
    }

    private fun up(id: Long, control: String) {
        val target = resolved.control(control)!!
        engine.onContact(TouchContact(id, TouchPhase.Up, target.centerX, target.centerY, id))
    }

    private fun cancel(id: Long) {
        engine.onContact(TouchContact(id, TouchPhase.Cancel, 0f, 0f, id))
    }
}
