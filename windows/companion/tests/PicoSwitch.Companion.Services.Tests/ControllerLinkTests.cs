using System.Diagnostics;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.ControllerLink;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

public sealed class ControllerLinkViewTests
{
    [Theory]
    [InlineData(ControllerLinkPhase.Unavailable, false, false)]
    [InlineData(ControllerLinkPhase.Ready, true, false)]
    [InlineData(ControllerLinkPhase.Starting, false, true)]
    [InlineData(ControllerLinkPhase.WaitingForConnection, false, true)]
    [InlineData(ControllerLinkPhase.Connected, false, false)]
    [InlineData(ControllerLinkPhase.Error, true, false)]
    public void EveryProductStateHasDeterministicActions(
        ControllerLinkPhase phase,
        bool canStart,
        bool busy)
    {
        var view = ControllerLinkView.Of(phase, managementReady: true);

        Assert.False(string.IsNullOrWhiteSpace(view.Headline));
        Assert.False(string.IsNullOrWhiteSpace(view.Explanation));
        Assert.Equal(canStart, view.CanStart);
        Assert.Equal(busy, view.Busy);
    }

    [Fact]
    public void ProductCopyDoesNotExposeImplementationTerms()
    {
        foreach (var phase in Enum.GetValues<ControllerLinkPhase>())
        {
            var current = ControllerLinkView.Of(phase, managementReady: true);
            var text = current.Headline + current.Explanation;
            Assert.DoesNotContain("AppContainer", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("AUMID", text, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("HCI", text, StringComparison.OrdinalIgnoreCase);
        }
    }
}

public sealed class ControllerLinkServiceTests
{
    [Fact]
    public async Task TrustedManagementIsRequiredBeforeHelperActivation()
    {
        var fixture = new Fixture(ready: false);
        await using var service = fixture.Service;

        await service.StartAsync();

        Assert.Equal(0, fixture.Factory.OpenCalls);
        Assert.Equal(ControllerLinkPhase.Unavailable, service.View.Value.Phase);
        Assert.Contains("trusted", service.View.Value.Explanation, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task StartActivatesHostAndWaitsForARealConnection()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;

        await service.StartAsync();

        Assert.Equal(1, fixture.Factory.OpenCalls);
        Assert.Equal(1, fixture.Host.StartCalls);
        Assert.Equal(ControllerLinkPhase.WaitingForConnection, service.View.Value.Phase);
        Assert.NotEqual(ControllerLinkPhase.Connected, service.View.Value.Phase);
        await service.StopAsync();
    }

    [Fact]
    public async Task HostSubscriptionIsTheOnlyConnectedSuccessSignal()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;
        await service.StartAsync();

        fixture.Host.RaiseState(ControllerLinkHostState.Connected);

        Assert.Equal(ControllerLinkPhase.Connected, service.View.Value.Phase);
        await service.StopAsync();
    }

    [Fact]
    public async Task StopNeutralizesInputBeforeTransportTeardown()
    {
        var fixture = new Fixture();
        var source = new ControllerSourceIdentity("test", "Test pad", 1, 2);
        fixture.Input.ApplyPhysicalFrame(
            source,
            ControllerButtonSet.Of(ControllerButton.A),
            AnalogFrame.Neutral with { LeftX = 255 });
        await using var service = fixture.Service;
        await service.StartAsync();
        fixture.Input.ApplyPhysicalFrame(
            source,
            ControllerButtonSet.Of(ControllerButton.A),
            AnalogFrame.Neutral with { LeftX = 255 });

        await service.StopAsync();

        Assert.Equal(ControllerState.Neutral, fixture.Input.Snapshot);
        Assert.Equal(RumbleRequest.None, fixture.Output.Last);
        Assert.Equal(1, fixture.Host.StopCalls);
    }

    [Fact]
    public async Task ManagementLossStopsTheHostAndNeutralizes()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;
        await service.StartAsync();
        fixture.Host.RaiseState(ControllerLinkHostState.Connected);

        fixture.Management.SetReady(false);
        await WaitForAsync(() => service.View.Value.Phase == ControllerLinkPhase.Unavailable);

        Assert.Equal(1, fixture.Host.StopCalls);
        Assert.Equal(ControllerState.Neutral, fixture.Input.Snapshot);
    }

    [Fact]
    public async Task OutputReportUsesSharedDecoderAndRumbleShaping()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;
        await service.StartAsync();

        fixture.Host.RaiseOutput([100, 200, 3, 1]);

        Assert.Equal(new RumbleRequest(96, 208), fixture.Output.Last);
        Assert.Equal(1, service.Metrics.OutputReportsDecoded);
        Assert.Equal(0, service.Metrics.MalformedOutputReports);
        await service.StopAsync();
    }

    [Fact]
    public async Task MalformedOutputIsRejectedWithoutChangingActuators()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;
        await service.StartAsync();

        fixture.Host.RaiseOutput([1, 2]);

        Assert.Equal(RumbleRequest.None, fixture.Output.Last);
        Assert.Equal(1, service.Metrics.MalformedOutputReports);
        await service.StopAsync();
    }

    [Fact]
    public async Task UnexpectedHelperLossBecomesRecoverableErrorAndNeutralizes()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;
        await service.StartAsync();
        fixture.Host.RaiseState(ControllerLinkHostState.Connected);

        fixture.Host.RaiseClosed("crashed");
        await WaitForAsync(() => service.View.Value.Phase == ControllerLinkPhase.Error);

        Assert.True(service.View.Value.CanStart);
        Assert.Equal(ControllerState.Neutral, fixture.Input.Snapshot);
        Assert.Equal(RumbleRequest.None, fixture.Output.Last);
    }

    [Fact]
    public async Task ReportPublisherUsesBoundedHostMailboxAtBridgeCadence()
    {
        var fixture = new Fixture();
        await using var service = fixture.Service;
        await service.StartAsync();

        await Task.Delay(70);
        await service.StopAsync();

        Assert.InRange(fixture.Host.InputReportsQueued, 4, 20);
        Assert.All(fixture.Host.Published, report => Assert.Equal(26, report.Length));
        Assert.InRange(service.Metrics.AverageReportInterval.TotalMilliseconds, 4, 16);
    }

    private static async Task WaitForAsync(Func<bool> predicate)
    {
        var deadline = Stopwatch.GetTimestamp() + (2 * Stopwatch.Frequency);
        while (!predicate() && Stopwatch.GetTimestamp() < deadline)
        {
            await Task.Delay(10);
        }

        Assert.True(predicate(), "condition did not settle before deadline");
    }

    private sealed class Fixture
    {
        public Fixture(bool ready = true)
        {
            Management = new FakeManagement(ready);
            Host = new FakeHost();
            Factory = new FakeFactory(Host);
            Input = new ControllerInputSession();
            Output = new FakeOutput();
            Service = new ControllerLinkService(
                Management, Input, Output, new DiagnosticLog(), Factory);
        }

        public FakeManagement Management { get; }

        public FakeHost Host { get; }

        public FakeFactory Factory { get; }

        public ControllerInputSession Input { get; }

        public FakeOutput Output { get; }

        public ControllerLinkService Service { get; }
    }

    private sealed class FakeManagement(bool ready) : IControllerLinkManagement
    {
        public bool Ready { get; private set; } = ready;

        public string? UnavailableReason => Ready ? null : "Connect to a trusted PicoSwitch adapter first.";

        public event Action? Changed;

        public void SetReady(bool value)
        {
            Ready = value;
            Changed?.Invoke();
        }

        public Task<PairingStatus> StartPairingAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(new PairingStatus(1, PairingState.Discovering));

        public Task<PairingStatus> PairingStatusAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(new PairingStatus(1, PairingState.Discovering));

        public Task<PairingStatus> CancelPairingAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(new PairingStatus(1, PairingState.Cancelled));
    }

    private sealed class FakeFactory(FakeHost host) : IControllerLinkHostFactory
    {
        public int OpenCalls { get; private set; }

        public Task<IControllerLinkHostConnection> OpenAsync(
            CancellationToken cancellationToken = default)
        {
            OpenCalls++;
            return Task.FromResult<IControllerLinkHostConnection>(host);
        }
    }

    private sealed class FakeHost : IControllerLinkHostConnection
    {
        public event Action<ControllerLinkHostState, string?>? StateChanged;
        public event Action<ControllerLinkOutputReport>? OutputReportReceived;
        public event Action<string>? Closed;

        public HostHello? Handshake { get; } = new(
            ControllerLinkIpcProtocol.HelperBuild,
            4,
            161,
            26,
            4,
            ControllerLinkHostConnection.DescriptorSha256);

        public long InputReportsQueued { get; private set; }
        public long InputReportsSent => InputReportsQueued;
        public long InputReportsCoalesced => 0;
        public long OutputReportsReceived { get; private set; }
        public int StartCalls { get; private set; }
        public int StopCalls { get; private set; }
        public List<byte[]> Published { get; } = [];

        public Task StartAsync(CancellationToken cancellationToken = default)
        {
            StartCalls++;
            StateChanged?.Invoke(ControllerLinkHostState.Starting, null);
            StateChanged?.Invoke(ControllerLinkHostState.Advertising, null);
            StateChanged?.Invoke(ControllerLinkHostState.WaitingForConnection, null);
            return Task.CompletedTask;
        }

        public void PublishInput(ReadOnlySpan<byte> report)
        {
            InputReportsQueued++;
            Published.Add(report.ToArray());
        }

        public Task StopAsync(CancellationToken cancellationToken = default)
        {
            StopCalls++;
            return Task.CompletedTask;
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;

        public void RaiseState(ControllerLinkHostState state) => StateChanged?.Invoke(state, null);

        public void RaiseClosed(string reason) => Closed?.Invoke(reason);

        public void RaiseOutput(byte[] payload)
        {
            OutputReportsReceived++;
            var now = Stopwatch.GetTimestamp();
            OutputReportReceived?.Invoke(new ControllerLinkOutputReport(1, now, now, payload));
        }
    }

    private sealed class FakeOutput : IControllerOutputBackend
    {
        public RumbleRequest Last { get; private set; }

        public void Apply(RumbleRequest request) => Last = request;
    }
}
