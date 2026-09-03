using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.ControllerLink;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Controller Link orchestration over Path C.
///
/// These are the invariants that survive a carrier change: trusted management
/// gates everything, no exit path can leave input held, feedback goes through
/// the shared codec, and the adapter's refusals become sentences a user can act
/// on. The HOGP-era states (Advertising, WaitingForConnection, Connecting,
/// Reconnecting) are gone rather than renamed, so nothing here asserts them.
/// </summary>
public sealed class ControllerLinkTests
{
    // ---------------------------------------------------------------- fakes

    private sealed class FakeManagement(bool ready) : IControllerLinkManagement
    {
        public bool Ready { get; private set; } = ready;

        public string? UnavailableReason =>
            Ready ? null : "Connect to a trusted PicoSwitch adapter first.";

        public event Action? Changed;

        public ControllerLinkState StartResult { get; set; } =
            new(Active: true, Version: ControllerLinkDataPlane.Version, FrameBytes: 30,
                AttMtu: 517, MinimumAttMtu: 33, MtuOk: true);

        public FakeDataPlane? Plane { get; set; } = new();

        public int StartCalls { get; private set; }

        public int StopCalls { get; private set; }

        public Exception? StartThrows { get; set; }

        public void SetReady(bool value)
        {
            Ready = value;
            Changed?.Invoke();
        }

        public Task<ControllerLinkState> StartDataPlaneAsync(CancellationToken ct = default)
        {
            StartCalls++;
            if (StartThrows is not null)
            {
                throw StartThrows;
            }

            return Task.FromResult(StartResult);
        }

        public Task<ControllerLinkState> StopDataPlaneAsync(CancellationToken ct = default)
        {
            StopCalls++;
            return Task.FromResult(new ControllerLinkState(Active: false));
        }

        public Task<ControllerLinkState> DataPlaneStatusAsync(CancellationToken ct = default) =>
            Task.FromResult(StartResult);

        public IControllerLinkDataPlane? TryCreateDataPlane() => Plane;
    }

    private sealed class FakeDataPlane : IControllerLinkDataPlane
    {
        public event Action<byte[]>? OutputFrameReceived;
        public event Action<string>? Closed;

        public bool OpenResult { get; set; } = true;

        public bool Disposed { get; private set; }

        public List<byte[]> Frames { get; } = [];

        public int AttMtu => 517;

        public long StatesPublished
        {
            get { lock (Frames) { return Frames.Count; } }
        }

        public long StatesCoalesced => 0;

        public long FramesWritten
        {
            get { lock (Frames) { return Frames.Count; } }
        }

        public long FrameWriteFailures => 0;

        public long OutputFramesReceived { get; private set; }

        public int MaximumInFlight => 1;

        /// <summary>
        /// Never behind. A fake that reported saturation would silently suppress
        /// the analog frames these tests count, and the failure would look like a
        /// publisher bug rather than a fixture one.
        /// </summary>
        public bool Busy => false;

        public TimeSpan AverageWriteLatency => TimeSpan.FromMilliseconds(1);

        public TimeSpan MaximumWriteLatency => TimeSpan.FromMilliseconds(2);

        public Task<bool> OpenAsync(CancellationToken ct = default) => Task.FromResult(OpenResult);

        public void PublishInput(ReadOnlySpan<byte> frame)
        {
            var copy = frame.ToArray();
            lock (Frames)
            {
                Frames.Add(copy);
            }
        }

        public void RaiseOutput(byte[] frame)
        {
            OutputFramesReceived++;
            OutputFrameReceived?.Invoke(frame);
        }

        public void RaiseClosed(string reason) => Closed?.Invoke(reason);

        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RecordingOutput : IControllerOutputBackend
    {
        private readonly List<RumbleRequest> applied = [];

        public IReadOnlyList<RumbleRequest> Applied
        {
            get { lock (applied) { return [.. applied]; } }
        }

        public void Apply(RumbleRequest request)
        {
            lock (applied)
            {
                applied.Add(request);
            }
        }
    }

    private static (ControllerLinkService Service, FakeManagement Management,
                    ControllerInputSession Input, RecordingOutput Output) Build(bool ready = true)
    {
        var management = new FakeManagement(ready);
        var input = new ControllerInputSession();
        var output = new RecordingOutput();
        var service = new ControllerLinkService(management, input, output, new DiagnosticLog());
        return (service, management, input, output);
    }

    private static async Task<bool> WaitForAsync(Func<bool> condition)
    {
        for (var i = 0; i < 200; i++)
        {
            if (condition())
            {
                return true;
            }

            await Task.Delay(10);
        }

        return condition();
    }

    private static int FrameCount(FakeDataPlane plane)
    {
        lock (plane.Frames)
        {
            return plane.Frames.Count;
        }
    }

    // ------------------------------------------------------------- the gate

    [Fact]
    public async Task RefusesToStartWithoutTrustedManagement()
    {
        var (service, management, _, _) = Build(ready: false);

        await service.StartAsync();

        // Controller Link requires trusted management. Not connected is a
        // refusal the user can act on, not an error.
        Assert.Equal(ControllerLinkPhase.Unavailable, service.View.Value.Phase);
        Assert.Equal(0, management.StartCalls);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task StreamsOnceBothEndsAreArmed()
    {
        var (service, management, _, _) = Build();

        await service.StartAsync();

        Assert.Equal(ControllerLinkPhase.Streaming, service.View.Value.Phase);
        Assert.Equal(1, management.StartCalls);

        // The adapter is armed BEFORE any gameplay byte moves, because that is
        // where the negotiated MTU is measured.
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) > 0));
        await service.DisposeAsync();
    }

    [Fact]
    public async Task PublishesCompleteCanonicalFrames()
    {
        var (service, management, _, _) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) > 0));

        byte[] frame;
        lock (management.Plane!.Frames)
        {
            frame = management.Plane.Frames[0];
        }

        // Exactly one whole frame per tick: header plus the canonical payload,
        // nothing fragmented and nothing padded.
        Assert.Equal(ControllerLinkDataPlane.FrameBytes, frame.Length);
        Assert.Equal(ControllerLinkDataPlane.Version, frame[0]);
        Assert.Equal(ControllerLinkDataPlane.OpcodeState, frame[1]);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task SequenceAdvancesSoTheAdapterCanRejectStaleFrames()
    {
        var (service, management, _, _) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) >= 3));

        List<byte[]> frames;
        lock (management.Plane!.Frames)
        {
            frames = [.. management.Plane.Frames];
        }

        var first = (ushort)(frames[0][2] | (frames[0][3] << 8));
        var third = (ushort)(frames[2][2] | (frames[2][3] << 8));
        Assert.NotEqual(first, third);
        await service.DisposeAsync();
    }

    // ------------------------------------------------------------- refusals

    [Fact]
    public async Task ReportsAnMtuTooSmallToCarryAFrame()
    {
        var (service, management, _, _) = Build();
        management.StartResult = new ControllerLinkState(
            Active: false, Version: ControllerLinkDataPlane.Version,
            FrameBytes: 30, AttMtu: 23, MinimumAttMtu: 33, MtuOk: false);

        await service.StartAsync();

        // A property of the negotiated link, not a fault — and reconnecting can
        // genuinely fix it, so the copy says so.
        Assert.Equal(ControllerLinkPhase.Error, service.View.Value.Phase);
        Assert.Contains("reconnect", service.View.Value.Explanation,
                        StringComparison.OrdinalIgnoreCase);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task ReportsFirmwareWithoutADataPlane()
    {
        var (service, management, _, _) = Build();
        management.Plane!.OpenResult = false;

        await service.StartAsync();

        Assert.Equal(ControllerLinkPhase.Error, service.View.Value.Phase);
        Assert.Contains("Update the adapter", service.View.Value.Explanation);

        // The adapter was told to stand down rather than left armed with nobody
        // streaming to it.
        Assert.Equal(1, management.StopCalls);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task ReportsAVersionDisagreement()
    {
        var (service, management, _, _) = Build();
        management.StartResult = new ControllerLinkState(
            Active: false, Version: ControllerLinkDataPlane.Version + 7,
            AttMtu: 517, MinimumAttMtu: 33, MtuOk: true);

        await service.StartAsync();

        Assert.Equal(ControllerLinkPhase.Error, service.View.Value.Phase);
        Assert.Contains("disagree", service.View.Value.Explanation);
        await service.DisposeAsync();
    }

    // --------------------------------------------------------------- output

    [Fact]
    public async Task DecodesFeedbackThroughTheSharedCodec()
    {
        var (service, management, _, output) = Build();
        await service.StartAsync();

        management.Plane!.RaiseOutput(
        [
            ControllerLinkDataPlane.Version,
            ControllerLinkDataPlane.OpcodeOutput,
            ControllerReportEncoder.OutputReportId,
            0xFF, 0xFF, 0x01, 0x00,
        ]);

        Assert.True(await WaitForAsync(() => output.Applied.Any(r => !r.Silent)));
        Assert.Equal(1, service.Metrics.OutputReportsDecoded);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task IgnoresMalformedFeedbackWithoutTearingDownTheLink()
    {
        var (service, management, _, _) = Build();
        await service.StartAsync();

        management.Plane!.RaiseOutput([0xEE, 0xEE]);
        management.Plane.RaiseOutput([ControllerLinkDataPlane.Version, 0x7F, 0x02, 0, 0, 0, 0]);

        Assert.True(await WaitForAsync(() => service.Metrics.MalformedOutputReports == 2));

        // A stray notification is counted and ignored; the link keeps running.
        Assert.Equal(ControllerLinkPhase.Streaming, service.View.Value.Phase);
        Assert.Equal(0, service.Metrics.OutputReportsDecoded);
        await service.DisposeAsync();
    }

    // -------------------------------------------------------- safe teardown

    [Fact]
    public async Task StopNeutralizesAndTellsTheAdapter()
    {
        var (service, management, _, output) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) > 0));

        await service.StopAsync();

        Assert.Equal(ControllerLinkPhase.Ready, service.View.Value.Phase);
        Assert.True(management.Plane!.Disposed);
        Assert.Equal(1, management.StopCalls);

        // No exit path may leave input held.
        Assert.NotEmpty(output.Applied);
        Assert.True(output.Applied[^1].Silent);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task ManagementLossStopsWithoutTalkingToTheAdapter()
    {
        var (service, management, _, output) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) > 0));

        var stopsBefore = management.StopCalls;
        management.SetReady(false);

        Assert.True(await WaitForAsync(
            () => service.View.Value.Phase == ControllerLinkPhase.Unavailable));

        // The link to the adapter is the thing that just went away, so talking
        // to it would be pointless. Its own watchdog neutralizes the console.
        Assert.Equal(stopsBefore, management.StopCalls);
        Assert.True(output.Applied[^1].Silent);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task CarrierLossTearsDownAndNeutralizes()
    {
        var (service, management, _, output) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) > 0));

        var stopsBefore = management.StopCalls;
        management.Plane!.RaiseClosed("session ended");

        Assert.True(await WaitForAsync(
            () => service.View.Value.Phase == ControllerLinkPhase.Error));

        // Controller Link cannot outlive its carrier, and there is nothing
        // separate still running that would need stopping.
        Assert.Equal(stopsBefore, management.StopCalls);
        Assert.True(output.Applied[^1].Silent);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task StopsPublishingAfterStop()
    {
        var (service, management, _, _) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) > 0));
        await service.StopAsync();

        var after = FrameCount(management.Plane!);
        await Task.Delay(80);
        var later = FrameCount(management.Plane!);

        // A frame escaping after teardown is exactly the held input Stop exists
        // to prevent.
        Assert.Equal(after, later);
        await service.DisposeAsync();
    }

    [Fact]
    public async Task RestartsWithoutReconnectingManagement()
    {
        var (service, management, _, _) = Build();
        await service.StartAsync();
        await service.StopAsync();

        management.Plane = new FakeDataPlane();
        await service.StartAsync();

        Assert.Equal(ControllerLinkPhase.Streaming, service.View.Value.Phase);
        Assert.Equal(2, management.StartCalls);
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane) > 0));
        await service.DisposeAsync();
    }

    [Theory]
    [InlineData("unknown command")]
    [InlineData("unavailable")]
    // Measured on real firmware 2026-09-02: this is what the adapter says when a
    // command exists but is not on its wireless allowlist, and it reached the
    // user as raw protocol text until the classification moved to
    // AdapterCommandException.IsUnsupported().
    [InlineData("command unavailable over Bluetooth")]
    public async Task ReportsOlderFirmwareInProductLanguage(string adapterMessage)
    {
        var (service, management, _, output) = Build();
        management.StartThrows = new AdapterCommandException("clink start", null, adapterMessage);

        await service.StartAsync();

        Assert.Equal(ControllerLinkPhase.Error, service.View.Value.Phase);
        Assert.Contains("Update the adapter", service.View.Value.Explanation);
        Assert.DoesNotContain("Bluetooth", service.View.Value.Explanation);
        Assert.True(output.Applied.All(r => r.Silent));
        await service.DisposeAsync();
    }

    [Fact]
    public async Task SurfacesAnUnexpectedFailureRatherThanSwallowingIt()
    {
        var (service, management, _, output) = Build();
        management.StartThrows = new InvalidOperationException("radio on fire");

        await service.StartAsync();

        Assert.Equal(ControllerLinkPhase.Error, service.View.Value.Phase);
        Assert.Contains("radio on fire", service.View.Value.Explanation);
        Assert.True(output.Applied.All(r => r.Silent));
        await service.DisposeAsync();
    }

    // -------------------------------------------------------------- metrics

    [Fact]
    public async Task MetricsSurviveTeardownSoARunStaysMeasurable()
    {
        var (service, management, _, _) = Build();
        await service.StartAsync();
        Assert.True(await WaitForAsync(() => FrameCount(management.Plane!) >= 2));
        await service.StopAsync();

        // A stopped session reporting zeroes would make the run that just
        // happened unmeasurable, which is when the numbers matter most.
        var metrics = service.Metrics;
        Assert.True(metrics.ReportsGenerated > 0);
        Assert.True(metrics.WritesIssued > 0);
        Assert.Equal(517, metrics.AttMtu);
        Assert.Contains("maxInFlight=1", metrics.Summary());
        await service.DisposeAsync();
    }
}
