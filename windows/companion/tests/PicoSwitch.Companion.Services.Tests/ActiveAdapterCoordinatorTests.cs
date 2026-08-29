using PicoSwitch.Companion.Services;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

public sealed class ActiveAdapterCoordinatorTests
{
    private static readonly AdapterId A = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
    private static readonly AdapterId B = AdapterId.FromAddress("AA:BB:CC:DD:EE:02")!.Value;
    private static readonly AdapterId C = AdapterId.FromAddress("AA:BB:CC:DD:EE:03")!.Value;

    [Fact]
    public void TheFirstSelectionHasNothingToRetire()
    {
        var coordinator = new ActiveAdapterCoordinator();
        var plan = coordinator.Begin(A)!;
        Assert.Null(plan.Previous);
        Assert.Equal(SwitchPhase.Activating, coordinator.State.Phase);
    }

    [Fact]
    public void ASwitchRetiresThePreviousAdapterFirst()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);

        var plan = coordinator.Begin(B)!;
        Assert.Equal(A, plan.Previous);
        Assert.Equal(SwitchPhase.Retiring, coordinator.State.Phase);

        // The selection is B from the instant the switch begins, even though
        // nothing is connected to it yet.
        Assert.Equal(B, coordinator.State.ActiveId);
        Assert.False(coordinator.State.Connected);
    }

    [Fact]
    public void ARepeatedChoiceOfAHealthySessionIsInert()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);
        Assert.Null(coordinator.Begin(A));
    }

    [Fact]
    public void ASelectedButDisconnectedAdapterIsStillReachableByChoosingItAgain()
    {
        // The truthful failure state must not also be a dead end.
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationFailed(A, "unreachable");

        var plan = coordinator.Begin(A);
        Assert.NotNull(plan);
        Assert.Null(plan!.Previous);
        Assert.Equal(SwitchPhase.Activating, coordinator.State.Phase);
    }

    [Fact]
    public void AFailedActivationNeverFallsBackToThePreviousAdapter()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);

        var plan = coordinator.Begin(B)!;
        coordinator.RetirementComplete(plan.Generation);
        coordinator.ActivationFailed(B, "unreachable");

        // The user asked for B. An app that quietly reconnects A while displaying
        // something else is the class of lie this subsystem exists to avoid.
        Assert.Equal(B, coordinator.State.ActiveId);
        Assert.False(coordinator.State.Connected);
        Assert.Equal("unreachable", coordinator.State.Failure);
    }

    [Fact]
    public void ANewerSwitchSupersedesAnOlderRetirement()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);

        var toB = coordinator.Begin(B)!;
        var toC = coordinator.Begin(C)!;

        Assert.False(coordinator.RetirementComplete(toB.Generation));
        Assert.True(coordinator.RetirementComplete(toC.Generation));
        Assert.Equal(C, coordinator.State.ActiveId);
    }

    [Fact]
    public void AResultForOneAdapterCannotSettleAnother()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(B);
        Assert.False(coordinator.ActivationSucceeded(A));
        Assert.False(coordinator.ActivationFailed(A, "stale"));
        Assert.False(coordinator.State.Connected);
        Assert.Null(coordinator.State.Failure);
    }

    [Fact]
    public void NoEventFromTheOutgoingAdapterIsAcceptedWhileRetiring()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);
        coordinator.Begin(B);

        Assert.False(coordinator.Accepts(A.Value));
        Assert.False(coordinator.Accepts(B.Value));

        // Even an address-less event: everything arriving during a retirement
        // belongs to the adapter being torn down.
        Assert.False(coordinator.Accepts(null));
    }

    [Fact]
    public void OutsideARetirementAnAddressLessEventIsGenuineProgress()
    {
        var coordinator = new ActiveAdapterCoordinator();
        var plan = coordinator.Begin(A)!;
        Assert.Null(plan.Previous);

        // Discovery and idle-reset states carry no device.
        Assert.True(coordinator.Accepts(null));
        Assert.True(coordinator.Accepts(A.Value));
        Assert.False(coordinator.Accepts(B.Value));
    }

    [Fact]
    public void ClearingAdvancesTheGenerationSoInFlightWorkCannotComplete()
    {
        var coordinator = new ActiveAdapterCoordinator();
        var plan = coordinator.Begin(A)!;
        coordinator.Cleared();

        Assert.Null(coordinator.State.ActiveId);
        Assert.False(coordinator.RetirementComplete(plan.Generation));
    }

    [Fact]
    public void AdoptionIsRefusedMidTransitionSoALoadCannotOverrideASwitch()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);
        coordinator.Begin(B);

        coordinator.Adopt(C, connected: true);
        Assert.Equal(B, coordinator.State.ActiveId);
    }

    [Fact]
    public void MarkDisconnectedOnlyAppliesToASettledConnectedSession()
    {
        var coordinator = new ActiveAdapterCoordinator();
        Assert.False(coordinator.MarkDisconnected());

        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);
        Assert.True(coordinator.MarkDisconnected());
        Assert.False(coordinator.State.Connected);

        // Mid-transition teardown is expected and is not a disconnection to report.
        coordinator.Begin(B);
        Assert.False(coordinator.MarkDisconnected());
    }
}

public sealed class AdapterSwitchTests
{
    private static readonly AdapterId A = AdapterId.FromAddress("AA:BB:CC:DD:EE:01")!.Value;
    private static readonly AdapterId B = AdapterId.FromAddress("AA:BB:CC:DD:EE:02")!.Value;
    private static readonly AdapterId C = AdapterId.FromAddress("AA:BB:CC:DD:EE:03")!.Value;

    [Fact]
    public async Task TheOrderIsSelectStopRetireClearActivate()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);

        var port = new RecordingPort();
        var outcome = await new AdapterSwitch(coordinator, port).SwitchToAsync(B);

        Assert.IsType<SwitchOutcome.Activating>(outcome);
        Assert.Equal(
            ["select:B", "stopLink:A", "retire:A", "clear", "activate:B"],
            port.Steps);
    }

    [Fact]
    public async Task WithNothingToRetireTheHandoverStepsAreSkipped()
    {
        var coordinator = new ActiveAdapterCoordinator();
        var port = new RecordingPort();
        await new AdapterSwitch(coordinator, port).SwitchToAsync(A);
        Assert.Equal(["select:A", "clear", "activate:A"], port.Steps);
    }

    [Fact]
    public async Task ASwitchThatLostTheRaceStopsWithoutActivating()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);

        // A newer switch begins while this one is inside its retirement await.
        var port = new RecordingPort
        {
            DuringRetirement = () => coordinator.Begin(C),
        };

        var outcome = await new AdapterSwitch(coordinator, port).SwitchToAsync(B);
        Assert.IsType<SwitchOutcome.Superseded>(outcome);
        Assert.DoesNotContain("activate:B", port.Steps);
        Assert.Equal(C, coordinator.State.ActiveId);
    }

    [Fact]
    public async Task SwitchingToAHealthyActiveSessionTearsNothingDown()
    {
        var coordinator = new ActiveAdapterCoordinator();
        coordinator.Begin(A);
        coordinator.ActivationSucceeded(A);

        var port = new RecordingPort();
        var outcome = await new AdapterSwitch(coordinator, port).SwitchToAsync(A);

        Assert.IsType<SwitchOutcome.AlreadyActive>(outcome);
        Assert.Empty(port.Steps);
    }

    private sealed class RecordingPort : IAdapterSwitchPort
    {
        public List<string> Steps { get; } = [];

        public Action? DuringRetirement { get; init; }

        public void SelectionCommitted(AdapterId target, AdapterId? previous) =>
            Steps.Add($"select:{Label(target)}");

        public Task StopControllerLinkAsync(AdapterId previous)
        {
            Steps.Add($"stopLink:{Label(previous)}");
            return Task.CompletedTask;
        }

        public Task RetireManagementAsync(AdapterId previous)
        {
            Steps.Add($"retire:{Label(previous)}");
            DuringRetirement?.Invoke();
            return Task.CompletedTask;
        }

        public void ClearAdapterScopedState() => Steps.Add("clear");

        public void BeginActivation(AdapterId target) => Steps.Add($"activate:{Label(target)}");

        private static string Label(AdapterId id) =>
            id == A ? "A" : id == B ? "B" : id == C ? "C" : id.Value;
    }
}
