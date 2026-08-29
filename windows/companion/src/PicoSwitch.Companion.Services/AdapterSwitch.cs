namespace PicoSwitch.Companion.Services;

/// <summary>
/// Everything a switch needs to be able to do.
///
/// Extracted as a port so the ORDER of a switch is testable without a Bluetooth
/// stack. The order is the whole safety property: if activation can begin before
/// retirement has finished, the outgoing adapter's trailing connection states land
/// on the incoming adapter's UI, which is the exact defect this design forbids.
/// </summary>
public interface IAdapterSwitchPort
{
    /// <summary>
    /// The user's choice has taken effect. <paramref name="target"/> is the
    /// selected adapter from this moment, even though nothing is connected to it
    /// yet.
    ///
    /// Called before any teardown so the UI never shows the outgoing adapter as
    /// the selected one while it is being dismantled.
    /// </summary>
    void SelectionCommitted(AdapterId target, AdapterId? previous);

    /// <summary>
    /// Stop the Controller Link and on-screen controller bound to
    /// <paramref name="previous"/>.
    ///
    /// A live controller session belongs to the adapter it was established
    /// against and is never silently carried across to another one.
    /// </summary>
    Task StopControllerLinkAsync(AdapterId previous);

    /// <summary>
    /// Retire <paramref name="previous"/>'s management session completely.
    ///
    /// Must not return until the session is gone, including any connection attempt
    /// still in flight. Everything downstream depends on this being a real wait
    /// rather than a request.
    /// </summary>
    Task RetireManagementAsync(AdapterId previous);

    /// <summary>Drop cached state and navigation that belonged to the outgoing adapter.</summary>
    void ClearAdapterScopedState();

    /// <summary>Start <paramref name="target"/>'s management connection.</summary>
    void BeginActivation(AdapterId target);
}

/// <summary>
/// One generation-owned transition from one adapter to another.
///
/// <code>
///   Begin(B)                        generation++, B is selected from here
///     -> SelectionCommitted(B, A)
///     -> StopControllerLinkAsync(A)
///     -> RetireManagementAsync(A)   awaited; A is completely gone after this
///     -> RetirementComplete(gen)    false if a newer switch replaced this one
///     -> ClearAdapterScopedState()
///     -> BeginActivation(B)
/// </code>
///
/// The generation is re-checked after every await. A switch that lost the race
/// stops without activating, so two overlapping switches can never both reach
/// <see cref="IAdapterSwitchPort.BeginActivation"/> and leave two adapters
/// believing they are being connected.
///
/// The connection OUTCOME is not awaited here. It arrives through the existing
/// relationship-connection path, which reports it to
/// <see cref="ActiveAdapterCoordinator.ActivationSucceeded"/> or
/// <see cref="ActiveAdapterCoordinator.ActivationFailed"/>. Restructuring that
/// path to be awaitable would mean rebuilding a connect sequence to gain nothing
/// this ordering does not already guarantee.
/// </summary>
public sealed class AdapterSwitch(ActiveAdapterCoordinator coordinator, IAdapterSwitchPort port)
{
    public async Task<SwitchOutcome> SwitchToAsync(AdapterId target)
    {
        if (coordinator.Begin(target) is not { } plan)
        {
            return SwitchOutcome.AlreadyActive.Instance;
        }

        port.SelectionCommitted(target, plan.Previous);

        if (plan.Previous is { } previous)
        {
            await port.StopControllerLinkAsync(previous).ConfigureAwait(false);
            await port.RetireManagementAsync(previous).ConfigureAwait(false);
            if (!coordinator.RetirementComplete(plan.Generation))
            {
                return new SwitchOutcome.Superseded(plan.Generation);
            }
        }

        // Re-checked even on the no-retirement path: SelectionCommitted does not
        // await, but a caller may still have raced another switch in between on a
        // different thread.
        if (coordinator.State.Generation != plan.Generation)
        {
            return new SwitchOutcome.Superseded(plan.Generation);
        }

        port.ClearAdapterScopedState();
        port.BeginActivation(target);
        return new SwitchOutcome.Activating(plan);
    }
}
