package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerState
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * The analog trigger gesture, observed where it matters: in the sequence of
 * published [ControllerState] snapshots a console would see.
 *
 * `TouchAnalogTriggerGeometryTest` pins the axis arithmetic. This suite drives a
 * real [TouchGamepad] over a real [ControllerInputState] with the actual shipped
 * NSO GameCube layout and asserts BYTES, because the trigger byte is the whole
 * contract: the firmware's GameCube seam derives the terminal click from it and
 * discards the `L2` bit entirely, so a claim about a local Boolean would be a
 * claim about nothing.
 *
 * Explicit timestamps and a hand-driven tick loop throughout — several of these
 * behaviours are statements about time, and a test that could not fail when the
 * timing rules changed would not be testing them.
 *
 * The invariants that matter most:
 *
 * - touching a trigger publishes NOTHING, so no analog pull is ever preceded by
 *   a terminal click the user did not ask for;
 * - the published byte below the detent can never reach the value the firmware
 *   reads as clicked;
 * - a hold carries a LEVEL, and the level is the one chosen at release;
 * - a finger always outranks a hold, and the hold comes back when it lifts;
 * - no boundary can leave a trigger held with nothing touching it.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class TouchAnalogTriggerOutputTest {

    private lateinit var input: ControllerInputState
    private lateinit var gamepad: TouchGamepad
    private lateinit var resolved: ResolvedTouchLayout
    private val states = mutableListOf<ControllerState>()
    private val feedback = mutableListOf<TouchFeedbackEvent>()
    private val latchEvents = mutableListOf<TouchLatchEvent>()

    private val latchConfig get() = gamepad.config.latch
    private val triggerConfig get() = gamepad.config.trigger

    /** The GameCube `L`, the one control in the catalog with real travel. */
    private val leftTriggerId = "trigger-l"
    private val rightTriggerId = "trigger-r"

    /** What the console currently believes the left trigger is doing. */
    private val leftByte: Int get() = states.last().leftTrigger
    private val leftBytes: List<Int> get() = states.map { it.leftTrigger }

    /** The byte above which the firmware's GameCube seam calls the trigger clicked. */
    private val wireDetentByte = TouchTriggerConfig.SUB_DETENT_BYTE.toInt()

    private fun start(scope: kotlinx.coroutines.CoroutineScope) {
        input = ControllerInputState()
        gamepad = TouchGamepad(input)
        gamepad.setFeedbackBackend { feedback += it }
        gamepad.setLatchObserver { latchEvents += it }
        scope.launch(UnconfinedTestDispatcher()) { input.state.collect { states += it } }
        gamepad.activate()
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        resolved = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile).layout,
            TouchLayoutRegion(0f, 0f, 1200f, 600f, unitScale = 1f),
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        gamepad.setLayout(resolved)
        states.clear()
        feedback.clear()
        latchEvents.clear()
    }

    // ------------------------------------------------------- nothing on the way down

    /**
     * The single most damaging thing this control could do. On this personality
     * full travel IS the terminal click, so a speculative press would fire the
     * GameCube detent at the START of every deliberate analog pull.
     */
    @Test fun `touching a trigger publishes nothing at all`() = runTest {
        start(backgroundScope)
        down(MS)
        assertEquals("no snapshot changed", emptyList<ControllerState>(), states)
        // And the surface still acknowledged the touch, so the control does not
        // feel dead under the thumb.
        assertEquals(listOf(TouchFeedbackEvent.Press), feedback)
    }

    @Test fun `a pull is continuous and never preceded by a click`() = runTest {
        start(backgroundScope)
        down(MS)
        val fractions = listOf(0.1f, 0.25f, 0.4f, 0.55f, 0.7f)
        fractions.forEachIndexed { index, fraction ->
            pullTo(fraction, MS + (index + 1) * 10 * MS)
        }
        assertEquals(fractions.size, leftBytes.size)
        fractions.forEachIndexed { index, fraction ->
            assertTriggerByte("the byte follows displacement", fraction, leftBytes[index])
        }
        assertTrue(
            "and no snapshot ever asserted the click",
            leftBytes.all { it <= wireDetentByte },
        )
        assertTrue(states.none { ControllerButton.L2 in it.buttons })
    }

    // ------------------------------------------------------------------- the tap

    /**
     * A tap has to remain a full trigger click, and it lands on RELEASE: that is
     * the price of never guessing at pointer-down, and analog correctness wins.
     */
    @Test fun `a tap is one full pull that lasts long enough to be seen`() = runTest {
        start(backgroundScope)
        down(MS)
        val end = MS + 40 * MS
        up(end)
        assertEquals("full travel, once", listOf(255), leftBytes)
        assertTrue(ControllerButton.L2 in states.last().buttons)

        // Not an instant press-and-release: the session coalesces onto a 125 Hz
        // report cadence through a conflating mailbox, so a pulse that did not
        // last would collapse into no change at all.
        assertEquals(end + latchConfig.retriggerReleaseNanos, gamepad.nextDeadlineNanos())
        advanceTo(end + SECOND)
        assertEquals(listOf(255, 0), leftBytes)
        assertFalse(ControllerButton.L2 in states.last().buttons)
    }

    /**
     * A press that never moves is not ambiguous forever: after the same
     * deliberate-hold base both latch dwells derive from, it is a full pull, and
     * the trigger stops feeling dead while a thumb rests on it. A real pull
     * crosses the drag slop within a few milliseconds, long before this.
     */
    @Test fun `a still press becomes a full pull once it is deliberate`() = runTest {
        start(backgroundScope)
        down(MS)
        advanceTo(MS + latchConfig.holdThresholdNanos - MS)
        assertEquals("still nothing before the base elapses", emptyList<ControllerState>(), states)

        advanceTo(MS + latchConfig.holdThresholdNanos)
        assertEquals(listOf(255), leftBytes)
        assertTrue(TouchFeedbackEvent.TriggerDetent in feedback)

        // Held, not pulsed: it stays down until the finger lifts.
        advanceTo(MS + 10 * SECOND)
        assertEquals(listOf(255), leftBytes)
        up(MS + 10 * SECOND)
        assertEquals("and no tap pulse on top of it", listOf(255, 0), leftBytes)
    }

    /**
     * The invariant the digital hold design turns on, restated for a control
     * with a value: an ARMING press is still an ordinary press. "Tap it, then
     * keep holding it" is something games ask for directly, and a trigger that
     * published nothing for as long as the thumb stayed down would be broken in
     * exactly the case the gesture was designed around.
     *
     * It arrives LATER than it used to, and deliberately. An arming press is
     * also the press that selects a held level, and resolving it into a full
     * pull the moment it became deliberate put the terminal detent on the wire
     * before the slide had chosen anything -- see the two tests below. So the
     * resolve waits until the gesture arms and the user has had one more base to
     * start sliding. A press that never slides still ends up fully pulled.
     */
    @Test fun `tapping then holding a trigger is an ordinary held trigger`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        assertEquals("tap only; the arming press has said nothing yet", listOf(255, 0), leftBytes)
        assertTrue(gamepad.engine.armedControlIds().contains(leftTriggerId))

        val resolved = second + latchConfig.latchEngageThresholdNanos +
            latchConfig.holdThresholdNanos
        advanceTo(resolved - MS)
        assertEquals("still nothing one millisecond short", listOf(255, 0), leftBytes)
        advanceTo(resolved)
        assertEquals("then a full pull", listOf(255, 0, 255), leftBytes)

        advanceTo(second + 30 * SECOND)
        assertEquals("held for as long as the thumb is", listOf(255, 0, 255), leftBytes)
        up(second + 30 * SECOND)
        advanceTo(second + 31 * SECOND)
        assertEquals("and letting go simply ends it", listOf(255, 0, 255, 0), leftBytes)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    /**
     * The excursion this pass exists to remove, stated on the wire.
     *
     * A partial hold is chosen by sliding an armed contact, and the level the
     * user is choosing is BELOW full travel. Resolving the still press into a
     * full pull first made the console see `255` and the terminal click, then a
     * slide down through every intermediate byte, then the level -- and on the
     * GameCube personality that click is a gameplay action a 40% pull does not
     * have. Nothing full may reach the wire unless the selected travel gets
     * there.
     */
    @Test fun `a partial hold never publishes a full trigger on its way to the level`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        assertEquals("the arming press published nothing", listOf(255, 0), leftBytes)
        states.clear()

        // The selection slide, sampled the way a finger actually moves.
        listOf(0.08f, 0.17f, 0.26f, 0.34f, 0.40f).forEachIndexed { index, fraction ->
            pullTo(fraction, armed + (index + 1) * 8 * MS)
        }
        up(armed + 60 * MS)
        advanceTo(armed + 5 * SECOND)

        assertTrue(
            "the slide passed through the detent: " + leftBytes,
            leftBytes.all { it <= wireDetentByte },
        )
        assertTrue("and asserted the click", states.none { ControllerButton.L2 in it.buttons })
        // Monotonic from rest to the chosen level, with no excursion above it.
        assertTriggerByte("no excursion above the selected level", 0.40f, leftBytes.max())
        assertTriggerByte("and it is what stays held", 0.40f, leftByte)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
        // Reported at COMMIT: a PARTIAL level rather than the full pull the press
        // used to resolve into first.
        //
        // Bounded by the detent rather than by a particular step of this slide.
        // Where in the slide the commit distance falls depends on the trigger's
        // full-travel budget, which is derived from its position -- and placement
        // is deliberately part of the analog-trigger design. Pinning the commit
        // to "before the last sample" would make this test fail whenever the
        // shipped `L` moved, for a reason that has nothing to do with the claim.
        val logged = latchEvents.filterIsInstance<TouchLatchEvent.Engaged>().last().analogValue!!
        assertTrue(
            "the log records a partial level, not a full pull: " + logged,
            logged < triggerConfig.detentEngageFraction,
        )
        assertTrue(logged > 0f)
        assertTrue("commit cannot report more than the slide ever reached", logged <= 0.40f + 1e-3f)
    }

    /**
     * The same claim where it is easiest to get wrong: the selection slide may
     * begin at any point after the gesture arms, and until it does the trigger
     * still owes the console nothing.
     */
    @Test fun `a slide that starts late still selects rather than replaces a full pull`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        // Most of the way through the selection window, but not past it.
        val late = armed + latchConfig.holdThresholdNanos - 20 * MS
        advanceTo(late)
        assertEquals("nothing yet", listOf(255, 0), leftBytes)
        states.clear()

        pullTo(0.55f, late)
        up(late + 20 * MS)
        advanceTo(late + 5 * SECOND)

        assertTrue("no full pull anywhere in the slide", leftBytes.all { it <= wireDetentByte })
        assertTriggerByte("the late slide's level", 0.55f, leftByte)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
    }

    // ------------------------------------------------------------------ the detent

    @Test fun `the detent engages at full travel and holds through jitter`() = runTest {
        start(backgroundScope)
        down(MS)
        pullTo(triggerConfig.detentEngageFraction - 0.02f, MS + 10 * MS)
        assertTrue("open just below the threshold", leftByte <= wireDetentByte)
        assertFalse(ControllerButton.L2 in states.last().buttons)

        // A hair PAST the threshold rather than exactly on it: the suite asks
        // for a pull by converting a fraction into a pixel position and the
        // engine converts it back, so an exact boundary is one ULP from either
        // answer. Where the boundary itself lies is pinned as arithmetic by
        // `TouchAnalogTriggerGeometryTest.the detent has hysteresis in both
        // directions`.
        pullTo(triggerConfig.detentEngageFraction + 0.005f, MS + 20 * MS)
        assertEquals("full travel, not merely the fraction", 255, leftByte)
        assertTrue(ControllerButton.L2 in states.last().buttons)
        assertEquals(1, feedback.count { it == TouchFeedbackEvent.TriggerDetent })

        // Inside the band the click does not chatter, and neither does the byte.
        pullTo(triggerConfig.detentEngageFraction - 0.05f, MS + 30 * MS)
        assertEquals(255, leftByte)
        assertEquals("no second tick", 1, feedback.count { it == TouchFeedbackEvent.TriggerDetent })

        pullTo(triggerConfig.detentReleaseFraction - 0.02f, MS + 40 * MS)
        assertTrue("below the band it lets go", leftByte <= wireDetentByte)
        assertFalse(ControllerButton.L2 in states.last().buttons)
    }

    /**
     * The hysteresis would be a local fiction without this. The firmware reads
     * the click off the BYTE, so a sub-detent value above `224` would assert it
     * on the wire whatever this side believed.
     */
    @Test fun `no sub-detent value ever reaches the byte the firmware calls clicked`() = runTest {
        start(backgroundScope)
        down(MS)
        var time = MS
        // Starting past the drag slop, since below it the contact is still a
        // candidate for a tap and publishes nothing at all -- which is the
        // separate claim the first test in this suite makes.
        var fraction = triggerConfig.dragSlopUnits * 2f /
            fullTravelOf(resolved.control(leftTriggerId)!!)
        while (fraction <= 1f) {
            time += MS
            pullTo(fraction, time)
            val byte = leftByte
            val clicked = ControllerButton.L2 in states.last().buttons
            assertTrue(
                "byte $byte at fraction $fraction contradicts the click bit",
                clicked == (byte > wireDetentByte),
            )
            fraction += 0.005f
        }
    }

    // ---------------------------------------------------------------- ownership

    @Test fun `a pull continues far outside the visible trigger`() = runTest {
        start(backgroundScope)
        val control = resolved.control(leftTriggerId)!!
        down(MS)
        // Straight across the whole rectangle: off the control, over the face
        // buttons, past the far edge. The contact owns the trigger regardless.
        move(control.centerX + 2000f, control.centerY + 2000f, MS + 10 * MS)
        assertEquals(255, leftByte)
        assertEquals(leftTriggerId, gamepad.engine.ownerOf(CONTACT))
    }

    @Test fun `a cancelled contact clears the pull but keeps an existing hold`() = runTest {
        start(backgroundScope)
        val committed = latchAt(0.6f, from = MS)
        val resume = committed + SECOND

        down(resume)
        pullTo(0.95f, resume + 10 * MS)
        assertEquals(255, leftByte)
        cancel(resume + 20 * MS)
        advanceTo(resume + 5 * SECOND)

        assertTriggerByte("the hold survives; only the finger's pull is gone", 0.6f, leftByte)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
    }

    // -------------------------------------------------------------------- holds

    /**
     * The shared gesture, with a value attached. Tap, hold the second press past
     * the dwell, then SLIDE — and for a trigger the slide is not merely a
     * confirmation, it is what chooses the level.
     */
    @Test fun `the hold gesture locks the trigger at the level it was released on`() = runTest {
        start(backgroundScope)
        val committed = latchAt(0.62f, from = MS)
        advanceTo(committed + 5 * SECOND)

        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
        assertTriggerByte("the level it was released on", 0.62f, leftByte)
        assertFalse("not a click; only travel", ControllerButton.L2 in states.last().buttons)
        assertEquals(0, gamepad.diagnostics().activeContacts)
    }

    @Test fun `a hold dragged to full travel keeps the terminal click`() = runTest {
        start(backgroundScope)
        val committed = latchAt(1f, from = MS)
        advanceTo(committed + 5 * SECOND)
        assertEquals(255, leftByte)
        assertTrue(ControllerButton.L2 in states.last().buttons)
    }

    /**
     * A slide that selects no travel is not a hold anyone asked for: the commit
     * distance is measured along the trigger's own axis, so sliding outward or
     * sideways cannot lock it to nothing.
     */
    @Test fun `sliding away from the travel axis does not lock the trigger`() = runTest {
        start(backgroundScope)
        val control = resolved.control(leftTriggerId)!!
        val axis = axisOf(control)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        // The gesture's own leading tap has already pulsed; the claim here is
        // about what the ARMED contact does.
        states.clear()
        // Straight back OUT along the axis, several times the commit distance.
        val away = commitDistance() * 4f
        move(control.centerX - axis.x * away, control.centerY - axis.y * away, armed + 10 * MS)
        up(armed + 20 * MS)
        advanceTo(armed + 5 * SECOND)

        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertEquals("and nothing is left held", 0, leftBytes.lastOrNull() ?: 0)
        assertTrue("nor did the armed contact ever publish", leftBytes.none { it > 0 })
    }

    @Test fun `an ordinary pull on a held trigger never changes the held level`() = runTest {
        start(backgroundScope)
        val committed = latchAt(0.55f, from = MS)
        val resume = committed + SECOND

        down(resume)
        pullTo(0.85f, resume + 10 * MS)
        assertTriggerByte("the finger outranks the hold while it is down", 0.85f, leftByte)
        up(resume + 20 * MS)
        advanceTo(resume + 5 * SECOND)
        assertTriggerByte("and the hold comes back untouched", 0.55f, leftByte)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
    }

    @Test fun `a tap on a held trigger pulses full and returns to the held level`() = runTest {
        start(backgroundScope)
        val committed = latchAt(0.4f, from = MS)
        val resume = committed + SECOND

        down(resume)
        up(resume + 40 * MS)
        assertEquals("a brief full pull", 255, leftByte)
        advanceTo(resume + 5 * SECOND)
        assertTriggerByte("back to the held level", 0.4f, leftByte)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
    }

    /**
     * A hold already at full travel cannot be re-fired by pulling harder, so the
     * pulse becomes a RELEASE edge instead — the same answer the digital
     * retrigger mask gives, for the same reason.
     */
    @Test fun `a tap on a fully held trigger pulses a release edge instead`() = runTest {
        start(backgroundScope)
        val committed = latchAt(1f, from = MS)
        val resume = committed + SECOND
        states.clear()

        down(resume)
        up(resume + 40 * MS)
        assertEquals("an observable release", 0, leftByte)
        advanceTo(resume + 5 * SECOND)
        assertEquals(listOf(0, 255), leftBytes)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
    }

    @Test fun `pressing and holding a held trigger removes the hold`() = runTest {
        start(backgroundScope)
        val committed = latchAt(0.7f, from = MS)
        val resume = committed + SECOND

        down(resume)
        advanceTo(resume + latchConfig.latchReleaseThresholdNanos)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertEquals("nothing is held any more", 0, leftByte)

        up(resume + latchConfig.latchReleaseThresholdNanos + 20 * MS)
        advanceTo(resume + 5 * SECOND)
        assertEquals("and the release gesture is not also a tap", 0, leftByte)
    }

    /**
     * Editing a held level must not mean lifting off and performing the whole
     * engage gesture again: the press that removes the hold leaves the SAME
     * contact armed, so a slide immediately chooses a new one.
     */
    @Test fun `press-hold-then-slide replaces the held level without lifting off`() = runTest {
        start(backgroundScope)
        val committed = latchAt(0.4f, from = MS)
        val resume = committed + SECOND

        down(resume)
        val removed = resume + latchConfig.latchReleaseThresholdNanos
        advanceTo(removed)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())

        pullTo(0.75f, removed + 10 * MS)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
        up(removed + 20 * MS)
        advanceTo(removed + 5 * SECOND)

        assertTriggerByte("the replacement level", 0.75f, leftByte)
        assertEquals(
            "and the log states the new level",
            0.75f,
            latchEvents.filterIsInstance<TouchLatchEvent.Engaged>().last().analogValue!!,
            0.02f,
        )
    }

    // -------------------------------------------------------------- lifecycle

    @Test fun `every boundary drops a held trigger and returns it to rest`() = runTest {
        TouchReleaseReason.entries.forEach { reason ->
            states.clear()
            latchEvents.clear()
            start(backgroundScope)
            latchAt(0.6f, from = MS)
            assertNotEquals(0, leftByte)

            gamepad.release(reason)
            assertEquals("$reason left the trigger held", 0, leftByte)
            assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
            assertNull("$reason left timed work behind", gamepad.nextDeadlineNanos())
        }
    }

    @Test fun `leaving the on-screen controller drops a held trigger`() = runTest {
        start(backgroundScope)
        latchAt(0.6f, from = MS)
        gamepad.deactivate()
        assertEquals(0, states.last().leftTrigger)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    /**
     * A replaced personality is a different controller: a GameCube trigger held
     * at 60% has no meaning on a Pro Controller 2, and mapping one onto the
     * other because a control has a similar name would be a hold the user never
     * made on a console they just switched to.
     */
    @Test fun `a personality change drops a held trigger`() = runTest {
        start(backgroundScope)
        latchAt(0.6f, from = MS)
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
        gamepad.setLayout(
            TouchLayoutResolver.resolve(
                TouchLayoutComposer.compose(pro2).layout,
                resolved.region,
            ),
            TouchReleaseReason.PersonalityChanged,
        )
        assertEquals(0, states.last().leftTrigger)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    @Test fun `a tick that arrives after teardown cannot resurrect a trigger`() = runTest {
        start(backgroundScope)
        down(MS)
        val deadline = gamepad.nextDeadlineNanos()!!
        gamepad.release(TouchReleaseReason.Disposed)
        gamepad.tick(deadline + SECOND)
        gamepad.tick(deadline + 10 * SECOND)
        assertEquals(0, states.lastOrNull()?.leftTrigger ?: 0)
    }

    // ------------------------------------------------------------- multi-touch

    @Test fun `both triggers pull independently on their own contacts`() = runTest {
        start(backgroundScope)
        val left = resolved.control(leftTriggerId)!!
        val right = resolved.control(rightTriggerId)!!

        contact(CONTACT, TouchPhase.Down, left.centerX, left.centerY, MS)
        contact(CONTACT + 1, TouchPhase.Down, right.centerX, right.centerY, MS)
        pull(left, CONTACT, 0.3f, MS + 10 * MS)
        pull(right, CONTACT + 1, 0.95f, MS + 20 * MS)

        val state = states.last()
        assertTriggerByte("the left trigger's own pull", 0.3f, state.leftTrigger)
        assertEquals(255, state.rightTrigger)
        assertTrue(ControllerButton.R2 in state.buttons)
        assertFalse(ControllerButton.L2 in state.buttons)
        assertEquals(2, gamepad.diagnostics().activeContacts)
    }

    /**
     * A trigger is not allowed to become a global gesture. Holding one must
     * leave every other control ordinary, including the ones the same hand is
     * using.
     */
    @Test fun `a held trigger leaves the rest of the controller ordinary`() = runTest {
        start(backgroundScope)
        latchAt(0.5f, from = MS)
        val a = resolved.control("a")!!
        val start = MS + 10 * SECOND

        contact(CONTACT + 2, TouchPhase.Down, a.centerX, a.centerY, start)
        assertTriggerByte("the hold is unaffected by another control", 0.5f, states.last().leftTrigger)
        assertTrue(states.last().buttons.isNotEmpty())
        contact(CONTACT + 2, TouchPhase.Up, a.centerX, a.centerY, start + 40 * MS)
        assertTriggerByte("and the hold is unaffected", 0.5f, states.last().leftTrigger)
    }

    /**
     * Two quick taps on two DIFFERENT controls are two first taps. The hold
     * recognizer is scoped per control, and a trigger must not join a sequence
     * some other button started.
     */
    @Test fun `a tap on another control cannot arm a trigger`() = runTest {
        start(backgroundScope)
        val z = resolved.control("z")!!
        contact(CONTACT + 3, TouchPhase.Down, z.centerX, z.centerY, MS)
        contact(CONTACT + 3, TouchPhase.Up, z.centerX, z.centerY, MS + 40 * MS)

        down(MS + 120 * MS)
        advanceTo(MS + 120 * MS + latchConfig.latchEngageThresholdNanos)
        pullTo(0.8f, MS + 120 * MS + latchConfig.latchEngageThresholdNanos + 10 * MS)
        up(MS + 120 * MS + latchConfig.latchEngageThresholdNanos + 20 * MS)
        advanceTo(MS + 20 * SECOND)

        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertEquals(0, leftByte)
    }

    /**
     * The reversible slide, with a level attached.
     *
     * The shared recognizer gives an analog trigger the same cancel, and the
     * LEVEL has to go with the hold: a trigger that kept the value it was locked
     * at would republish it the moment anything latched the control again.
     */
    @Test fun `sliding back to the origin cancels a trigger hold and its level`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        // The gesture's own leading tap has already pulsed full; the claims here
        // are about what the armed contact does.
        states.clear()

        pullTo(0.6f, armed + 10 * MS)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
        assertTriggerByte("the level it locked at", 0.6f, leftByte)

        // All the way back to the trigger: inside the cancel radius.
        pullTo(0.03f, armed + 20 * MS)
        assertEquals("the hold is off", emptySet<String>(), gamepad.engine.latchedControlIds())
        assertEquals(1, gamepad.diagnostics().latchesCancelled)
        assertTrue(
            "and the finger still owns the trigger",
            gamepad.engine.ownerOf(CONTACT) == leftTriggerId,
        )

        up(armed + 40 * MS)
        advanceTo(armed + 5 * SECOND)
        assertEquals("nothing is held, at any level", 0, leftByte)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertTrue("and no detent was ever asserted", leftBytes.all { it <= wireDetentByte })
    }

    // ------------------------------------------------- the fallback is a decision

    /**
     * The selection window closing is a STATE TRANSITION, not merely something
     * that published a value.
     *
     * A press that waits it out has been answered as an ordinary held trigger.
     * Leaving the arm standing let a slide made afterwards still lock a partial
     * hold -- a persistent state reached through a gesture the recognizer had
     * already resolved as something else, and one that reads as the trigger
     * spontaneously dropping from full to a level the user never chose.
     */
    @Test fun `the full-pull fallback consumes the latch candidate`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        assertTrue("armed before the window closes", gamepad.engine.armedControlIds().contains(leftTriggerId))

        advanceTo(armed + latchConfig.holdThresholdNanos)
        assertEquals("the fallback published a full pull", 255, leftByte)
        assertEquals(
            "and the candidate is gone",
            emptySet<String>(),
            gamepad.engine.armedControlIds(),
        )
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    /**
     * The other branch, unchanged: a slide that arrives IN time still selects,
     * and still never passes through the detent on its way there.
     */
    @Test fun `a slide before the fallback still selects a partial hold`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        // Comfortably inside the window.
        val sliding = armed + latchConfig.holdThresholdNanos / 2L
        advanceTo(sliding)
        assertEquals("nothing published while it waited", listOf(255, 0), leftBytes)
        states.clear()

        pullTo(0.45f, sliding)
        up(sliding + 20 * MS)
        advanceTo(sliding + 5 * SECOND)

        assertTrue("no full pull anywhere", leftBytes.all { it <= wireDetentByte })
        assertFalse(ControllerButton.L2 in states.last().buttons)
        assertTriggerByte("the selected level", 0.45f, leftByte)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
    }

    /**
     * The case the residual path allowed: full pull first, THEN a slide.
     *
     * The slide must move the live value like any other pull -- the finger is
     * still down and still owns the trigger -- and must leave nothing held when
     * it lifts.
     */
    @Test fun `a slide after the fallback moves the value but cannot latch`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        val resolvedAt = armed + latchConfig.holdThresholdNanos
        advanceTo(resolvedAt)
        assertEquals(255, leftByte)
        states.clear()

        // A deliberate slide, several times the commit distance along the axis,
        // ending on a partial level.
        pullTo(0.35f, resolvedAt + 10 * MS)
        assertTriggerByte("the live value follows the finger", 0.35f, leftByte)
        assertEquals("but nothing locked", emptySet<String>(), gamepad.engine.latchedControlIds())
        assertTrue(
            "and no engage was logged",
            latchEvents.filterIsInstance<TouchLatchEvent.Engaged>().isEmpty(),
        )

        up(resolvedAt + 20 * MS)
        advanceTo(resolvedAt + 5 * SECOND)
        assertEquals("it ends unlatched and at rest", 0, leftByte)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertNull("with no timed work left behind", gamepad.nextDeadlineNanos())
    }

    /**
     * The consumption is scoped to the CONTACT that spent it. Lifting off and
     * performing the gesture again must work exactly as it always did, or the
     * fix would have turned one abandoned attempt into a dead control.
     */
    @Test fun `a fresh gesture after a consumed candidate can still latch`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val armed = second + latchConfig.latchEngageThresholdNanos
        advanceTo(armed + latchConfig.holdThresholdNanos)
        up(armed + latchConfig.holdThresholdNanos + 20 * MS)
        advanceTo(armed + 5 * SECOND)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())

        val committed = latchAt(0.5f, from = armed + 6 * SECOND)
        advanceTo(committed + 5 * SECOND)
        assertEquals(setOf(leftTriggerId), gamepad.engine.latchedControlIds())
        assertTriggerByte("the new gesture chose its level", 0.5f, leftByte)
    }

    // ------------------------------------------------------------ re-placement

    /**
     * The axis and the distance are both derived from WHERE the control is, and
     * the layout editor exists so users move it. Neither may be remembered from
     * the placement it had: a trigger dragged to the bottom of the screen has to
     * pull UPWARD, over the shorter vertical distance, on the very next gesture.
     */
    @Test fun `moving a trigger repoints and rescales the next pull`() = runTest {
        start(backgroundScope)
        val cornerTravel = fullTravelOf(resolved.control(leftTriggerId)!!)
        down(MS)
        pullTo(1f, MS + 10 * MS)
        assertEquals("the shipped corner placement pulls inward", 255, leftByte)
        up(MS + 20 * MS)
        advanceTo(MS + SECOND)

        // Straight down the middle, near the bottom edge: a purely vertical axis
        // pointing back up at the centre of the rectangle.
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val moved = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(
                profile,
                nudged(profile, leftTriggerId, 0.425f, 0.8f),
            ).layout,
            resolved.region,
            TouchLayoutAuditMode.UserDraft,
        )
        assertTrue(moved.problem ?: "", moved.fits)
        gamepad.setLayout(moved)
        resolved = moved
        states.clear()

        val control = moved.control(leftTriggerId)!!
        val axis = axisOf(control)
        assertEquals("the pull is now straight up", -1f, axis.y, 1e-3f)
        val travel = fullTravelOf(control)
        assertEquals("over a quarter of the height", moved.region.height * 0.25f, travel, 1e-3f)
        assertTrue("which is shorter than the corner placement asked for", travel < cornerTravel)

        val start = 2 * SECOND
        contact(CONTACT, TouchPhase.Down, control.centerX, control.centerY, start)
        // The direction that used to be the pull is now the wrong way entirely.
        contact(CONTACT, TouchPhase.Move, control.centerX, control.centerY + travel, start + 10 * MS)
        assertEquals("downward no longer pulls it", 0, leftBytes.lastOrNull() ?: 0)

        contact(CONTACT, TouchPhase.Move, control.centerX, control.centerY - travel / 2f, start + 20 * MS)
        assertTriggerByte("half the new distance is half the pull", 0.5f, leftByte)
        contact(CONTACT, TouchPhase.Move, control.centerX, control.centerY - travel, start + 30 * MS)
        assertEquals("and the whole of it is a full pull", 255, leftByte)
        contact(CONTACT, TouchPhase.Up, control.centerX, control.centerY - travel, start + 40 * MS)
        advanceTo(start + 5 * SECOND)
        assertEquals(0, leftByte)
    }

    // ------------------------------------------------------------ fill direction

    private fun fillOf(id: String) = gamepad.diagnostics().analogTriggerFills[id]

    /**
     * The direction is published for every analog trigger in the layout, not
     * only for one that has been touched: a control at rest still has to know
     * which way it WOULD fill.
     */
    @Test fun `both triggers publish a fill direction before anything is touched`() = runTest {
        start(backgroundScope)
        val fills = gamepad.diagnostics().analogTriggerFills
        assertEquals("only the controls with real travel", setOf(leftTriggerId, rightTriggerId), fills.keys)
        // As their mirrored outer placements dictate.
        assertEquals(TouchFillDirection.Right, fills[leftTriggerId])
        assertEquals(TouchFillDirection.Left, fills[rightTriggerId])
    }

    /**
     * Frozen once the swipe has declared itself.
     *
     * A thumb sweeping an arc crosses the dominance boundary all the time, and a
     * fill that flipped from downward to sideways mid-pull would read as the
     * control changing its mind about what the user is doing.
     */
    @Test fun `a curved sweep never flips the fill direction`() = runTest {
        start(backgroundScope)
        val control = resolved.control(leftTriggerId)!!
        down(MS)
        // The first movement past the slop is what fixes it -- here, downward.
        move(control.centerX + 8f, control.centerY + 120f, MS + 10 * MS)
        val frozen = fillOf(leftTriggerId)
        assertEquals(TouchFillDirection.Down, frozen)

        // Then a sweep that is emphatically sideways, back up, and across.
        listOf(
            control.centerX + 400f to control.centerY + 20f,
            control.centerX + 520f to control.centerY - 180f,
            control.centerX - 260f to control.centerY + 380f,
            control.centerX + 40f to control.centerY + 300f,
        ).forEachIndexed { index, (x, y) ->
            move(x, y, MS + (index + 2) * 10 * MS)
            assertEquals("step $index", frozen, fillOf(leftTriggerId))
        }
        up(MS + 100 * MS)
    }

    /**
     * The idle default's freeze made observable.
     *
     * Before a swipe exists there is nothing to read but the axis, and that axis
     * is the one frozen at pointer-down. Geometry is replaced UNDER the live
     * contact here, which the product never does -- `setLayout` releases first,
     * and every retained contact position was measured against the old rectangle
     * -- because reaching past that is the only way to make the difference
     * between the frozen axis and a re-derived one visible.
     */
    @Test fun `geometry replaced under a live contact does not turn the fill`() = runTest {
        start(backgroundScope)
        down(MS)
        assertEquals(TouchFillDirection.Right, fillOf(leftTriggerId))

        gamepad.engine.installLayout(movedToBottomCentre())
        // The control now sits below the centre and would derive Up if the
        // picture were re-derived; the gesture in progress keeps its answer.
        assertEquals("frozen for this contact", TouchFillDirection.Right, fillOf(leftTriggerId))

        up(MS + 40 * MS)
        // The NEXT gesture picks up where the control actually is.
        assertEquals(TouchFillDirection.Up, fillOf(leftTriggerId))
    }

    // --------------------------------------------- the fill follows the SWIPE

    /**
     * The correction this pass exists for, on the control that exposed it.
     *
     * The shipped `R` has an inward axis of about `(-0.732, +0.681)`. A straight
     * DOWNWARD swipe projects positively onto that and correctly increases
     * travel -- but the axis leans horizontally, so a fill chosen from the axis
     * drew a bar growing LEFT while the thumb moved DOWN. The value is still the
     * axis projection; only the picture now reads the swipe.
     */
    @Test fun `a downward swipe on R fills down even though its axis leans across`() = runTest {
        start(backgroundScope)
        val right = resolved.control(rightTriggerId)!!
        val axis = axisOf(right)
        assertTrue("the axis really is horizontally dominant", abs(axis.x) > abs(axis.y))
        assertTrue("and it leans left", axis.x < 0f)

        contact(RIGHT_CONTACT, TouchPhase.Down, right.centerX, right.centerY, MS)
        contact(RIGHT_CONTACT, TouchPhase.Move, right.centerX, right.centerY + 200f, MS + 10 * MS)

        assertEquals(TouchFillDirection.Down, fillOf(rightTriggerId))
        // And the value is unchanged: still the projection onto the inward axis.
        assertEquals(
            "travel is the axis projection, not the swipe",
            expectedRightByte(0f, 200f, right),
            states.last().rightTrigger,
        )
        assertTrue("a downward swipe does pull it", states.last().rightTrigger > 0)
    }

    @Test fun `a leftward swipe on R fills left`() = runTest {
        start(backgroundScope)
        val right = resolved.control(rightTriggerId)!!
        contact(RIGHT_CONTACT, TouchPhase.Down, right.centerX, right.centerY, MS)
        contact(RIGHT_CONTACT, TouchPhase.Move, right.centerX - 200f, right.centerY, MS + 10 * MS)

        assertEquals(TouchFillDirection.Left, fillOf(rightTriggerId))
        assertEquals(
            expectedRightByte(-200f, 0f, right),
            states.last().rightTrigger,
        )
    }

    @Test fun `a downward swipe on L fills down`() = runTest {
        start(backgroundScope)
        val left = resolved.control(leftTriggerId)!!
        down(MS)
        move(left.centerX, left.centerY + 200f, MS + 10 * MS)
        assertEquals(TouchFillDirection.Down, fillOf(leftTriggerId))
        assertTrue("and it pulls", leftByte > 0)
    }

    /**
     * The other two cardinals, on the control whose axis points down-and-right,
     * so neither of them is the direction the axis would have chosen.
     */
    @Test fun `upward and rightward swipes fill up and right`() = runTest {
        start(backgroundScope)
        val left = resolved.control(leftTriggerId)!!

        down(MS)
        move(left.centerX + 20f, left.centerY - 200f, MS + 10 * MS)
        assertEquals(TouchFillDirection.Up, fillOf(leftTriggerId))
        // Straight back out of the trigger, so the value stays at rest -- the
        // picture's direction and the analog value are separate answers.
        assertEquals("no travel from a backwards swipe", 0, leftBytes.lastOrNull() ?: 0)
        up(MS + 20 * MS)
        advanceTo(MS + SECOND)

        val second = 2 * SECOND
        contact(CONTACT, TouchPhase.Down, left.centerX, left.centerY, second)
        contact(CONTACT, TouchPhase.Move, left.centerX + 200f, left.centerY + 20f, second + 10 * MS)
        assertEquals(TouchFillDirection.Right, fillOf(leftTriggerId))
        assertTrue("and rightward does pull it", leftByte > 0)
    }

    /**
     * Below the slop the movement is jitter, not a swipe, and committing to a
     * direction there would let a resting thumb decide the picture.
     */
    @Test fun `movement below the slop does not establish a direction`() = runTest {
        start(backgroundScope)
        val left = resolved.control(leftTriggerId)!!
        val slop = triggerConfig.dragSlopUnits * resolved.region.unitScale
        down(MS)

        // Sideways, but not far enough to be a drag at all.
        move(left.centerX + slop * 0.5f, left.centerY, MS + 5 * MS)
        assertEquals(
            "still the position-derived default",
            TouchFillDirection.Right,
            fillOf(leftTriggerId),
        )
        assertEquals("and nothing published", emptyList<ControllerState>(), states)

        // Past it, downward: NOW the swipe decides.
        move(left.centerX + slop * 0.5f, left.centerY + slop * 3f, MS + 10 * MS)
        assertEquals(TouchFillDirection.Down, fillOf(leftTriggerId))
    }

    /**
     * A swipe direction belongs to its contact. Releasing discards it, and the
     * next gesture on the same control decides again from scratch.
     */
    @Test fun `a new gesture may choose a different direction`() = runTest {
        start(backgroundScope)
        val left = resolved.control(leftTriggerId)!!

        down(MS)
        move(left.centerX + 200f, left.centerY + 10f, MS + 10 * MS)
        assertEquals(TouchFillDirection.Right, fillOf(leftTriggerId))
        up(MS + 20 * MS)
        advanceTo(MS + SECOND)
        assertEquals(
            "released, so back to the position-derived default",
            TouchFillDirection.Right,
            fillOf(leftTriggerId),
        )

        val second = 2 * SECOND
        contact(CONTACT, TouchPhase.Down, left.centerX, left.centerY, second)
        contact(CONTACT, TouchPhase.Move, left.centerX + 10f, left.centerY + 200f, second + 10 * MS)
        assertEquals("the new gesture decides for itself", TouchFillDirection.Down, fillOf(leftTriggerId))
    }

    /**
     * The separation, stated as an equality: two swipes that fill DIFFERENTLY
     * produce the SAME analog value when their projection onto the inward axis
     * is the same. Presentation cannot reach travel.
     */
    @Test fun `the fill direction does not change what the trigger publishes`() = runTest {
        start(backgroundScope)
        val left = resolved.control(leftTriggerId)!!
        val axis = axisOf(left)
        val travel = fullTravelOf(left) * 0.5f

        // Along the axis: fills whichever cardinal that lean picks.
        down(MS)
        move(left.centerX + axis.x * travel, left.centerY + axis.y * travel, MS + 10 * MS)
        val alongAxisByte = leftByte
        val alongAxisFill = fillOf(leftTriggerId)
        up(MS + 20 * MS)
        advanceTo(MS + SECOND)

        // A different stroke with the SAME projection: straight down far enough
        // that dy * axis.y equals the projection above.
        val second = 2 * SECOND
        val downOnly = travel / axis.y
        contact(CONTACT, TouchPhase.Down, left.centerX, left.centerY, second)
        contact(CONTACT, TouchPhase.Move, left.centerX, left.centerY + downOnly, second + 10 * MS)

        // Within one LSB: the two strokes reconstruct the same projection through
        // different pixel paths, and the suite's usual float tolerance applies.
        assertEquals(
            "same projection, same byte",
            alongAxisByte.toDouble(),
            leftByte.toDouble(),
            1.0,
        )
        assertEquals("but this one was a downward swipe", TouchFillDirection.Down, fillOf(leftTriggerId))
        assertEquals(
            "the position-derived axis still points right",
            TouchFillDirection.Right,
            alongAxisFill,
        )
    }

    /** The byte `R` publishes for a displacement, via the inward-axis projection. */
    private fun expectedRightByte(dx: Float, dy: Float, control: ResolvedTouchControl): Int {
        val value = TouchTriggerTravel.analogValue(dx, dy, axisOf(control), fullTravelOf(control))
        val detent = value >= triggerConfig.detentEngageFraction
        val published = if (detent) 1f else minOf(value, triggerConfig.subDetentCeiling)
        return (published * 255f).roundToInt()
    }

    /**
     * Moving a trigger in the editor re-presents it immediately, with no special
     * case for where it started and without waiting to be pressed first.
     */
    @Test fun `moving a trigger turns its fill for the next gesture`() = runTest {
        start(backgroundScope)
        assertEquals(TouchFillDirection.Right, fillOf(leftTriggerId))

        gamepad.setLayout(movedToBottomCentre())
        assertEquals("at rest, straight away", TouchFillDirection.Up, fillOf(leftTriggerId))

        val control = gamepad.engine.resolvedLayout.control(leftTriggerId)!!
        contact(CONTACT, TouchPhase.Down, control.centerX, control.centerY, 2 * SECOND)
        assertEquals("and the gesture agrees", TouchFillDirection.Up, fillOf(leftTriggerId))
        contact(CONTACT, TouchPhase.Up, control.centerX, control.centerY, 2 * SECOND + 20 * MS)
    }

    /** `trigger-l` dragged to the bottom centre, where its axis points back up. */
    private fun movedToBottomCentre(): ResolvedTouchLayout {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val moved = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(
                profile,
                nudged(profile, leftTriggerId, 0.425f, 0.8f),
            ).layout,
            resolved.region,
            TouchLayoutAuditMode.UserDraft,
        )
        assertTrue(moved.problem ?: "", moved.fits)
        return moved
    }

    // ----------------------------------------------------------- digital triggers

    /**
     * Only the GameCube personality has trigger travel the console acts on. A
     * Pro Controller 2 `ZL` is digital on the far side however hard it is pulled,
     * so it must keep answering on the way DOWN — a travel gesture there would
     * let a stray drag silently send nothing.
     */
    @Test fun `a digital trigger still presses fully the instant it is touched`() = runTest {
        start(backgroundScope)
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
        val pro2Layout = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(pro2).layout,
            resolved.region,
        )
        gamepad.setLayout(pro2Layout)
        states.clear()

        val zl = pro2Layout.controls.first {
            (it.spec.action as? TouchControlAction.Trigger)?.side == ControlSide.Left
        }
        assertFalse((zl.spec.action as TouchControlAction.Trigger).analog)
        contact(CONTACT + 4, TouchPhase.Down, zl.centerX, zl.centerY, MS)
        assertEquals(255, states.last().leftTrigger)
        assertTrue(ControllerButton.L2 in states.last().buttons)
        contact(CONTACT + 4, TouchPhase.Up, zl.centerX, zl.centerY, MS + 40 * MS)
        assertEquals(0, states.last().leftTrigger)
    }

    // ------------------------------------------------------------------ helpers

    /**
     * Assert the published byte for a pull of [fraction], within one LSB.
     *
     * The tolerance is this suite's own float error, not slack in the contract:
     * a pull is asked for by converting a fraction into a pixel position and the
     * engine converts it back, so a value sitting exactly on a rounding boundary
     * can land either side of it. One byte in 255 is 0.4% of travel and cannot
     * cross the detent, which every claim that depends on the detent states
     * against [wireDetentByte] separately.
     */
    private fun assertTriggerByte(message: String, fraction: Float, actual: Int) {
        assertEquals(message, expectedByte(fraction).toDouble(), actual.toDouble(), 1.0)
    }

    /** The byte the wire carries for a pull of [fraction], capped below the detent. */
    private fun expectedByte(fraction: Float): Int {
        val detent = fraction >= triggerConfig.detentEngageFraction
        val value = if (detent) 1f else minOf(fraction, triggerConfig.subDetentCeiling)
        return (value * 255f).roundToInt()
    }

    private fun advanceTo(nowNanos: Long) {
        while (true) {
            val deadline = gamepad.nextDeadlineNanos() ?: return
            if (deadline > nowNanos) return
            gamepad.tick(deadline)
        }
    }

    /** Tap, then hold the second press until the engage dwell arms it. */
    private fun armEngage(at: Long): Long {
        down(at)
        up(at + 40 * MS)
        advanceTo(at + 40 * MS)
        val second = at + 160 * MS
        down(second)
        advanceTo(second + latchConfig.latchEngageThresholdNanos)
        return second
    }

    /** The whole hold gesture, ending on a pull of [fraction]. Returns the release time. */
    private fun latchAt(fraction: Float, from: Long): Long {
        val second = armEngage(from)
        val armed = second + latchConfig.latchEngageThresholdNanos
        pullTo(fraction, armed + 10 * MS)
        up(armed + 20 * MS)
        advanceTo(armed + 20 * MS)
        return armed + 20 * MS
    }

    private fun commitDistance(): Float =
        latchConfig.latchCommitDistanceUnits * resolved.region.unitScale

    /** Move the owning contact to exactly [fraction] of full travel along the axis. */
    private fun pullTo(fraction: Float, at: Long) =
        pull(resolved.control(leftTriggerId)!!, CONTACT, fraction, at)

    private fun pull(control: ResolvedTouchControl, id: Long, fraction: Float, at: Long) {
        val axis = axisOf(control)
        val travel = fullTravelOf(control) * fraction
        contact(id, TouchPhase.Move, control.centerX + axis.x * travel, control.centerY + axis.y * travel, at)
    }

    private fun axisOf(control: ResolvedTouchControl) = TouchTriggerTravel.inwardAxis(
        control.centerX, control.centerY, resolved.region, triggerConfig.centerEpsilonUnits,
    )

    /** Full travel for this control's own axis; the two are no longer separable. */
    private fun fullTravelOf(control: ResolvedTouchControl) = TouchTriggerTravel.fullTravelPx(
        resolved.region,
        axisOf(control),
        triggerConfig.travelFraction,
        triggerConfig.verticalTravelRatio,
    )

    private fun down(at: Long) = atCentre(TouchPhase.Down, at)

    private fun up(at: Long) = atCentre(TouchPhase.Up, at)

    private fun cancel(at: Long) = atCentre(TouchPhase.Cancel, at)

    private fun move(x: Float, y: Float, at: Long) = contact(CONTACT, TouchPhase.Move, x, y, at)

    private fun atCentre(phase: TouchPhase, at: Long) {
        val control = resolved.control(leftTriggerId)!!
        contact(CONTACT, phase, control.centerX, control.centerY, at)
    }

    private fun contact(id: Long, phase: TouchPhase, x: Float, y: Float, at: Long) {
        gamepad.engine.onContact(TouchContact(id, phase, x, y, at))
    }
}

private const val CONTACT = 11L
private const val RIGHT_CONTACT = 12L
private const val MS = 1_000_000L
private const val SECOND = 1_000L * MS
