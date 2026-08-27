package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerState
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The hold gestures observed where they actually matter: in the sequence of
 * published [ControllerState] snapshots.
 *
 * `TouchLatchTest` pins the recognizer. This suite deliberately does NOT look at
 * `latched` or any other internal flag except to prove that a boundary cleared
 * it — it drives a real [TouchGamepad] over a real [ControllerInputState] and
 * asserts the EDGES a console would see. The retrigger in particular is a claim
 * about output, not about state: "the hold is briefly masked" is worth nothing
 * if a released snapshot never reaches the state machine, and only this layer
 * can say whether one did.
 *
 * Collected on an unconfined dispatcher so every distinct snapshot is captured;
 * a conflating collector would silently drop exactly the transient release edge
 * these tests exist to observe.
 *
 * `SHOULDER_LEFT` throughout, because it binds to a LOGICAL button (`L1`) that
 * no face-layout presentation can rewrite — the edges under test are then the
 * only thing that can move.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class TouchLatchOutputTest {

    private lateinit var input: ControllerInputState
    private lateinit var gamepad: TouchGamepad
    private lateinit var resolved: ResolvedTouchLayout
    private val states = mutableListOf<ControllerState>()

    private val config get() = gamepad.config.latch

    /** Whether the console would currently see the control down. */
    private val held: List<Boolean> get() = states.map { ControllerButton.L1 in it.buttons }

    private fun start(scope: kotlinx.coroutines.CoroutineScope) {
        input = ControllerInputState()
        gamepad = TouchGamepad(input)
        scope.launch(UnconfinedTestDispatcher()) { input.state.collect { states += it } }
        gamepad.activate()
        resolved = TouchLayoutResolver.resolve(
            TouchLayoutV1.layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        gamepad.setLayout(resolved)
        states.clear()
    }

    // ------------------------------------------------------------ ordinary input

    @Test fun `an ordinary tap is one press edge and one release edge`() = runTest {
        start(backgroundScope)
        tap(at = MS)
        assertEquals(listOf(true, false), held)
        assertNull("nothing timed is left over", gamepad.nextDeadlineNanos())
    }

    @Test fun `rapid mashing is nothing but press and release edges`() = runTest {
        start(backgroundScope)
        var time = MS
        repeat(10) {
            down(time)
            time += 40 * MS
            advanceTo(time)
            up(time)
            time += 70 * MS
            advanceTo(time)
        }
        assertEquals(
            "twenty alternating edges, and no hold",
            List(20) { it % 2 == 0 },
            held,
        )
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    /**
     * The gap the old plain-double-tap gesture fell into. Two taps inside the
     * double-tap window are still two taps; only the dwell promotes them.
     */
    @Test fun `two quick taps inside the double-tap window do not latch`() = runTest {
        start(backgroundScope)
        tap(at = MS)
        tap(at = MS + 150 * MS)
        advanceTo(MS + 5 * SECOND)
        assertEquals(listOf(true, false, true, false), held)
    }

    // ------------------------------------------------------------------- engage

    @Test fun `the engage gesture leaves the button pressed after the finger lifts`() = runTest {
        start(backgroundScope)
        val firstTapEnd = tap(at = MS)
        val second = firstTapEnd + 120 * MS

        down(second)
        assertEquals("the second press is an ordinary press", listOf(true, false, true), held)
        assertEquals(
            "and the dwell is twice the base",
            second + 2 * config.holdThresholdNanos,
            gamepad.nextDeadlineNanos(),
        )

        advanceTo(second + config.latchEngageThresholdNanos)
        slide(commitDistance() * 1.2f, second + config.latchEngageThresholdNanos + 20 * MS)
        up(second + config.latchEngageThresholdNanos + 40 * MS)
        advanceTo(second + 5 * SECOND)

        assertEquals(
            "neither arming, committing nor lifting produced an edge of its own",
            listOf(true, false, true),
            held,
        )
        assertEquals(0, gamepad.diagnostics().activeContacts)
    }

    /**
     * The gesture the arm-then-slide model exists to protect: holding a button
     * after double-tapping it is ordinary gameplay, and must stay ordinary
     * however long it is held.
     */
    @Test fun `a double tap held indefinitely is two presses and one release`() = runTest {
        start(backgroundScope)
        val second = armEngage(at = MS)
        advanceTo(second + 30 * SECOND)
        assertEquals(listOf(true, false, true), held)
        up(second + 30 * SECOND)
        assertEquals(listOf(true, false, true, false), held)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    @Test fun `releasing while armed but before sliding is an ordinary release`() = runTest {
        start(backgroundScope)
        val second = armEngage(at = MS)
        up(second + SECOND)
        advanceTo(second + 5 * SECOND)
        assertEquals(listOf(true, false, true, false), held)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    @Test fun `a slide short of the commit distance is just another tap`() = runTest {
        start(backgroundScope)
        val second = armEngage(at = MS)
        slide(commitDistance() * 0.9f, second + SECOND)
        up(second + 2 * SECOND)
        advanceTo(second + 5 * SECOND)
        assertEquals(listOf(true, false, true, false), held)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    @Test fun `a second press released before the engage dwell is just another tap`() = runTest {
        start(backgroundScope)
        val firstTapEnd = tap(at = MS)
        val second = firstTapEnd + 120 * MS
        down(second)
        // Long enough to have released a hold, nowhere near long enough to make one.
        val justShort = second + config.latchEngageThresholdNanos - MS
        advanceTo(justShort)
        up(justShort)
        advanceTo(justShort + 5 * SECOND)
        assertEquals(listOf(true, false, true, false), held)
    }

    // ---------------------------------------------------------------- retrigger

    @Test fun `a quick tap on a held button is a release edge then a fresh press edge`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        states.clear()

        val tapAt = 10 * SECOND
        down(tapAt)
        assertEquals(
            "the decision window must not disturb the hold",
            emptyList<Boolean>(),
            held,
        )

        up(tapAt + 40 * MS)
        assertEquals("the release edge lands when the tap ends", listOf(false), held)

        advanceTo(tapAt + 40 * MS + config.retriggerReleaseNanos)
        assertEquals("and the hold comes straight back", listOf(false, true), held)
        assertEquals(setOf(TouchLayoutV1.SHOULDER_LEFT), gamepad.engine.latchedControlIds())
    }

    @Test fun `repeated quick taps repeat the pulse and the hold survives all of them`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        states.clear()

        var time = 10 * SECOND
        repeat(3) {
            down(time)
            up(time + 40 * MS)
            time += 200 * MS
            advanceTo(time)
            assertEquals(
                "the hold must never be lost between pulses",
                setOf(TouchLayoutV1.SHOULDER_LEFT),
                gamepad.engine.latchedControlIds(),
            )
        }
        assertEquals(listOf(false, true, false, true, false, true), held)
        assertEquals(3, gamepad.diagnostics().retriggerPulses)
    }

    /**
     * The reason the pulse waits for the release. Starting it on the way down
     * would make every unlatch emit a pointless release/press first.
     */
    @Test fun `the unlatch gesture emits no retrigger pulse on its way down`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        states.clear()

        val pressAt = 10 * SECOND
        down(pressAt)
        advanceTo(pressAt + config.latchReleaseThresholdNanos)
        assertEquals(
            "no edge at all: the hold ended but the finger is still on it",
            emptyList<Boolean>(),
            held,
        )
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertEquals(0, gamepad.diagnostics().retriggerPulses)
    }

    // ------------------------------------------------------------------ release

    @Test fun `the release gesture is half the engage dwell and needs no leading tap`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        states.clear()

        val pressAt = 10 * SECOND
        down(pressAt)
        assertEquals(
            "one press is the whole gesture, and it is due at the base threshold",
            pressAt + config.holdThresholdNanos,
            gamepad.nextDeadlineNanos(),
        )
        assertEquals(
            2L,
            config.latchEngageThresholdNanos / config.latchReleaseThresholdNanos,
        )

        advanceTo(pressAt + config.latchReleaseThresholdNanos)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertEquals("the physical finger is still authoritative", emptyList<Boolean>(), held)

        up(pressAt + config.latchReleaseThresholdNanos + 60 * MS)
        assertEquals("the button releases when, and only when, the finger lifts", listOf(false), held)
        advanceTo(pressAt + 5 * SECOND)
        assertEquals(listOf(false), held)
    }

    @Test fun `after unlatching, the control is an ordinary button again`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        val pressAt = 10 * SECOND
        down(pressAt)
        advanceTo(pressAt + config.latchReleaseThresholdNanos)
        up(pressAt + config.latchReleaseThresholdNanos + 60 * MS)
        advanceTo(pressAt + 5 * SECOND)
        states.clear()

        tap(at = 20 * SECOND)
        assertEquals(listOf(true, false), held)
        assertEquals(
            "and no pulse, because there is nothing being held any more",
            0,
            gamepad.diagnostics().retriggerPulses,
        )
    }

    // -------------------------------------------------------------- mash safety

    @Test fun `mashing a held button retriggers it and never releases the hold`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        states.clear()

        var time = 10 * SECOND
        repeat(12) {
            down(time)
            time += 40 * MS
            up(time)
            time += 70 * MS
            advanceTo(time)
        }
        assertEquals(
            "still held, because no mashed press outlives even the shorter dwell",
            setOf(TouchLayoutV1.SHOULDER_LEFT),
            gamepad.engine.latchedControlIds(),
        )
        assertEquals(12, gamepad.diagnostics().retriggerPulses)
        assertEquals(List(24) { it % 2 == 1 }, held)
        assertTrue("and it ends up held", held.last())
    }

    // ----------------------------------------------------------------- teardown

    @Test fun `a teardown during a pending pulse converges to neutral`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        val tapAt = 10 * SECOND
        down(tapAt)
        up(tapAt + 40 * MS)
        assertTrue("a reassertion is genuinely pending", gamepad.nextDeadlineNanos() != null)

        gamepad.release(TouchReleaseReason.LinkEnded)
        states.clear()

        gamepad.tick(tapAt + 10 * SECOND)
        assertEquals("nothing may be republished after teardown", emptyList<Boolean>(), held)
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertNull(gamepad.nextDeadlineNanos())
    }

    @Test fun `a teardown during a pending engage dwell cannot latch afterwards`() = runTest {
        start(backgroundScope)
        val firstTapEnd = tap(at = MS)
        val second = firstTapEnd + 120 * MS
        down(second)
        gamepad.release(TouchReleaseReason.ModeExit)
        states.clear()
        slide(commitDistance() * 2f, second + config.latchEngageThresholdNanos)

        gamepad.tick(second + config.latchEngageThresholdNanos)
        gamepad.tick(second + 10 * SECOND)
        assertEquals(emptyList<Boolean>(), held)
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    @Test fun `a teardown while armed cannot be completed by a later slide`() = runTest {
        start(backgroundScope)
        val second = armEngage(at = MS)
        assertEquals(setOf(TouchLayoutV1.SHOULDER_LEFT), gamepad.engine.armedControlIds())

        gamepad.release(TouchReleaseReason.LinkEnded)
        states.clear()

        slide(commitDistance() * 2f, second + SECOND)
        gamepad.tick(second + 10 * SECOND)
        assertEquals(emptyList<Boolean>(), held)
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    @Test fun `a teardown during a pending release dwell leaves nothing held`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        down(10 * SECOND)
        gamepad.release(TouchReleaseReason.AuthorityChanged)
        states.clear()

        gamepad.tick(10 * SECOND + config.latchReleaseThresholdNanos)
        gamepad.tick(20 * SECOND)
        assertEquals(emptyList<Boolean>(), held)
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    /** Deactivating drops authority, which must not strand a hold on the way out. */
    @Test fun `leaving the mode releases a held control through the live state machine`() = runTest {
        start(backgroundScope)
        latch(at = MS)
        assertTrue(held.last())

        gamepad.deactivate()
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
    }

    // ------------------------------------------------------------------ helpers

    /** The host's tick driver: ask for the deadline, tick at exactly that instant, repeat. */
    // ------------------------------------------------------------------- cancel

    /**
     * The slide is reversible until the finger lifts.
     *
     * Committing on the way out and cancelling on the way back is the same
     * continuous motion, and it removes the one unforgiving moment in this
     * gesture: a slide that had passed the commit distance used to be final, so
     * a user who changed their mind had to lift off and then press and hold the
     * control to undo a hold they never wanted.
     */
    @Test fun `sliding back to the origin cancels the hold it just made`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val settled = second + config.latchEngageThresholdNanos

        slide(commitDistance() * 1.2f, settled + 20 * MS)
        assertEquals("out past the commit distance locks it", latched, engagedIds())
        assertEquals(1, gamepad.diagnostics().latchesEngaged)

        // Back toward the origin, but still outside the cancel radius: the hold
        // is untouched. Halfway is deliberately inside the BAND, which is the
        // whole reason the band exists.
        slide(cancelDistance() * 1.8f, settled + 30 * MS)
        assertEquals("still held in the band", latched, engagedIds())
        assertEquals(0, gamepad.diagnostics().latchesCancelled)

        // Inside the radius: the hold comes off.
        slide(cancelDistance() * 0.5f, settled + 40 * MS)
        assertEquals("back home cancels it", emptySet<String>(), engagedIds())
        assertEquals(1, gamepad.diagnostics().latchesCancelled)
        assertEquals(
            "and the control is offered again rather than simply dropped",
            latched,
            gamepad.engine.armedControlIds(),
        )
    }

    /**
     * The console must not be able to tell that any of it happened while the
     * finger is down, and the release afterwards has to be an ordinary one.
     */
    @Test fun `a cancelled gesture releases like an ordinary press`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val settled = second + config.latchEngageThresholdNanos
        slide(commitDistance() * 1.2f, settled + 20 * MS)
        slide(cancelDistance() * 0.5f, settled + 40 * MS)

        assertEquals(
            "no edge from committing, and none from cancelling",
            listOf(true, false, true),
            held,
        )
        up(settled + 60 * MS)
        advanceTo(settled + 5 * SECOND)

        assertEquals("one release, at the finger's own lift", listOf(true, false, true, false), held)
        assertEquals(emptySet<String>(), gamepad.engine.latchedControlIds())
        assertNull("nothing timed is left over", gamepad.nextDeadlineNanos())
    }

    /**
     * The other half of the hysteresis: cancelling returns the contact to ARMED,
     * so a user who overshot can simply slide out again without lifting off.
     */
    @Test fun `sliding out again after a cancel locks it once more`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val settled = second + config.latchEngageThresholdNanos
        slide(commitDistance() * 1.2f, settled + 20 * MS)
        slide(cancelDistance() * 0.5f, settled + 40 * MS)
        assertEquals(emptySet<String>(), engagedIds())

        slide(commitDistance() * 1.2f, settled + 60 * MS)
        assertEquals(latched, engagedIds())
        assertEquals(2, gamepad.diagnostics().latchesEngaged)
        assertEquals(1, gamepad.diagnostics().latchesCancelled)

        up(settled + 80 * MS)
        advanceTo(settled + 5 * SECOND)
        assertEquals("and it outlives the finger", latched, gamepad.engine.latchedControlIds())
    }

    /**
     * The reason there are two distances and not one.
     *
     * A thumb parked on a single threshold produces a stream of lock/unlock
     * transitions, and every one of them is a real change to what the console is
     * told once the finger lifts. Here the contact crosses the COMMIT distance
     * repeatedly and never comes near the cancel radius, so nothing after the
     * first crossing may change at all.
     */
    @Test fun `jitter across the commit distance cannot flap the hold`() = runTest {
        start(backgroundScope)
        val second = armEngage(MS)
        val settled = second + config.latchEngageThresholdNanos
        slide(commitDistance() * 1.02f, settled + 20 * MS)
        assertEquals(latched, engagedIds())

        var time = settled + 30 * MS
        listOf(0.98f, 1.03f, 0.95f, 1.10f, 0.90f, 1.01f, 0.99f).forEach { fraction ->
            slide(commitDistance() * fraction, time)
            time += 10 * MS
            assertEquals("at ${fraction}x the commit distance", latched, engagedIds())
        }
        assertEquals("locked exactly once", 1, gamepad.diagnostics().latchesEngaged)
        assertEquals("and never cancelled", 0, gamepad.diagnostics().latchesCancelled)
        assertEquals("no edges either", listOf(true, false, true), held)
    }

    /**
     * A boundary during the reversible window is the same boundary as any other:
     * everything goes, and nothing pending can bring it back.
     */
    @Test fun `a boundary during a cancellable hold cannot resurrect it`() = runTest {
        TouchReleaseReason.entries.forEach { reason ->
            states.clear()
            start(backgroundScope)
            val second = armEngage(MS)
            val settled = second + config.latchEngageThresholdNanos
            slide(commitDistance() * 1.2f, settled + 20 * MS)
            assertEquals("$reason: locked first", latched, engagedIds())

            gamepad.release(reason)
            assertEquals("$reason left it held", emptySet<String>(), engagedIds())
            assertEquals("$reason left it pressed", false, held.last())

            // The finger is still on the glass as far as the platform knows, so
            // the rest of the gesture still arrives. None of it may bring the
            // hold back.
            slide(cancelDistance() * 0.5f, settled + 40 * MS)
            slide(commitDistance() * 1.4f, settled + 60 * MS)
            up(settled + 80 * MS)
            advanceTo(settled + 5 * SECOND)

            assertEquals("$reason: still nothing held", emptySet<String>(), engagedIds())
            assertEquals("$reason: still nothing pressed", false, held.last())
            assertNull("$reason left timed work behind", gamepad.nextDeadlineNanos())
        }
    }

    /** The one control this suite drives, as a set, for the assertions above. */
    private val latched = setOf(TouchLayoutV1.SHOULDER_LEFT)

    private fun engagedIds() = gamepad.engine.latchedControlIds()

    private fun cancelDistance(): Float =
        config.latchCancelDistanceUnits * resolved.region.unitScale

    private fun advanceTo(nowNanos: Long) {
        while (true) {
            val deadline = gamepad.nextDeadlineNanos() ?: return
            if (deadline > nowNanos) return
            gamepad.tick(deadline)
        }
    }

    /** Tap, hold the second press past the dwell, slide away, then lift. */
    private fun latch(at: Long) {
        val second = armEngage(at)
        val settled = second + config.latchEngageThresholdNanos
        slide(commitDistance() * 1.2f, settled + 20 * MS)
        up(settled + 40 * MS)
        advanceTo(settled + SECOND)
        check(TouchLayoutV1.SHOULDER_LEFT in gamepad.engine.latchedControlIds()) { "failed to latch" }
    }

    /** Tap, then hold past the dwell, leaving the contact down and armed. */
    private fun armEngage(at: Long): Long {
        val firstTapEnd = tap(at)
        val second = firstTapEnd + 120 * MS
        down(second)
        advanceTo(second + config.latchEngageThresholdNanos)
        return second
    }

    private fun commitDistance(): Float =
        config.latchCommitDistanceUnits * resolved.region.unitScale

    private fun slide(by: Float, at: Long) {
        val target = resolved.control(TouchLayoutV1.SHOULDER_LEFT)!!
        gamepad.engine.onContact(
            TouchContact(7L, TouchPhase.Move, target.centerX + by, target.centerY, at),
        )
    }

    private fun tap(at: Long): Long {
        down(at)
        val end = at + 40 * MS
        up(end)
        return end
    }

    private fun down(at: Long) = contact(TouchPhase.Down, at)

    private fun up(at: Long) = contact(TouchPhase.Up, at)

    private fun contact(phase: TouchPhase, at: Long) {
        val target = resolved.control(TouchLayoutV1.SHOULDER_LEFT)!!
        gamepad.engine.onContact(TouchContact(7L, phase, target.centerX, target.centerY, at))
    }
}

private const val MS = 1_000_000L
private const val SECOND = 1_000L * MS
