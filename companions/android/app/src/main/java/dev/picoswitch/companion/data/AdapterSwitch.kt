package dev.picoswitch.companion.data

/**
 * Everything a switch needs to be able to do.
 *
 * Extracted as a port so the ORDER of a switch is testable without an Android
 * Bluetooth stack. The order is the whole safety property: if activation can
 * begin before retirement has finished, the outgoing adapter's trailing
 * connection states land on the incoming adapter's UI, which is the exact defect
 * this design forbids.
 */
interface AdapterSwitchPort {
    /**
     * The user's choice has taken effect. [target] is the selected adapter from
     * this moment, even though nothing is connected to it yet.
     *
     * Called before any teardown so the UI never shows the outgoing adapter as
     * the selected one while it is being dismantled.
     */
    fun selectionCommitted(target: AdapterId, previous: AdapterId?)

    /**
     * Stop the Controller Link and on-screen controller bound to [previous].
     *
     * A live controller session belongs to the adapter it was established
     * against and is never silently carried across to another one.
     */
    suspend fun stopControllerLink(previous: AdapterId)

    /**
     * Retire [previous]'s management session completely.
     *
     * Must not return until the session is gone, including any connection
     * attempt still in flight. Everything downstream depends on this being a
     * real wait rather than a request.
     */
    suspend fun retireManagement(previous: AdapterId)

    /** Drop cached state and navigation that belonged to the outgoing adapter. */
    fun clearAdapterScopedState()

    /** Start [target]'s management connection. */
    fun beginActivation(target: AdapterId)
}

/**
 * One generation-owned transition from one adapter to another.
 *
 * ```text
 *   begin(B)                      generation++, B is selected from here
 *     -> selectionCommitted(B, A)
 *     -> stopControllerLink(A)
 *     -> retireManagement(A)      awaited; A is completely gone after this
 *     -> retirementComplete(gen)  false if a newer switch replaced this one
 *     -> clearAdapterScopedState()
 *     -> beginActivation(B)
 * ```
 *
 * The generation is re-checked after every suspension point. A switch that lost
 * the race stops without activating, so two overlapping switches can never both
 * reach [AdapterSwitchPort.beginActivation] and leave two adapters believing
 * they are being connected.
 *
 * The connection OUTCOME is not awaited here. It arrives through the existing
 * relationship-connection path, which reports it to
 * [ActiveAdapterCoordinator.activationSucceeded] or
 * [ActiveAdapterCoordinator.activationFailed]. Restructuring that path to be
 * awaitable would mean rebuilding a hardware-validated connect sequence to gain
 * nothing this ordering does not already guarantee.
 */
class AdapterSwitch(
    private val coordinator: ActiveAdapterCoordinator,
    private val port: AdapterSwitchPort,
) {
    suspend fun switchTo(target: AdapterId): SwitchOutcome {
        val plan = coordinator.begin(target) ?: return SwitchOutcome.AlreadyActive
        port.selectionCommitted(target, plan.previous)

        if (plan.previous != null) {
            port.stopControllerLink(plan.previous)
            port.retireManagement(plan.previous)
            if (!coordinator.retirementComplete(plan.generation)) {
                return SwitchOutcome.Superseded(plan.generation)
            }
        }
        // Re-checked even on the no-retirement path: selectionCommitted is a
        // suspension-free call, but a caller may still have raced another switch
        // in between on a different thread.
        if (coordinator.state.generation != plan.generation) {
            return SwitchOutcome.Superseded(plan.generation)
        }

        port.clearAdapterScopedState()
        port.beginActivation(target)
        return SwitchOutcome.Activating(plan)
    }
}
