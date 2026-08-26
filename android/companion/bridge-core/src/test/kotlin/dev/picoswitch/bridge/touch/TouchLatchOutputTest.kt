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
