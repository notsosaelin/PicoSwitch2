package dev.picoswitch.companion.data

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The adapter switch, which is the one place in this feature where getting the
 * ORDER wrong is silently wrong rather than loudly wrong.
 *
 * The property under test is stated once and pinned from several directions:
 *
 * > A switch from adapter A to adapter B is one generation-owned transition. A
 * > is retired completely before B becomes authoritative. No stale event from A
 * > can update B's UI or lifecycle. If B fails, the app ends in a truthful
 * > selected-but-disconnected state for B, and never falls back to A.
 *
 * The port is faked because none of that needs Bluetooth to be true — and
 * because a test that needed a radio would not be run.
 */
class AdapterSwitchTest {
    private val a = AdapterId.fromAddress("88:A2:9E:D1:77:78")!!
    private val b = AdapterId.fromAddress("88:A2:9E:D1:77:79")!!
    private val c = AdapterId.fromAddress("88:A2:9E:D1:77:7A")!!

    /** Records every step in the order it happened, so ordering can be asserted directly. */
    private class RecordingPort : AdapterSwitchPort {
        val steps = mutableListOf<String>()
        /** When set, retirement suspends here until completed, standing in for a slow teardown. */
        var retirementGate: CompletableDeferred<Unit>? = null
        var onRetiring: (() -> Unit)? = null

        override fun selectionCommitted(target: AdapterId, previous: AdapterId?) {
            steps += "select(${target.shortLabel},from=${previous?.shortLabel ?: "none"})"
        }

        override suspend fun stopControllerLink(previous: AdapterId) {
            steps += "stopLink(${previous.shortLabel})"
        }

        override suspend fun retireManagement(previous: AdapterId) {
            steps += "retireStart(${previous.shortLabel})"
            onRetiring?.invoke()
            retirementGate?.await()
            steps += "retireDone(${previous.shortLabel})"
        }

        override fun clearAdapterScopedState() {
            steps += "clear"
        }

        override fun beginActivation(target: AdapterId) {
            steps += "activate(${target.shortLabel})"
        }
    }

    private fun fixture(active: AdapterId? = null): Pair<ActiveAdapterCoordinator, RecordingPort> {
        val coordinator = ActiveAdapterCoordinator(active)
        return coordinator to RecordingPort()
    }

    @Test fun `the outgoing adapter is fully retired before the incoming one is activated`() = runTest {
        val (coordinator, port) = fixture(active = a)
        val outcome = AdapterSwitch(coordinator, port).switchTo(b)

        assertTrue(outcome is SwitchOutcome.Activating)
        assertEquals(
            listOf(
                "select(7779,from=7778)",
                "stopLink(7778)",
                "retireStart(7778)",
                "retireDone(7778)",
                "clear",
                "activate(7779)",
            ),
            port.steps,
        )
    }

    @Test fun `the selection is committed before anything is torn down`() = runTest {
        val (coordinator, port) = fixture(active = a)
        AdapterSwitch(coordinator, port).switchTo(b)
        // The user asked for B; the UI must be able to say so immediately rather
        // than showing A as selected while A is being dismantled.
        assertEquals(0, port.steps.indexOfFirst { it.startsWith("select(") })
    }

    @Test fun `the incoming adapter is selected from the moment the switch begins`() = runTest {
        val (coordinator, port) = fixture(active = a)
        port.retirementGate = CompletableDeferred()
        val running = async { AdapterSwitch(coordinator, port).switchTo(b) }
        // Let the switch run up to the retirement gate; runTest queues children
        // rather than starting them eagerly.
        runCurrent()
        // Mid-retirement: B is already the selection, A is the one leaving.
        assertEquals(b, coordinator.state.activeId)
        assertEquals(a, coordinator.state.retiringId)
        assertEquals(SwitchPhase.Retiring, coordinator.state.phase)
        port.retirementGate!!.complete(Unit)
        running.await()
        assertEquals(SwitchPhase.Activating, coordinator.state.phase)
        assertNull(coordinator.state.retiringId)
    }

    @Test fun `nothing from the outgoing adapter is accepted while it is retiring`() = runTest {
        val (coordinator, port) = fixture(active = a)
        port.retirementGate = CompletableDeferred()
        val running = async { AdapterSwitch(coordinator, port).switchTo(b) }
        // Let the switch run up to the retirement gate; runTest queues children
        // rather than starting them eagerly.
        runCurrent()

        // This is the stale-callback case in its most dangerous form: A is still
        // emitting, B is already the selection.
        assertFalse(coordinator.accepts(a.value))
        assertFalse(coordinator.accepts(b.value))
        assertFalse(coordinator.accepts(null))

        port.retirementGate!!.complete(Unit)
        running.await()

        // Once A is gone, B's own progress is accepted and A's is not.
        assertTrue(coordinator.accepts(b.value))
        assertFalse(coordinator.accepts(a.value))
        // Scan and idle states carry no device and are B's progress by then,
        // which is only safe because retirement was awaited.
        assertTrue(coordinator.accepts(null))
    }

    @Test fun `a failed activation leaves the chosen adapter selected and disconnected`() = runTest {
        val (coordinator, port) = fixture(active = a)
        AdapterSwitch(coordinator, port).switchTo(b)

        assertTrue(coordinator.activationFailed(b, "The adapter did not respond"))

        assertEquals(b, coordinator.state.activeId)
        assertFalse(coordinator.state.connected)
        assertEquals("The adapter did not respond", coordinator.state.failure)
        assertEquals(SwitchPhase.Settled, coordinator.state.phase)
        // The property that matters: no silent fallback. A was not reconnected
        // and was not re-selected.
        assertFalse(port.steps.any { it.contains("activate(7778)") })
    }

    @Test fun `a result for the abandoned adapter cannot settle the chosen one`() = runTest {
        val (coordinator, port) = fixture(active = a)
        AdapterSwitch(coordinator, port).switchTo(b)

        // A's connect job unwinding late and reporting its own outcome.
        assertFalse(coordinator.activationSucceeded(a))
        assertFalse(coordinator.activationFailed(a, "A failed"))

        assertEquals(b, coordinator.state.activeId)
        assertFalse(coordinator.state.connected)
        assertNull(coordinator.state.failure)
        assertEquals(SwitchPhase.Activating, coordinator.state.phase)
    }

    @Test fun `A to B to A leaves one settled connected adapter each time`() = runTest {
        val (coordinator, port) = fixture(active = a)
        val switch = AdapterSwitch(coordinator, port)
        coordinator.activationSucceeded(a)

        repeat(3) {
            assertTrue(switch.switchTo(b) is SwitchOutcome.Activating)
            assertTrue(coordinator.activationSucceeded(b))
            assertEquals(b, coordinator.state.activeId)
            assertTrue(coordinator.state.connected)
            assertEquals(SwitchPhase.Settled, coordinator.state.phase)

            assertTrue(switch.switchTo(a) is SwitchOutcome.Activating)
            assertTrue(coordinator.activationSucceeded(a))
            assertEquals(a, coordinator.state.activeId)
            assertTrue(coordinator.state.connected)
            assertEquals(SwitchPhase.Settled, coordinator.state.phase)
        }

        // Six switches, six retirements, six activations. Never two activations
        // without a retirement between them.
        assertEquals(6, port.steps.count { it.startsWith("retireDone(") })
        assertEquals(6, port.steps.count { it.startsWith("activate(") })
        val order = port.steps.filter { it.startsWith("retireDone(") || it.startsWith("activate(") }
        order.chunked(2).forEach { pair ->
            assertTrue("expected retire then activate, got $pair", pair[0].startsWith("retireDone("))
            assertTrue("expected retire then activate, got $pair", pair[1].startsWith("activate("))
        }
    }

    @Test fun `selecting the adapter that is already active and settled does nothing`() = runTest {
        val (coordinator, port) = fixture(active = a)
        coordinator.activationSucceeded(a)
        assertEquals(SwitchOutcome.AlreadyActive, AdapterSwitch(coordinator, port).switchTo(a))
        // A healthy session must not be torn down by a repeated tap on its row.
        assertTrue(port.steps.isEmpty())
    }

    @Test fun `re-selecting the active adapter after a failure does retry it`() = runTest {
        val (coordinator, port) = fixture(active = a)
        AdapterSwitch(coordinator, port).switchTo(b)
        coordinator.activationFailed(b, "no response")
        port.steps.clear()
        // Settled-but-disconnected is not "already active" in the sense that
        // should block a retry; the user tapping B again must reach B.
        assertTrue(AdapterSwitch(coordinator, port).switchTo(b) is SwitchOutcome.Activating)
        assertTrue(port.steps.contains("activate(7779)"))
    }

    @Test fun `a switch overtaken during retirement stops without activating`() = runTest {
        val (coordinator, port) = fixture(active = a)
        // A newer switch begins while the first is still tearing A down.
        port.onRetiring = { coordinator.begin(c) }

        val outcome = AdapterSwitch(coordinator, port).switchTo(b)

        assertTrue("expected Superseded, got $outcome", outcome is SwitchOutcome.Superseded)
        assertFalse(port.steps.any { it.startsWith("activate(") })
        // The newer switch owns the selection.
        assertEquals(c, coordinator.state.activeId)
    }

    @Test fun `the first ever selection has nothing to retire`() = runTest {
        val (coordinator, port) = fixture(active = null)
        assertTrue(AdapterSwitch(coordinator, port).switchTo(a) is SwitchOutcome.Activating)
        assertEquals(listOf("select(7778,from=none)", "clear", "activate(7778)"), port.steps)
        assertEquals(SwitchPhase.Activating, coordinator.state.phase)
    }

    @Test fun `removing the active adapter kills a transition that targeted it`() = runTest {
        val (coordinator, _) = fixture(active = a)
        val plan = coordinator.begin(b)!!
        coordinator.cleared()
        assertNull(coordinator.state.activeId)
        // The in-flight switch can no longer complete against a deleted adapter.
        assertFalse(coordinator.retirementComplete(plan.generation))
        assertFalse(coordinator.activationSucceeded(b))
    }
}

/** Decision-level behaviour of the active-adapter authority, apart from the switch that drives it. */
class ActiveAdapterCoordinatorTest {
    private val a = AdapterId.fromAddress("88:A2:9E:D1:77:78")!!
    private val b = AdapterId.fromAddress("88:A2:9E:D1:77:79")!!

    @Test fun `every switch advances the generation exactly once`() {
        val coordinator = ActiveAdapterCoordinator(a)
        val first = coordinator.begin(b)!!
        val second = coordinator.begin(a)!!
        assertEquals(first.generation + 1, second.generation)
        // The first switch's remaining steps are now inert.
        assertFalse(coordinator.retirementComplete(first.generation))
        assertTrue(coordinator.retirementComplete(second.generation))
    }

    @Test fun `retirement cannot be completed twice`() {
        val coordinator = ActiveAdapterCoordinator(a)
        val plan = coordinator.begin(b)!!
        assertTrue(coordinator.retirementComplete(plan.generation))
        assertFalse(coordinator.retirementComplete(plan.generation))
    }

    @Test fun `a settled session that ends is reported as disconnected without losing the selection`() {
        val coordinator = ActiveAdapterCoordinator(a)
        coordinator.activationSucceeded(a)
        assertTrue(coordinator.markDisconnected())
        assertEquals(a, coordinator.state.activeId)
        assertFalse(coordinator.state.connected)
        // Nothing to report the second time.
        assertFalse(coordinator.markDisconnected())
    }

    @Test fun `a disconnect during a transition is expected teardown, not news`() {
        val coordinator = ActiveAdapterCoordinator(a)
        coordinator.activationSucceeded(a)
        coordinator.begin(b)
        assertFalse(coordinator.markDisconnected())
    }

    @Test fun `adopt does not disturb a live transition`() {
        val coordinator = ActiveAdapterCoordinator(a)
        coordinator.begin(b)
        coordinator.adopt(a, connected = true)
        assertEquals(b, coordinator.state.activeId)
        assertFalse(coordinator.state.connected)
    }

    @Test fun `adopt records a first pair when nothing is in flight`() {
        val coordinator = ActiveAdapterCoordinator(null)
        coordinator.adopt(a, connected = true)
        assertEquals(a, coordinator.state.activeId)
        assertTrue(coordinator.state.connected)
    }

    @Test fun `an event from an unknown address is never accepted for the active adapter`() {
        val coordinator = ActiveAdapterCoordinator(a)
        assertTrue(coordinator.accepts(a.value))
        assertFalse(coordinator.accepts(b.value))
        assertFalse(coordinator.accepts("not-an-address"))
    }

    @Test fun `with nothing active only unattributed events are accepted`() {
        val coordinator = ActiveAdapterCoordinator(null)
        assertTrue(coordinator.accepts(null))
        assertFalse(coordinator.accepts(a.value))
    }
}
