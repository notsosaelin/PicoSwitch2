package dev.picoswitch.companion.data

/**
 * Where a switch from one adapter to another currently is.
 *
 * `Retiring` and `Activating` are separate on purpose. The outgoing adapter must
 * be completely gone before the incoming one becomes authoritative, and a phase
 * that covered both would make "is this event from the adapter we are leaving or
 * the one we are joining?" unanswerable at exactly the moment it matters.
 */
enum class SwitchPhase { Settled, Retiring, Activating }

data class ActiveAdapterState(
    /**
     * The adapter the user has chosen. Set the instant a switch begins and NOT
     * reverted if that adapter turns out to be unreachable: the honest report of
     * a failed switch is "the adapter you selected is not connected", never a
     * silent return to the previous one.
     */
    val activeId: AdapterId? = null,
    /** The adapter being torn down, non-null only during [SwitchPhase.Retiring]. */
    val retiringId: AdapterId? = null,
    val generation: Long = 0,
    val phase: SwitchPhase = SwitchPhase.Settled,
    val connected: Boolean = false,
    /** Why the last activation failed, if it did. Cleared when a new switch begins. */
    val failure: String? = null,
) {
    val transitioning: Boolean get() = phase != SwitchPhase.Settled
}

data class SwitchPlan(
    val target: AdapterId,
    /** Null when nothing was active, which is the only case with no retirement. */
    val previous: AdapterId?,
    val generation: Long,
)

sealed interface SwitchOutcome {
    /** The target was already active and settled; nothing was torn down. */
    data object AlreadyActive : SwitchOutcome
    /** The target is selected and its connection has been started. */
    data class Activating(val plan: SwitchPlan) : SwitchOutcome
    /** A newer switch replaced this one; this one stopped without activating. */
    data class Superseded(val generation: Long) : SwitchOutcome
}

/**
 * The single authority on which adapter is active, and on whether an event is
 * allowed to say anything about it.
 *
 * ## The rule this exists to enforce
 *
 * A switch from adapter A to adapter B is **one generation-owned transition**. A
 * is retired completely before B becomes authoritative, so no callback,
 * connection state or snapshot belonging to A can reach B's UI or lifecycle. If
 * B cannot be reached, the app settles into a truthful "B selected, not
 * connected" state; it never falls back to A.
 *
 * That last point is deliberate and is the reason [activationFailed] keeps
 * [ActiveAdapterState.activeId] pointing at the target. A fallback would be a
 * hidden state transition — the user asked for B, and an app that quietly
 * reconnects A while displaying something else is the class of lie this whole
 * subsystem has been built to avoid.
 *
 * ## Why a second coordinator
 *
 * `AdapterRelationshipCoordinator` owns *one attempt* at *one relationship*:
 * association, bonding, connect progression, and the generation that makes a
 * stale attempt inert. It is unchanged and still owns all of that. This owns the
 * layer above it — which adapter that coordinator is currently working on, and
 * the ordered handover between two of them. Folding the two together would put
 * bond progression and adapter selection under one generation counter, so a
 * retry of a connection would look like a change of adapter.
 *
 * Ordering is executed by [AdapterSwitch]; this object only decides.
 */
class ActiveAdapterCoordinator(initialActive: AdapterId? = null) {
    private var current = ActiveAdapterState(activeId = initialActive)

    val state: ActiveAdapterState @Synchronized get() = current

    /**
     * Begin a switch to [target].
     *
     * Returns null only when the target is already active, settled AND
     * connected, so a repeated tap on a healthy session cannot tear it down.
     *
     * Connected is part of that test on purpose. A target that is selected but
     * disconnected — the state a failed activation deliberately leaves behind —
     * must still be reachable by choosing it again, or the truthful failure
     * state would also be a dead end. That switch retires nothing, because the
     * target is already the active adapter, and goes straight to activation.
     *
     * The generation increments here and nowhere else. Any switch already in
     * flight is dead from this moment: its remaining steps will find a
     * generation that is not theirs and stop without activating.
     */
    @Synchronized
    fun begin(target: AdapterId): SwitchPlan? {
        if (current.activeId == target && current.phase == SwitchPhase.Settled && current.connected) return null
        val previous = current.activeId?.takeIf { it != target }
        val generation = current.generation + 1
        current = ActiveAdapterState(
            activeId = target,
            retiringId = previous,
            generation = generation,
            // With nothing to retire there is nothing to wait for, so the switch
            // starts already past the handover.
            phase = if (previous != null) SwitchPhase.Retiring else SwitchPhase.Activating,
            connected = false,
            failure = null,
        )
        return SwitchPlan(target, previous, generation)
    }

    /**
     * The outgoing adapter is gone. Returns false when a newer switch has taken
     * over, which is the caller's signal to stop rather than activate.
     */
    @Synchronized
    fun retirementComplete(generation: Long): Boolean {
        if (generation != current.generation || current.phase != SwitchPhase.Retiring) return false
        current = current.copy(phase = SwitchPhase.Activating, retiringId = null)
        return true
    }

    /**
     * Report a connection outcome, guarded by identity rather than by
     * generation.
     *
     * Identity is the right guard here because the connect path is shared with
     * ordinary reconnects that never involved a switch, and because it makes the
     * dangerous case impossible to express: a result for A cannot settle B.
     */
    @Synchronized
    fun activationSucceeded(adapterId: AdapterId): Boolean {
        if (adapterId != current.activeId) return false
        current = current.copy(
            phase = SwitchPhase.Settled,
            retiringId = null,
            connected = true,
            failure = null,
        )
        return true
    }

    /** Settles at "selected, not connected". [ActiveAdapterState.activeId] is intentionally kept. */
    @Synchronized
    fun activationFailed(adapterId: AdapterId, message: String): Boolean {
        if (adapterId != current.activeId) return false
        current = current.copy(
            phase = SwitchPhase.Settled,
            retiringId = null,
            connected = false,
            failure = message,
        )
        return true
    }

    /** An accepted, settled session ended. Ignored mid-transition, where teardown is expected. */
    @Synchronized
    fun markDisconnected(): Boolean {
        if (current.phase != SwitchPhase.Settled || !current.connected) return false
        current = current.copy(connected = false)
        return true
    }

    /**
     * The active adapter was removed from the app, or the last one was.
     *
     * The generation still advances: anything in flight for the adapter that no
     * longer exists must not be able to complete.
     */
    @Synchronized
    fun cleared() {
        current = ActiveAdapterState(generation = current.generation + 1)
    }

    /** Adopt a selection made outside a switch, such as the registry's load or a verified first pair. */
    @Synchronized
    fun adopt(adapterId: AdapterId?, connected: Boolean = false) {
        if (current.transitioning) return
        current = current.copy(activeId = adapterId, connected = connected && adapterId != null, failure = null)
    }

    /**
     * May an event carrying [address] say anything about the active adapter?
     *
     * Three answers, in order:
     *
     * 1. **During a retirement, no.** Everything arriving then belongs to the
     *    adapter being torn down, and the transition owns what the user sees.
     * 2. **An event with no address is accepted outside a retirement.** The scan
     *    and idle-reset states carry no device, and they are genuine progress
     *    for the adapter being activated. This is safe only because retirement
     *    is awaited: by the time the incoming adapter is being activated, the
     *    outgoing one has already emitted its last state.
     * 3. **Otherwise it must be the active adapter's own address.**
     */
    @Synchronized
    fun accepts(address: String?): Boolean = when {
        current.phase == SwitchPhase.Retiring -> false
        address == null -> true
        else -> AdapterId.fromAddress(address) == current.activeId
    }
}
