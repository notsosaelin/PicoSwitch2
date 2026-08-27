package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.TouchContribution
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The two hold gestures and the retrigger, at the layer that recognizes them.
 *
 * Every scenario drives the engine with EXPLICIT timestamps and drives the tick
 * loop by hand, because all three behaviours are statements about time and a
 * test that could not fail when the timing rules changed would not be testing
 * anything. [advanceTo] is the host's driver, reproduced exactly: ask what the
 * engine is waiting for, tick at that instant, ask again.
 *
 * `TouchLatchOutputTest` pins the same behaviours as published controller-state
 * EDGES. This suite pins the recognizer itself: which press is a candidate for
 * which gesture, and what abandons one.
 *
 * The invariants that matter most are not "the gestures work" — those are easy —
 * but the five around them:
 *
 * - rapid tapping is ORDINARY rapid tapping, at any speed, latched or not;
 * - "double tap, then keep holding" is ORDINARY gameplay and never locks;
 * - engaging a hold is strictly harder than releasing one;
 * - ordinary input is never delayed or swallowed by either recognizer;
 * - no boundary, and no pending timed work, can leave a control held with
 *   nothing touching it.
 */
class TouchLatchTest {

    private lateinit var engine: TouchControlEngine
    private lateinit var resolved: ResolvedTouchLayout
    private val published = mutableListOf<TouchContribution>()
    private val feedback = mutableListOf<TouchFeedbackEvent>()
    private val latchEvents = mutableListOf<TouchLatchEvent>()

    private val config get() = engine.config.latch

    @Before fun setUp() {
        published.clear()
        feedback.clear()
        latchEvents.clear()
        engine = TouchControlEngine(
            onContribution = { published += it },
            feedback = { feedback += it },
        )
        engine.setLatchObserver { latchEvents += it }
        resolved = TouchLayoutResolver.resolve(
            TouchLayoutV1.layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        engine.setLayout(resolved)
        published.clear()
        latchEvents.clear()
    }

    // ---------------------------------------------------------------- the timing

    /**
     * The product rule, as arithmetic: creating a persistent state is twice as
     * deliberate as removing one, and both come from a single base so they
     * cannot drift apart.
     */
    @Test fun `engaging takes twice the dwell of releasing, from one base`() {
        assertEquals(config.holdThresholdNanos, config.latchReleaseThresholdNanos)
        assertEquals(config.holdThresholdNanos * 2, config.latchEngageThresholdNanos)
        val retuned = config.copy(holdThresholdNanos = 500 * MS)
        assertEquals(500 * MS, retuned.latchReleaseThresholdNanos)
        assertEquals(1000 * MS, retuned.latchEngageThresholdNanos)
    }

    /**
     * The commit distance is its own threshold, not a multiple of the drift
     * tolerance: the two answer different questions, and a slide has to be well
     * clear of anything jitter can reach.
     */
    @Test fun `the commit distance is a distinct threshold well clear of drift`() {
        assertTrue(
            config.latchCommitDistanceUnits > config.gestureSlopUnits * 2f,
        )
    }

    // ------------------------------------------------------------- ordinary input

    @Test fun `a single tap presses and releases like it always did`() {
        tap(TouchLayoutV1.FACE_NORTH, at = MS)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(listOf(TouchFeedbackEvent.Press, TouchFeedbackEvent.Release), feedback)
        assertNull("an ordinary tap leaves the engine event-driven", engine.nextDeadlineNanos())
    }

    @Test fun `a physical hold on an unlatched control with no leading tap does nothing`() {
        down(TouchLayoutV1.FACE_NORTH, at = MS)
        assertNull("one press alone is never the engage gesture", engine.nextDeadlineNanos())
        assertEquals(setOf(ControllerButton.Y), engine.contribution.positionalButtons)
        advanceTo(MS + 10 * SECOND)
        up(TouchLayoutV1.FACE_NORTH, at = MS + 10 * SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    /**
     * The defect the dwell exists to fix. A plain double tap cannot be the
     * gesture, because mashing a button IS a stream of double taps.
     */
    @Test fun `a double tap whose second press is released is just two taps`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        tap(TouchLayoutV1.FACE_NORTH, at = end + 120 * MS)
        advanceTo(end + 10 * SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(emptyList<TouchLatchEvent>(), latchEvents)
    }

    @Test fun `mashing an unlatched button at any speed never latches it`() {
        listOf(60L, 90L, 130L, 200L).forEach { period ->
            setUp()
            var time = MS
            repeat(12) {
                down(TouchLayoutV1.FACE_SOUTH, at = time)
                assertTrue(
                    "press must reach the sink on the press, not after a window",
                    ControllerButton.A in engine.contribution.positionalButtons,
                )
                time += 40 * MS
                advanceTo(time)
                up(TouchLayoutV1.FACE_SOUTH, at = time)
                assertFalse(
                    "release must reach the sink on the release",
                    ControllerButton.A in engine.contribution.positionalButtons,
                )
                time += period * MS
                advanceTo(time)
            }
            assertEquals("period ${period}ms", emptySet<String>(), engine.latchedControlIds())
            assertEquals("period ${period}ms", TouchContribution.Neutral, engine.contribution)
        }
    }

    // -------------------------------------------------------------- engaging

    /**
     * The gesture the previous implementation got wrong. "Double tap, then keep
     * holding" is something a game may ask for directly, so no dwell — however
     * long — may be allowed to turn it into a persistent hold.
     */
    @Test fun `a double tap held indefinitely is ordinary gameplay, not a latch`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        advanceTo(second + 30 * SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(
            "and it is still an ordinary held button throughout",
            setOf(ControllerButton.Y),
            engine.contribution.positionalButtons,
        )
        up(TouchLayoutV1.FACE_NORTH, at = second + 30 * SECOND)
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
    }

    @Test fun `the dwell arms the gesture and commits nothing`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        val second = end + 120 * MS
        down(TouchLayoutV1.FACE_NORTH, at = second)
        assertEquals(
            second + config.latchEngageThresholdNanos,
            engine.nextDeadlineNanos(),
        )
        assertEquals(emptySet<String>(), engine.armedControlIds())

        advanceTo(second + config.latchEngageThresholdNanos)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.armedControlIds())
        assertEquals("armed is not held", emptySet<String>(), engine.latchedControlIds())
        assertEquals(
            "arming reports nothing: nothing changed",
            emptyList<TouchLatchEvent>(),
            latchEvents,
        )
        assertTrue(TouchFeedbackEvent.LatchArmed in feedback)
        assertNull("and it is waiting on movement, not on time", engine.nextDeadlineNanos())
    }

    @Test fun `a deliberate slide while armed commits the latch`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 1.1f, at = second + SECOND)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.latchedControlIds())
        assertEquals(emptySet<String>(), engine.armedControlIds())
        assertEquals(listOf(TouchLatchEvent.Engaged(TouchLayoutV1.FACE_NORTH)), latchEvents)
        assertTrue(TouchFeedbackEvent.LatchEngaged in feedback)

        up(TouchLayoutV1.FACE_NORTH, at = second + 2 * SECOND)
        assertEquals(0, engine.diagnostics().activeContacts)
        assertEquals(setOf(ControllerButton.Y), engine.contribution.positionalButtons)
    }

    @Test fun `a slide shorter than the commit distance does not latch`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 0.9f, at = second + SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(
            "still armed, so a longer slide would still work",
            setOf(TouchLayoutV1.FACE_NORTH),
            engine.armedControlIds(),
        )
    }

    /** Any deliberate motion will do; the user should not have to guess a direction. */
    @Test fun `the slide may go in any direction`() {
        val reach = commitDistance() * 1.1f
        val diagonal = reach / 1.415f
        listOf(
            reach to 0f, -reach to 0f, 0f to reach, 0f to -reach,
            diagonal to diagonal, -diagonal to -diagonal,
        ).forEach { (dx, dy) ->
            setUp()
            val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
            slideBy(TouchLayoutV1.FACE_NORTH, dx, dy, at = second + SECOND)
            assertEquals(
                "slide ($dx, $dy)",
                setOf(TouchLayoutV1.FACE_NORTH),
                engine.latchedControlIds(),
            )
        }
    }

    /**
     * Leaving the control is neither necessary nor sufficient. A big button is
     * as easy to lock as a small one, and where inside it the user happened to
     * touch changes nothing.
     */
    @Test fun `leaving the button's bounds is not what commits the latch`() {
        val stick = resolved.control(TouchLayoutV1.SHOULDER_LEFT)!!
        // A slide that clears the control entirely but is short of the threshold.
        val shortHop = stick.hitHalfWidth * 1.05f
        assertTrue("the fixture needs a control smaller than the threshold", shortHop < commitDistance())
        val second = armEngage(TouchLayoutV1.SHOULDER_LEFT, at = MS)
        slide(TouchLayoutV1.SHOULDER_LEFT, by = shortHop, at = second + SECOND)
        assertEquals(
            "outside the button but not far enough",
            emptySet<String>(),
            engine.latchedControlIds(),
        )
        slide(TouchLayoutV1.SHOULDER_LEFT, by = commitDistance() * 1.1f, at = second + 2 * SECOND)
        assertEquals(setOf(TouchLayoutV1.SHOULDER_LEFT), engine.latchedControlIds())
    }

    @Test fun `the commit distance is the same for a large control and a small one`() {
        val large = resolved.control(TouchLayoutV1.DPAD)!!
        val small = resolved.control(TouchLayoutV1.STICK_CLICK_LEFT)!!
        assertTrue("the fixture needs two very different sizes", large.halfWidth > small.halfWidth * 2f)
        listOf(TouchLayoutV1.SHOULDER_LEFT, TouchLayoutV1.STICK_CLICK_LEFT).forEach { control ->
            setUp()
            val second = armEngage(control, at = MS)
            slide(control, by = commitDistance() * 0.9f, at = second + SECOND)
            assertEquals(control, emptySet<String>(), engine.latchedControlIds())
            slide(control, by = commitDistance() * 1.1f, at = second + 2 * SECOND)
            assertEquals(control, setOf(control), engine.latchedControlIds())
        }
    }

    /** Where inside the control the press began must not change the distance either. */
    @Test fun `touching near an edge does not change the commit distance`() {
        val target = resolved.control(TouchLayoutV1.SHOULDER_LEFT)!!
        val startX = target.centerX + target.halfWidth * 0.9f
        val end = tapAt(TouchLayoutV1.SHOULDER_LEFT, startX, target.centerY, at = MS)
        val second = end + 120 * MS
        engine.onContact(TouchContact(CONTACT, TouchPhase.Down, startX, target.centerY, second))
        advanceTo(second + config.latchEngageThresholdNanos)
        move(CONTACT, startX - commitDistance() * 0.9f, target.centerY, second + SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        move(CONTACT, startX - commitDistance() * 1.1f, target.centerY, second + 2 * SECOND)
        assertEquals(setOf(TouchLayoutV1.SHOULDER_LEFT), engine.latchedControlIds())
    }

    @Test fun `releasing while armed but before the slide leaves the control unlatched`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        up(TouchLayoutV1.FACE_NORTH, at = second + SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(emptySet<String>(), engine.armedControlIds())
        assertEquals("the press simply ended", TouchContribution.Neutral, engine.contribution)
        assertNull(engine.nextDeadlineNanos())
    }

    @Test fun `commit is immediate rather than confirmed on lift`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 1.1f, at = second + SECOND)
        assertEquals(
            "latched on the movement itself",
            setOf(TouchLayoutV1.FACE_NORTH),
            engine.latchedControlIds(),
        )
    }

    @Test fun `sliding back after commit does not undo the latch`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 1.2f, at = second + SECOND)
        slide(TouchLayoutV1.FACE_NORTH, by = 0f, at = second + 2 * SECOND)
        slide(TouchLayoutV1.FACE_NORTH, by = -commitDistance() * 1.2f, at = second + 3 * SECOND)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.latchedControlIds())
        up(TouchLayoutV1.FACE_NORTH, at = second + 4 * SECOND)
        assertEquals(setOf(ControllerButton.Y), engine.contribution.positionalButtons)
    }

    @Test fun `natural drift before the dwell does not abandon the candidate`() {
        val target = resolved.control(TouchLayoutV1.FACE_NORTH)!!
        val drift = config.gestureSlopUnits * resolved.region.unitScale * 0.8f
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        val second = end + 120 * MS
        down(TouchLayoutV1.FACE_NORTH, at = second)
        move(CONTACT, target.centerX + drift, target.centerY, second + 30 * MS)
        advanceTo(second + config.latchEngageThresholdNanos)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.armedControlIds())
    }

    /** A drag is not the hold the gesture asked for. */
    @Test fun `a drag before the dwell abandons the candidate`() {
        val target = resolved.control(TouchLayoutV1.FACE_NORTH)!!
        val drag = config.gestureSlopUnits * resolved.region.unitScale * 2f
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        val second = end + 120 * MS
        down(TouchLayoutV1.FACE_NORTH, at = second)
        move(CONTACT, target.centerX + drag, target.centerY, second + 30 * MS)
        assertNull(engine.nextDeadlineNanos())
        advanceTo(second + 5 * SECOND)
        assertEquals(emptySet<String>(), engine.armedControlIds())
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        // The press itself is unaffected: sliding off a button never released it.
        assertEquals(setOf(ControllerButton.Y), engine.contribution.positionalButtons)
    }

    @Test fun `mashing an unlatched button never even arms it`() {
        listOf(60L, 90L, 130L, 200L).forEach { period ->
            setUp()
            var time = MS
            repeat(12) {
                down(TouchLayoutV1.FACE_SOUTH, at = time)
                time += 40 * MS
                advanceTo(time)
                up(TouchLayoutV1.FACE_SOUTH, at = time)
                time += period * MS
                advanceTo(time)
            }
            assertEquals("period ${period}ms", emptySet<String>(), engine.armedControlIds())
            assertEquals("period ${period}ms", emptySet<String>(), engine.latchedControlIds())
        }
    }

    @Test fun `taps on different controls never combine into one gesture`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        down(TouchLayoutV1.FACE_EAST, at = end + 60 * MS)
        assertNull("a first press on another control is not a candidate", engine.nextDeadlineNanos())
        advanceTo(end + 5 * SECOND)
        assertEquals(emptySet<String>(), engine.armedControlIds())
    }

    @Test fun `a cancelled second press cannot arm or commit`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        val second = end + 120 * MS
        down(TouchLayoutV1.FACE_NORTH, at = second)
        engine.onContact(TouchContact(CONTACT, TouchPhase.Cancel, 0f, 0f, second + 20 * MS))
        advanceTo(second + 5 * SECOND)
        assertEquals(emptySet<String>(), engine.armedControlIds())
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a cancelled contact while armed cannot commit afterwards`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        engine.onContact(TouchContact(CONTACT, TouchPhase.Cancel, 0f, 0f, second + SECOND))
        assertEquals(emptySet<String>(), engine.armedControlIds())
        // The platform can still deliver a stray move for a contact it took away.
        slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 2f, at = second + 2 * SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a gap wider than the double-tap window is two first taps`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        down(TouchLayoutV1.FACE_NORTH, at = end + config.doubleTapWindowNanos + MS)
        assertNull(engine.nextDeadlineNanos())
        advanceTo(end + 10 * SECOND)
        assertEquals(emptySet<String>(), engine.armedControlIds())
    }

    @Test fun `a bounce closer than the minimum gap is not a double tap`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        down(TouchLayoutV1.FACE_NORTH, at = end + config.minTapGapNanos / 2)
        assertNull(engine.nextDeadlineNanos())
    }

    @Test fun `a long first press cannot start the engage gesture`() {
        down(TouchLayoutV1.FACE_NORTH, at = MS)
        val end = MS + config.maxTapDurationNanos + MS
        advanceTo(end)
        up(TouchLayoutV1.FACE_NORTH, at = end)
        down(TouchLayoutV1.FACE_NORTH, at = end + 100 * MS)
        assertNull(engine.nextDeadlineNanos())
    }

    /**
     * A platform with no clock gets no gesture rather than a random one. See
     * [TouchContact.timeNanos].
     */
    @Test fun `a host that reports no time never arms`() {
        repeat(3) {
            engine.onContact(contactAt(TouchLayoutV1.FACE_NORTH, TouchPhase.Down, 0L))
            engine.onContact(contactAt(TouchLayoutV1.FACE_NORTH, TouchPhase.Up, 0L))
        }
        assertNull(engine.nextDeadlineNanos())
        assertEquals(emptySet<String>(), engine.armedControlIds())
    }

    @Test fun `one control's slide cannot latch another`() {
        val end = tap(TouchLayoutV1.FACE_NORTH, at = MS)
        down(TouchLayoutV1.FACE_NORTH, at = end + 120 * MS, contact = 80)
        down(TouchLayoutV1.FACE_EAST, at = end + 130 * MS, contact = 81)
        advanceTo(end + 5 * SECOND)
        val target = resolved.control(TouchLayoutV1.FACE_NORTH)!!
        move(80, target.centerX + commitDistance() * 1.1f, target.centerY, end + 6 * SECOND)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.latchedControlIds())
    }

    // -------------------------------------------------------------- releasing

    @Test fun `one press held past the release dwell unlatches, with no leading tap`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        latchEvents.clear()

        val pressAt = 10 * SECOND
        down(TouchLayoutV1.FACE_NORTH, at = pressAt)
        assertEquals(
            pressAt + config.latchReleaseThresholdNanos,
            engine.nextDeadlineNanos(),
        )
        advanceTo(pressAt + config.latchReleaseThresholdNanos)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(listOf(TouchLatchEvent.Released(TouchLayoutV1.FACE_NORTH)), latchEvents)
    }

    /**
     * The finger that performed the release gesture stays authoritative. Dropping
     * the button at the instant the latch cleared would be a release edge the
     * user never made.
     */
    @Test fun `unlatching under a finger does not release until the finger lifts`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        published.clear()

        val pressAt = 10 * SECOND
        down(TouchLayoutV1.FACE_NORTH, at = pressAt)
        advanceTo(pressAt + config.latchReleaseThresholdNanos)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertTrue(
            "the button must not have dropped while a finger was on it",
            published.none { ControllerButton.Y !in it.positionalButtons },
        )

        up(TouchLayoutV1.FACE_NORTH, at = pressAt + config.latchReleaseThresholdNanos + 20 * MS)
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a cancelled release gesture neither unlatches nor retriggers`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        val pressAt = 10 * SECOND
        down(TouchLayoutV1.FACE_NORTH, at = pressAt)
        engine.onContact(TouchContact(CONTACT, TouchPhase.Cancel, 0f, 0f, pressAt + 20 * MS))
        advanceTo(pressAt + 5 * SECOND)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.latchedControlIds())
        assertEquals(0, engine.diagnostics().retriggerPulses)
    }

    @Test fun `arming, engaging and releasing all feel different from each other`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        assertEquals(
            "armed first, then locked, and no release anywhere in it",
            listOf(TouchFeedbackEvent.LatchArmed, TouchFeedbackEvent.LatchEngaged),
            feedback.filter { it.name.startsWith("Latch") },
        )
        feedback.clear()
        unlatch(TouchLayoutV1.FACE_NORTH, at = 10 * SECOND)
        assertEquals(
            "releasing needs no arming step of its own",
            listOf(TouchFeedbackEvent.LatchReleased),
            feedback.filter { it.name.startsWith("Latch") },
        )
    }

    /** Unlocking is a plain press-and-hold: no leading tap, and no slide. */
    @Test fun `releasing a hold requires no slide`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        val pressAt = 10 * SECOND
        down(TouchLayoutV1.FACE_NORTH, at = pressAt)
        advanceTo(pressAt + config.latchReleaseThresholdNanos)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
    }

    // -------------------------------------------------------------- retrigger

    @Test fun `a quick tap on a latched control pulses only once it ends`() {
        latch(TouchLayoutV1.FACE_SOUTH, at = MS)
        published.clear()
        feedback.clear()

        val tapAt = 10 * SECOND
        down(TouchLayoutV1.FACE_SOUTH, at = tapAt)
        assertTrue(
            "the decision window must leave the hold alone",
            ControllerButton.A in engine.contribution.positionalButtons,
        )
        assertEquals(emptyList<TouchContribution>(), published)

        up(TouchLayoutV1.FACE_SOUTH, at = tapAt + 40 * MS)
        advanceTo(tapAt + 40 * MS + config.retriggerReleaseNanos)

        assertEquals(
            "exactly one release edge and one press edge",
            listOf(false, true),
            published.map { ControllerButton.A in it.positionalButtons },
        )
        assertEquals(setOf(TouchLayoutV1.FACE_SOUTH), engine.latchedControlIds())
        assertEquals(1, engine.diagnostics().retriggerPulses)
    }

    @Test fun `the release edge outlives one report interval by a wide margin`() {
        // The session coalesces state onto a 125 Hz cadence through a conflated
        // mailbox, so a release and a press in the same instant collapse into no
        // change at all. This is the guarantee that they cannot.
        assertTrue(
            "a retrigger release must survive both the report cadence and a 30 Hz consumer",
            config.retriggerReleaseNanos > 33 * MS,
        )
    }

    @Test fun `mashing a latched button retriggers and never releases the hold`() {
        latch(TouchLayoutV1.FACE_SOUTH, at = MS)
        published.clear()

        var time = 10 * SECOND
        repeat(12) {
            down(TouchLayoutV1.FACE_SOUTH, at = time)
            time += 40 * MS
            up(TouchLayoutV1.FACE_SOUTH, at = time)
            time += 70 * MS
            advanceTo(time)
            assertEquals(
                "no mashed press outlives even the shorter dwell",
                setOf(TouchLayoutV1.FACE_SOUTH),
                engine.latchedControlIds(),
            )
        }
        assertEquals(12, engine.diagnostics().retriggerPulses)
        assertEquals(setOf(ControllerButton.A), engine.contribution.positionalButtons)
    }

    @Test fun `a retrigger is an ordinary press, not a latch transition`() {
        latch(TouchLayoutV1.FACE_SOUTH, at = MS)
        feedback.clear()
        latchEvents.clear()

        val tapAt = 10 * SECOND
        down(TouchLayoutV1.FACE_SOUTH, at = tapAt)
        up(TouchLayoutV1.FACE_SOUTH, at = tapAt + 40 * MS)
        advanceTo(tapAt + SECOND)

        assertEquals(
            "one press and one release, exactly as any other button",
            listOf(TouchFeedbackEvent.Press, TouchFeedbackEvent.Release),
            feedback,
        )
        assertEquals(
            "a retrigger changes nothing about the hold, so it reports nothing",
            emptyList<TouchLatchEvent>(),
            latchEvents,
        )
    }

    /** A latched trigger retriggers on both halves at once, or it retriggers on neither. */
    @Test fun `a retriggered trigger drops its analog pull as well as its bit`() {
        latch(TouchLayoutV1.TRIGGER_RIGHT, at = MS)
        assertTrue(engine.contribution.rightTrigger > 0)

        val tapAt = 10 * SECOND
        down(TouchLayoutV1.TRIGGER_RIGHT, at = tapAt)
        up(TouchLayoutV1.TRIGGER_RIGHT, at = tapAt + 40 * MS)
        assertFalse(ControllerButton.R2 in engine.contribution.logicalButtons)
        assertEquals(0, engine.contribution.rightTrigger)

        advanceTo(tapAt + 40 * MS + config.retriggerReleaseNanos)
        assertTrue(ControllerButton.R2 in engine.contribution.logicalButtons)
        assertTrue(engine.contribution.rightTrigger > 0)
    }

    @Test fun `a pulse masks only the control it belongs to`() {
        latch(TouchLayoutV1.FACE_SOUTH, at = MS)
        latch(TouchLayoutV1.SHOULDER_LEFT, at = 5 * SECOND)

        down(TouchLayoutV1.FACE_SOUTH, at = 10 * SECOND, contact = 71)
        up(TouchLayoutV1.FACE_SOUTH, at = 10 * SECOND + 40 * MS, contact = 71)
        assertFalse(ControllerButton.A in engine.contribution.positionalButtons)
        assertTrue(
            "the other hold is untouched",
            ControllerButton.L1 in engine.contribution.logicalButtons,
        )
    }

    @Test fun `an unlatched control is never masked, because its tap is already an edge`() {
        tap(TouchLayoutV1.FACE_SOUTH, at = MS)
        assertNull(engine.nextDeadlineNanos())
        assertEquals(0, engine.diagnostics().retriggerPulses)
    }

    // ----------------------------------------------------------- multi-touch

    @Test fun `a latched button and a live one coexist`() {
        latch(TouchLayoutV1.SHOULDER_LEFT, at = MS)

        val stick = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        engine.onContact(TouchContact(70, TouchPhase.Down, stick.centerX, stick.centerY, 10 * SECOND))
        move(70, stick.centerX, stick.centerY - stick.trackingRadius, 10 * SECOND)
        down(TouchLayoutV1.FACE_SOUTH, at = 10 * SECOND, contact = 71)

        val held = engine.contribution
        assertTrue(ControllerButton.L1 in held.logicalButtons)
        assertEquals(setOf(ControllerButton.A), held.positionalButtons)
        assertTrue("the stick must still track", held.leftY != TouchContribution.Neutral.leftY)
    }

    // ------------------------------------------------------------ what may latch

    @Test fun `a trigger latches both its digital bit and its full pull`() {
        latch(TouchLayoutV1.TRIGGER_RIGHT, at = MS)
        val held = engine.contribution
        assertTrue(ControllerButton.R2 in held.logicalButtons)
        assertTrue(held.rightTrigger > 0)
    }

    @Test fun `the D-pad cannot be latched`() {
        val dpad = resolved.control(TouchLayoutV1.DPAD)!!
        val edge = dpad.centerY - dpad.trackingRadius * 0.9f
        var time = MS
        repeat(2) {
            engine.onContact(TouchContact(CONTACT, TouchPhase.Down, dpad.centerX, edge, time))
            time += 500 * MS
            advanceTo(time)
            engine.onContact(TouchContact(CONTACT, TouchPhase.Up, dpad.centerX, edge, time))
            time += 120 * MS
        }
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a stick cannot be latched`() {
        var time = MS
        repeat(2) {
            down(TouchLayoutV1.STICK_LEFT, at = time)
            time += 500 * MS
            advanceTo(time)
            up(TouchLayoutV1.STICK_LEFT, at = time)
            time += 120 * MS
        }
        assertEquals(emptySet<String>(), engine.latchedControlIds())
    }

    // ------------------------------------------------------------- configuration

    @Test fun `the global setting turns both gestures off`() {
        engine.setConfig(engine.config.copy(latch = config.copy(enabledByDefault = false)))
        latchEvents.clear()
        attemptLatch(TouchLayoutV1.FACE_NORTH, at = MS)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(emptySet<String>(), engine.armedControlIds())
        assertEquals(emptyList<TouchLatchEvent>(), latchEvents)
        assertNull(engine.nextDeadlineNanos())
    }

    /** Taking the gesture away mid-arm must not leave a slide able to complete it. */
    @Test fun `disabling latch support while armed cannot commit afterwards`() {
        val second = armEngage(TouchLayoutV1.FACE_NORTH, at = MS)
        assertEquals(setOf(TouchLayoutV1.FACE_NORTH), engine.armedControlIds())

        engine.setConfig(engine.config.copy(latch = config.copy(enabledByDefault = false)))
        assertEquals(emptySet<String>(), engine.armedControlIds())

        slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 2f, at = second + SECOND)
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        up(TouchLayoutV1.FACE_NORTH, at = second + 2 * SECOND)
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a per-control choice overrides the global default in both directions`() {
        installLayout(disabled = setOf(TouchLayoutV1.FACE_NORTH))
        attemptLatch(TouchLayoutV1.FACE_NORTH, at = MS)
        assertEquals(emptySet<String>(), engine.latchedControlIds())

        engine.setConfig(engine.config.copy(latch = config.copy(enabledByDefault = false)))
        installLayout(enabled = setOf(TouchLayoutV1.FACE_EAST))
        attemptLatch(TouchLayoutV1.FACE_EAST, at = 10 * SECOND)
        assertEquals(setOf(TouchLayoutV1.FACE_EAST), engine.latchedControlIds())
    }

    @Test fun `a control the user disabled never retriggers either`() {
        installLayout(disabled = setOf(TouchLayoutV1.FACE_SOUTH))
        attemptLatch(TouchLayoutV1.FACE_SOUTH, at = MS)
        tap(TouchLayoutV1.FACE_SOUTH, at = 10 * SECOND)
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(0, engine.diagnostics().retriggerPulses)
    }

    @Test fun `turning the setting off drops what was already latched`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        latchEvents.clear()
        engine.setConfig(engine.config.copy(latch = config.copy(enabledByDefault = false)))
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertNull("and any pending timed work goes with it", engine.nextDeadlineNanos())
        assertEquals(
            listOf(
                TouchLatchEvent.Cleared(
                    setOf(TouchLayoutV1.FACE_NORTH),
                    TouchReleaseReason.SettingsChanged,
                ),
            ),
            latchEvents,
        )
    }

    /**
     * Taking the gesture away must not leave the button it would have released
     * stuck down, and must not leave a pulse nothing can lift.
     */
    @Test fun `disabling a control's latch mid-pulse clears the hold and the pulse`() {
        latch(TouchLayoutV1.FACE_SOUTH, at = MS)
        val tapAt = 10 * SECOND
        down(TouchLayoutV1.FACE_SOUTH, at = tapAt)
        up(TouchLayoutV1.FACE_SOUTH, at = tapAt + 40 * MS)
        assertTrue("a pulse is genuinely in flight", engine.nextDeadlineNanos() != null)

        engine.setConfig(engine.config.copy(latch = config.copy(enabledByDefault = false)))
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertNull(engine.nextDeadlineNanos())
        engine.onTick(tapAt + 10 * SECOND)
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    // --------------------------------------------------------------- lifecycle

    @Test fun `every global release drops every latch and says why`() {
        TouchReleaseReason.entries.forEach { reason ->
            setUp()
            latch(TouchLayoutV1.FACE_NORTH, at = MS)
            latchEvents.clear()
            engine.releaseAll(reason)
            assertEquals(reason.name, emptySet<String>(), engine.latchedControlIds())
            assertEquals(reason.name, TouchContribution.Neutral, engine.contribution)
            assertEquals(
                reason.name,
                listOf(TouchLatchEvent.Cleared(setOf(TouchLayoutV1.FACE_NORTH), reason)),
                latchEvents,
            )
            assertNull(reason.name, engine.nextDeadlineNanos())
        }
    }

    /**
     * The pull model IS the session safety: nothing captured a control, so a
     * tick that lands after a boundary finds no work at all.
     */
    @Test fun `a teardown mid-gesture cannot be resurrected by a later tick`() {
        listOf<Pair<String, (Long) -> Unit>>(
            "engage dwell" to { at ->
                val end = tap(TouchLayoutV1.FACE_NORTH, at = at)
                down(TouchLayoutV1.FACE_NORTH, at = end + 120 * MS)
            },
            "armed engage gesture" to { at ->
                val end = tap(TouchLayoutV1.FACE_NORTH, at = at)
                down(TouchLayoutV1.FACE_NORTH, at = end + 120 * MS)
                advanceTo(end + 120 * MS + config.latchEngageThresholdNanos)
            },
            "release dwell" to { at ->
                latch(TouchLayoutV1.FACE_NORTH, at = at)
                down(TouchLayoutV1.FACE_NORTH, at = at + 5 * SECOND)
            },
            "retrigger pulse" to { at ->
                latch(TouchLayoutV1.FACE_NORTH, at = at)
                down(TouchLayoutV1.FACE_NORTH, at = at + 5 * SECOND)
                up(TouchLayoutV1.FACE_NORTH, at = at + 5 * SECOND + 40 * MS)
            },
        ).forEach { (name, arm) ->
            setUp()
            arm(MS)
            assertTrue(
                "$name must genuinely be pending",
                engine.nextDeadlineNanos() != null || engine.armedControlIds().isNotEmpty(),
            )

            engine.releaseAll(TouchReleaseReason.LinkEnded)
            assertNull("$name: teardown leaves nothing scheduled", engine.nextDeadlineNanos())
            assertEquals(name, emptySet<String>(), engine.armedControlIds())
            published.clear()

            // The host's timer had already been armed and fires anyway, and the
            // platform can still deliver a move for a contact that is gone.
            engine.onTick(MS + 30 * SECOND)
            slide(TouchLayoutV1.FACE_NORTH, by = commitDistance() * 2f, at = MS + 45 * SECOND)
            engine.onTick(MS + 60 * SECOND)
            assertEquals(name, emptyList<TouchContribution>(), published)
            assertEquals(name, TouchContribution.Neutral, engine.contribution)
            assertEquals(name, emptySet<String>(), engine.latchedControlIds())
        }
    }

    @Test fun `new geometry cannot inherit a latch from the old arrangement`() {
        latch(TouchLayoutV1.FACE_NORTH, at = MS)
        engine.setLayout(
            TouchLayoutResolver.resolve(
                TouchLayoutV1.layout,
                TouchLayoutRegion(0f, 0f, 1100f, 520f, 1f),
            ),
        )
        assertEquals(emptySet<String>(), engine.latchedControlIds())
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a release also forgets a tap that was in flight`() {
        tap(TouchLayoutV1.FACE_NORTH, at = MS)
        engine.releaseAll(TouchReleaseReason.HostInactive)
        down(TouchLayoutV1.FACE_NORTH, at = MS + 120 * MS)
        assertNull(engine.nextDeadlineNanos())
    }

    @Test fun `an idle release reports nothing, so the log stays readable`() {
        engine.releaseAll(TouchReleaseReason.HostInactive)
        engine.releaseAll(TouchReleaseReason.GeometryInvalidated)
        assertEquals(emptyList<TouchLatchEvent>(), latchEvents)
    }

    // ------------------------------------------------------------------ helpers

    /**
     * The host's tick driver, reproduced: ask what the engine is waiting for,
     * tick at exactly that instant, ask again. Ticking once at the end instead
     * would collapse a pulse and a dwell into a single publish and hide the very
     * edges these tests exist to observe.
     */
    private fun advanceTo(nowNanos: Long) {
        while (true) {
            val deadline = engine.nextDeadlineNanos() ?: return
            if (deadline > nowNanos) return
            engine.onTick(deadline)
        }
    }

    /** Reinstall the layout with explicit per-control latch choices. */
    private fun installLayout(
        enabled: Set<String> = emptySet(),
        disabled: Set<String> = emptySet(),
    ) {
        val base = TouchLayoutV1.layout
        val layout = base.copy(
            controls = base.controls.map { spec ->
                when (spec.id) {
                    in enabled -> spec.copy(latch = true)
                    in disabled -> spec.copy(latch = false)
                    else -> spec
                }
            },
        )
        resolved = TouchLayoutResolver.resolve(layout, TouchLayoutRegion(0f, 0f, 832f, 440f, 1f))
        assertTrue(resolved.problem ?: "", resolved.fits)
        engine.setLayout(resolved)
    }

    /** Tap, hold past the dwell, slide away, lift; require it engaged. */
    private fun latch(control: String, at: Long) {
        attemptLatch(control, at)
        check(control in engine.latchedControlIds()) { "$control failed to latch" }
    }

    /** The commit distance in this layout's real coordinates. */
    private fun commitDistance(): Float =
        config.latchCommitDistanceUnits * resolved.region.unitScale

    /**
     * Tap, then press and hold past the dwell. Returns the second press's time,
     * leaving that contact DOWN and armed.
     */
    private fun armEngage(control: String, at: Long): Long {
        val tapEnd = tap(control, at)
        val second = tapEnd + 120 * MS
        down(control, second)
        advanceTo(second + config.latchEngageThresholdNanos)
        return second
    }

    /** Move the held contact [by] pixels along x from the control's centre. */
    private fun slide(control: String, by: Float, at: Long) {
        val target = resolved.control(control)!!
        move(CONTACT, target.centerX + by, target.centerY, at)
    }

    private fun slideBy(control: String, dx: Float, dy: Float, at: Long) {
        val target = resolved.control(control)!!
        move(CONTACT, target.centerX + dx, target.centerY + dy, at)
    }

    /** Press and hold past the release dwell, lift; require it released. */
    private fun unlatch(control: String, at: Long) {
        down(control, at)
        val settled = at + config.latchReleaseThresholdNanos
        advanceTo(settled)
        up(control, settled + 20 * MS)
        advanceTo(settled + SECOND)
        check(control !in engine.latchedControlIds()) { "$control failed to unlatch" }
    }

    private fun attemptLatch(control: String, at: Long) {
        val second = armEngage(control, at)
        val settled = second + config.latchEngageThresholdNanos
        slide(control, by = commitDistance() * 1.2f, at = settled + 20 * MS)
        up(control, settled + 40 * MS)
        advanceTo(settled + SECOND)
    }

    /** One press and release; returns the release time. */
    private fun tap(control: String, at: Long): Long {
        down(control, at)
        val end = at + 40 * MS
        up(control, end)
        return end
    }

    /** A tap at an explicit point rather than the control's centre. */
    private fun tapAt(control: String, x: Float, y: Float, at: Long): Long {
        engine.onContact(TouchContact(CONTACT, TouchPhase.Down, x, y, at))
        val end = at + 40 * MS
        engine.onContact(TouchContact(CONTACT, TouchPhase.Up, x, y, end))
        return end
    }

    private fun down(control: String, at: Long, contact: Long = CONTACT) {
        engine.onContact(contactAt(control, TouchPhase.Down, at, contact))
    }

    private fun up(control: String, at: Long, contact: Long = CONTACT) {
        engine.onContact(contactAt(control, TouchPhase.Up, at, contact))
    }

    private fun move(contact: Long, x: Float, y: Float, at: Long) {
        engine.onContact(TouchContact(contact, TouchPhase.Move, x, y, at))
    }

    private fun contactAt(
        control: String,
        phase: TouchPhase,
        at: Long,
        contact: Long = CONTACT,
    ): TouchContact {
        val target = resolved.control(control)!!
        return TouchContact(contact, phase, target.centerX, target.centerY, at)
    }
}

/**
 * The per-control choice, from the editor through the document and back.
 *
 * The property this suite exists for is the one that is invisible until it
 * breaks: a layout written before double-tap hold existed must still load, and a
 * document must never be able to say that a control is currently HELD.
 */
class TouchLatchPersistenceTest {

    private val profile = TouchProfileCatalog.require(TouchProfileId.Pro2)
    private val base get() = authored(profile)

    private fun latchOf(document: TouchLayoutDocument, id: String) =
        requireNotNull(document.instance(id)).latch

    @Test fun `the editor writes the choice and Default erases it`() {
        val disabled = TouchLayoutEditor.setLatch(
            base, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = false, editGroup = false,
        )
        assertEquals(false, latchOf(disabled, TouchLayoutV1.FACE_SOUTH))

        val restored = TouchLayoutEditor.setLatch(
            disabled, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = null, editGroup = false,
        )
        // Not stored as "false": the answer goes back to null, so the control
        // follows the global setting again instead of freezing today's value.
        assertNull(latchOf(restored, TouchLayoutV1.FACE_SOUTH))
        assertEquals(base, restored)
    }

    @Test fun `a group edit gives each control its own answer`() {
        val grouped = TouchLayoutEditor.setLatch(
            base, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = true, editGroup = true,
        )
        val faces = setOf(
            TouchLayoutV1.FACE_NORTH, TouchLayoutV1.FACE_SOUTH,
            TouchLayoutV1.FACE_EAST, TouchLayoutV1.FACE_WEST,
        )
        assertTrue(faces.all { latchOf(grouped, it) == true })
        assertTrue(grouped.controls.filterNot { it.instanceId in faces }.all { it.latch == null })

        val one = TouchLayoutEditor.setLatch(
            grouped, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = false, editGroup = false,
        )
        assertEquals(false, latchOf(one, TouchLayoutV1.FACE_SOUTH))
        assertEquals(true, latchOf(one, TouchLayoutV1.FACE_NORTH))
    }

    @Test fun `a control that cannot latch never stores an opinion about it`() {
        val attempted = TouchLayoutEditor.setLatch(
            base, profile, setOf(TouchLayoutV1.STICK_LEFT), latch = true, editGroup = false,
        )
        assertEquals(base, attempted)
    }

    @Test fun `two instances of one binding hold independently`() {
        val (document, duplicate) = withDuplicate(profile, TouchLayoutV1.FACE_SOUTH, 0.5f, 0.7f)
        val chosen = TouchLayoutEditor.setLatch(
            document, profile, setOf(duplicate), latch = false, editGroup = false,
        )
        // The point of instance-scoped behaviour: one A may hold and the other
        // may not, because they are separate objects rather than one binding.
        assertEquals(false, latchOf(chosen, duplicate))
        assertNull(latchOf(chosen, TouchLayoutV1.FACE_SOUTH))
    }

    @Test fun `the choice reaches the composed control the engine reads`() {
        val document = TouchLayoutEditor.setLatch(
            base, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = false, editGroup = false,
        )
        val composed = TouchLayoutComposer.compose(profile, document).layout
        assertEquals(false, composed.controls.first { it.id == TouchLayoutV1.FACE_SOUTH }.latch)
        // Everything else stays on "follow the global setting".
        assertTrue(composed.controls.filter { it.id != TouchLayoutV1.FACE_SOUTH }.all { it.latch == null })
    }

    @Test fun `the choice survives a round trip through the document`() {
        val document = TouchLayoutEditor.setLatch(
            TouchLayoutEditor.setLatch(
                base, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = false, editGroup = false,
            ),
            profile,
            setOf(TouchLayoutV1.SHOULDER_RIGHT),
            latch = true,
            editGroup = false,
        )
        val library = TouchProfileLibrary(
            personality = TouchProfileId.Pro2,
            userProfiles = listOf(TouchLayoutProfile("p1", "Mine", document)),
            selectedProfileId = "p1",
        )
        val decoded = TouchProfileLibraryJsonCodec.decode(
            TouchProfileLibraryJsonCodec.encode(library),
            TouchProfileId.Pro2,
        )
        assertEquals(
            document,
            (decoded as TouchProfileLibraryDecodeResult.Valid).value.activeDocument,
        )
    }

    /**
     * The compatibility guarantee. A document written before this feature has no
     * `latch` key at all, and must load unchanged — same schema version, same
     * geometry, every control on the global default.
     */
    @Test fun `a layout written before the feature still loads`() {
        val legacy = """
            {
              "schemaVersion": 1,
              "profileId": "pro2",
              "templateId": "${profile.defaultTemplate.id}",
              "basedOnRevision": ${profile.defaultTemplate.templateRevision},
              "controls": { "${TouchLayoutV1.FACE_SOUTH}": { "scale": 1.2, "visible": false } }
            }
        """.trimIndent()
        val decoded = TouchLayoutOverrideJsonCodec.decode(legacy)
        val value = (decoded as TouchOverrideDecodeResult.Valid).value
        assertEquals(TouchLayoutOverride.CURRENT_SCHEMA_VERSION, value.schemaVersion)
        val control = value.controls.getValue(TouchLayoutV1.FACE_SOUTH)
        assertEquals(1.2f, control.scale)
        assertEquals(false, control.visible)
        assertNull(control.latch)
    }

    /**
     * Adding the field did NOT bump the retired schema, on purpose: a bump would
     * have made every older build refuse the whole document ("written by a newer
     * app") and throw away geometry the user spent time on, to protect one
     * optional preference that degrades to its default anyway. Pinned because
     * the schema-1 decoder still has to read exactly what was written.
     */
    @Test fun `adding the field did not change the retired schema version`() {
        assertEquals(1, TouchLayoutOverride.CURRENT_SCHEMA_VERSION)
    }

    @Test fun `a non-boolean latch is refused rather than guessed at`() {
        val bad = """
            {
              "schemaVersion": 1,
              "profileId": "pro2",
              "templateId": "${profile.defaultTemplate.id}",
              "basedOnRevision": ${profile.defaultTemplate.templateRevision},
              "controls": { "${TouchLayoutV1.FACE_SOUTH}": { "latch": "yes please" } }
            }
        """.trimIndent()
        assertTrue(TouchLayoutOverrideJsonCodec.decode(bad) is TouchOverrideDecodeResult.Invalid)
    }

    /** Configuration is persisted; what is currently HELD is not, and cannot be. */
    @Test fun `no persisted field can say a control is currently held`() {
        val document = TouchLayoutEditor.setLatch(
            base, profile, setOf(TouchLayoutV1.FACE_SOUTH), latch = true, editGroup = false,
        )
        val encoded = TouchProfileLibraryJsonCodec.encode(
            TouchProfileLibrary(
                personality = TouchProfileId.Pro2,
                userProfiles = listOf(TouchLayoutProfile("p1", "Mine", document)),
                selectedProfileId = "p1",
            ),
        )
        assertFalse("latched" in encoded)
        assertFalse("pressed" in encoded)
        assertFalse("retrigger" in encoded)
    }
}

private const val MS = 1_000_000L
private const val SECOND = 1_000L * MS

/** One contact id, reused: a tap sequence is one finger arriving twice. */
private const val CONTACT = 4_242L
