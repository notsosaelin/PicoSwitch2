using System.Diagnostics;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.ControllerLink;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Controller Link orchestration over the trusted management link (Path C).
///
/// ## The shape, and why it is this small
///
/// Windows has no Classic HID Device role, and a second LE relationship to the
/// adapter is refused by the controller with 0x0B, so there is no second
/// connection to manage. Controller state rides a binary characteristic on the
/// management session that is already open.
///
/// That removes a whole class of state this service used to own. Nothing
/// advertises, nothing pairs, nothing dials this PC, and the link cannot be
/// lost independently of its carrier — if management is up the data plane is
/// reachable, and if management goes Controller Link goes with it. What remains
/// is genuinely small: gate on trusted management, arm both ends, stream, and
/// neutralize on every exit.
///
/// ## What did not change
///
/// Everything above the transport is the code the HOGP carrier used:
/// ControllerInputSession owns normalized state, ControllerReportEncoder owns
/// the wire layout, the 125 Hz scheduler is latest-state-wins, BridgeOutputCodec
/// and RumbleShaping own feedback. Only the boundary moved.
/// </summary>
public sealed class ControllerLinkService : IAsyncDisposable
{
    private static readonly TimeSpan ReportInterval = TimeSpan.FromMilliseconds(8);

    private readonly IControllerLinkManagement management;
    private readonly ControllerInputSession input;
    private readonly IControllerOutputBackend output;
    private readonly DiagnosticLog diagnostics;
    private readonly StateValue<ControllerLinkView> view;
    private readonly SemaphoreSlim lifecycle = new(1, 1);

    private IControllerLinkDataPlane? dataPlane;
    private CancellationTokenSource? sessionCancellation;
    private Task? publisherTask;
    private int stopping;
    private ushort sequence;
    private int shapedLeft;
    private int shapedRight;
    private long reportsGenerated;
    private long reportIntervals;
    private long totalReportIntervalTicks;
    private long maximumReportIntervalTicks;
    private long outputReportsDecoded;
    private long malformedOutputReports;
    private long outputDeliveryFailures;
    private ControllerLinkMetrics finalMetrics = ControllerLinkMetrics.Empty;
    private bool disposed;

    public ControllerLinkService(
        IControllerLinkManagement management,
        ControllerInputSession input,
        IControllerOutputBackend output,
        DiagnosticLog diagnostics)
    {
        this.management = management;
        this.input = input;
        this.output = output;
        this.diagnostics = diagnostics;
        view = new StateValue<ControllerLinkView>(ReadyView());
        management.Changed += OnManagementChanged;
    }

    public IReadOnlyStateValue<ControllerLinkView> View => view;

    public ControllerLinkMetrics Metrics
    {
        get
        {
            var plane = dataPlane;
            if (plane is null)
            {
                return finalMetrics with
                {
                    ReportsGenerated = Interlocked.Read(ref reportsGenerated),
                };
            }

            var intervals = Interlocked.Read(ref reportIntervals);
            return new ControllerLinkMetrics(
                Interlocked.Read(ref reportsGenerated),
                plane.StatesPublished,
                plane.FramesWritten,
                plane.StatesCoalesced,
                plane.FrameWriteFailures,
                plane.MaximumInFlight,
                intervals == 0 ? TimeSpan.Zero : TicksToTimeSpan(
                    Interlocked.Read(ref totalReportIntervalTicks) / intervals),
                TicksToTimeSpan(Interlocked.Read(ref maximumReportIntervalTicks)),
                plane.AverageWriteLatency,
                plane.MaximumWriteLatency,
                plane.OutputFramesReceived,
                Interlocked.Read(ref outputReportsDecoded),
                Interlocked.Read(ref malformedOutputReports),
                Interlocked.Read(ref outputDeliveryFailures),
                plane.AttMtu);
        }
    }

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (dataPlane is not null)
            {
                return;
            }

            if (!management.Ready)
            {
                SetView(ControllerLinkPhase.Unavailable, management.UnavailableReason);
                return;
            }

            Interlocked.Exchange(ref stopping, 0);
            ResetMetrics();
            input.Neutralize();
            output.Apply(RumbleRequest.None);
            SetView(ControllerLinkPhase.Starting);

            // Arm the adapter FIRST. It measures the negotiated ATT MTU and
            // refuses below one whole frame, so this answers "may I stream"
            // before a single gameplay byte moves.
            var state = await management.StartDataPlaneAsync(cancellationToken)
                .ConfigureAwait(false);
            diagnostics.Info(
                "controller-link",
                $"adapter data plane: active={state.Active} version={state.Version} " +
                $"frame={state.FrameBytes} mtu={state.AttMtu}/{state.MinimumAttMtu}");

            if (!state.Active)
            {
                SetView(ControllerLinkPhase.Error, RefusalReason(state));
                return;
            }

            var plane = management.TryCreateDataPlane();
            if (plane is null)
            {
                await SafeStopDataPlaneAsync().ConfigureAwait(false);
                SetView(
                    ControllerLinkPhase.Unavailable,
                    management.UnavailableReason
                        ?? "The management connection went away while starting.");
                return;
            }

            if (!await plane.OpenAsync(cancellationToken).ConfigureAwait(false))
            {
                await plane.DisposeAsync().ConfigureAwait(false);
                await SafeStopDataPlaneAsync().ConfigureAwait(false);
                SetView(
                    ControllerLinkPhase.Error,
                    "This adapter's firmware does not support Controller Link. Update the adapter.");
                return;
            }

            dataPlane = plane;
            plane.OutputFrameReceived += OnOutputFrame;
            plane.Closed += OnCarrierLost;

            sessionCancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            publisherTask = PublishReportsAsync(sessionCancellation.Token);
            SetView(ControllerLinkPhase.Streaming);
            diagnostics.Info(
                "controller-link",
                $"streaming at {1000.0 / ReportInterval.TotalMilliseconds:F0} Hz, mtu={plane.AttMtu}");
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            diagnostics.Error("controller-link", $"start failed: {error.Message}");
            await TearDownLockedAsync(tellAdapter: true).ConfigureAwait(false);
            SetView(ControllerLinkPhase.Error, ProductError(error));
        }
        finally
        {
            lifecycle.Release();
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (dataPlane is null)
            {
                SetReadyOrUnavailable();
                return;
            }

            Interlocked.Exchange(ref stopping, 1);
            SetView(ControllerLinkPhase.Stopping);
            input.Neutralize();
            ApplyOutput(RumbleRequest.None);
            await TearDownLockedAsync(tellAdapter: true, cancellationToken).ConfigureAwait(false);
            diagnostics.Info("controller-link", $"stopped: {Metrics.Summary()}");
            SetReadyOrUnavailable();
        }
        finally
        {
            Interlocked.Exchange(ref stopping, 0);
            lifecycle.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        management.Changed -= OnManagementChanged;
        await StopAsync().ConfigureAwait(false);
        disposed = true;
        lifecycle.Dispose();
    }

    private async Task PublishReportsAsync(CancellationToken cancellationToken)
    {
        long previous = 0;
        var frame = new byte[ControllerLinkDataPlane.FrameBytes];
        try
        {
            using var timer = new PeriodicTimer(ReportInterval);
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                var now = Stopwatch.GetTimestamp();
                if (previous != 0)
                {
                    var interval = now - previous;
                    Interlocked.Increment(ref reportIntervals);
                    Interlocked.Add(ref totalReportIntervalTicks, interval);
                    UpdateMaximum(ref maximumReportIntervalTicks, interval);
                }

                previous = now;

                // One tick, one complete normalized state, one frame handed to
                // the bounded writer. Whether it reaches the air now or replaces
                // a pending frame is the writer's decision, not this loop's.
                var payload = ControllerReportEncoder.Encode(input.Snapshot);
                ControllerLinkDataPlane.EncodeInput(payload, unchecked(sequence++), frame);
                dataPlane?.PublishInput(frame);
                Interlocked.Increment(ref reportsGenerated);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            OnCarrierLost($"Controller report scheduler failed: {error.Message}");
        }
    }

    private void OnOutputFrame(byte[] frame)
    {
        var decodedFrame = ControllerLinkDataPlane.DecodeOutput(frame);
        if (decodedFrame is null)
        {
            Interlocked.Increment(ref malformedOutputReports);
            return;
        }

        var decoded = BridgeOutputCodec.Decode(decodedFrame.Payload, decodedFrame.ReportId);
        if (decoded is null)
        {
            Interlocked.Increment(ref malformedOutputReports);
            return;
        }

        Interlocked.Increment(ref outputReportsDecoded);

        shapedLeft = RumbleShaping.Shape(decoded.Value.Rumble.Left, shapedLeft);
        shapedRight = RumbleShaping.Shape(decoded.Value.Rumble.Right, shapedRight);
        ApplyOutput(new RumbleRequest(shapedLeft, shapedRight));

        var count = Interlocked.Read(ref outputReportsDecoded);
        if (count == 1 || count % 128 == 0)
        {
            diagnostics.Debug(
                "controller-link",
                $"output reports={count} malformed={Interlocked.Read(ref malformedOutputReports)} " +
                $"player={decoded.Value.PlayerIndicator} motion={decoded.Value.MotionRequested}");
        }
    }

    private void ApplyOutput(RumbleRequest request)
    {
        try
        {
            output.Apply(request);
        }
        catch (Exception error)
        {
            Interlocked.Increment(ref outputDeliveryFailures);
            diagnostics.Warn("controller-link", $"local output delivery failed: {error.Message}");
        }
    }

    /// <summary>
    /// The carrier went away. Controller Link cannot outlive it, and unlike the
    /// HOGP host there is nothing separate still running that would need
    /// stopping — so this is a teardown, not a reconnect.
    /// </summary>
    private void OnCarrierLost(string reason)
    {
        if (Volatile.Read(ref stopping) != 0 || dataPlane is null)
        {
            return;
        }

        _ = HandleCarrierLossAsync(reason);
    }

    private async Task HandleCarrierLossAsync(string reason)
    {
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            if (dataPlane is null || Volatile.Read(ref stopping) != 0)
            {
                return;
            }

            Interlocked.Exchange(ref stopping, 1);
            input.Neutralize();
            ApplyOutput(RumbleRequest.None);

            // Do not talk to the adapter: the reason we are here is that the
            // link to it is gone. Its own stale-input watchdog neutralizes the
            // console within 300 ms, which is exactly what that watchdog is for.
            await TearDownLockedAsync(tellAdapter: false).ConfigureAwait(false);
            diagnostics.Error("controller-link", $"carrier lost: {reason}; {Metrics.Summary()}");
            SetView(ControllerLinkPhase.Error, ProductError(new InvalidOperationException(reason)));
        }
        finally
        {
            Interlocked.Exchange(ref stopping, 0);
            lifecycle.Release();
        }
    }

    private void OnManagementChanged()
    {
        if (management.Ready)
        {
            if (dataPlane is null && view.Value.Phase is
                ControllerLinkPhase.Unavailable or ControllerLinkPhase.Stopped)
            {
                SetView(ControllerLinkPhase.Ready);
            }

            return;
        }

        if (dataPlane is null)
        {
            SetView(ControllerLinkPhase.Unavailable, management.UnavailableReason);
            return;
        }

        _ = StopForManagementLossAsync();
    }

    private async Task StopForManagementLossAsync()
    {
        diagnostics.Warn("controller-link", "trusted management lost; stopping safely");
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            if (dataPlane is not null)
            {
                Interlocked.Exchange(ref stopping, 1);
                input.Neutralize();
                ApplyOutput(RumbleRequest.None);
                await TearDownLockedAsync(tellAdapter: false).ConfigureAwait(false);
            }
        }
        finally
        {
            Interlocked.Exchange(ref stopping, 0);
            lifecycle.Release();
        }

        SetView(ControllerLinkPhase.Unavailable, management.UnavailableReason);
    }

    private async Task TearDownLockedAsync(
        bool tellAdapter,
        CancellationToken cancellationToken = default)
    {
        var currentCancellation = sessionCancellation;
        sessionCancellation = null;
        currentCancellation?.Cancel();

        var currentPublisher = publisherTask;
        publisherTask = null;
        if (currentPublisher is not null)
        {
            try
            {
                await currentPublisher.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        var plane = dataPlane;
        if (plane is not null)
        {
            // Snapshot BEFORE clearing the field, or a stopped session reports
            // zeroes and the run that just happened becomes unmeasurable.
            finalMetrics = Metrics;
            dataPlane = null;
            plane.OutputFrameReceived -= OnOutputFrame;
            plane.Closed -= OnCarrierLost;
            await plane.DisposeAsync().ConfigureAwait(false);
        }

        if (tellAdapter && management.Ready)
        {
            await SafeStopDataPlaneAsync(cancellationToken).ConfigureAwait(false);
        }

        currentCancellation?.Dispose();
        input.Neutralize();
        ApplyOutput(RumbleRequest.None);
    }

    private async Task SafeStopDataPlaneAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            await management.StopDataPlaneAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            // The adapter's watchdog neutralizes on its own if this never
            // lands, so a failed stop is untidy rather than unsafe.
            diagnostics.Debug("controller-link", $"adapter stop failed: {error.Message}");
        }
    }

    private void SetReadyOrUnavailable() =>
        SetView(
            management.Ready ? ControllerLinkPhase.Ready : ControllerLinkPhase.Unavailable,
            management.UnavailableReason);

    private ControllerLinkView ReadyView() => ControllerLinkView.Of(
        management.Ready ? ControllerLinkPhase.Ready : ControllerLinkPhase.Unavailable,
        management.UnavailableReason,
        management.Ready);

    private void SetView(ControllerLinkPhase phase, string? detail = null) =>
        view.Set(ControllerLinkView.Of(phase, detail, management.Ready));

    /// <summary>
    /// Turn the adapter's refusal into something a user can act on. The MTU
    /// case is worth naming: it is a property of the negotiated link rather
    /// than a fault, and reconnecting genuinely can fix it.
    /// </summary>
    private static string RefusalReason(ControllerLinkState state)
    {
        if (!state.MtuOk && state.AttMtu > 0)
        {
            return "This Bluetooth connection is too small to carry controller input. " +
                   "Disconnect and reconnect the adapter.";
        }

        if (state.Version != 0 && state.Version != ControllerLinkDataPlane.Version)
        {
            return "The adapter and this app disagree about Controller Link. Update both.";
        }

        return "The adapter refused to start Controller Link.";
    }

    private static string ProductError(Exception error)
    {
        var message = error.Message;
        if (message.Contains("unknown command", StringComparison.OrdinalIgnoreCase))
        {
            return "This adapter's firmware does not support Controller Link. Update the adapter.";
        }

        return message;
    }

    private void ResetMetrics()
    {
        reportsGenerated = 0;
        reportIntervals = 0;
        totalReportIntervalTicks = 0;
        maximumReportIntervalTicks = 0;
        outputReportsDecoded = 0;
        malformedOutputReports = 0;
        outputDeliveryFailures = 0;
        shapedLeft = 0;
        shapedRight = 0;
        sequence = 0;
        finalMetrics = ControllerLinkMetrics.Empty;
    }

    private static void UpdateMaximum(ref long target, long candidate)
    {
        var current = Volatile.Read(ref target);
        while (candidate > current)
        {
            var observed = Interlocked.CompareExchange(ref target, candidate, current);
            if (observed == current)
            {
                return;
            }

            current = observed;
        }
    }

    private static TimeSpan TicksToTimeSpan(long ticks) =>
        TimeSpan.FromSeconds(ticks / (double)Stopwatch.Frequency);
}

/// <summary>
/// Everything a qualification run needs to tell "the scheduler is slow" from
/// "the radio is behind" from "the adapter is dropping frames".
/// </summary>
public sealed record ControllerLinkMetrics(
    long ReportsGenerated,
    long StatesPublished,
    long WritesIssued,
    long StatesCoalesced,
    long WriteFailures,
    int MaximumInFlight,
    TimeSpan AverageReportInterval,
    TimeSpan MaximumReportInterval,
    TimeSpan AverageWriteLatency,
    TimeSpan MaximumWriteLatency,
    long OutputFramesReceived,
    long OutputReportsDecoded,
    long MalformedOutputReports,
    long OutputDeliveryFailures,
    int AttMtu)
{
    public static readonly ControllerLinkMetrics Empty = new(
        0, 0, 0, 0, 0, 0,
        TimeSpan.Zero, TimeSpan.Zero, TimeSpan.Zero, TimeSpan.Zero,
        0, 0, 0, 0, 0);

    public string Summary() =>
        $"generated={ReportsGenerated} published={StatesPublished} written={WritesIssued} " +
        $"coalesced={StatesCoalesced} writeFailures={WriteFailures} maxInFlight={MaximumInFlight} " +
        $"intervalAvgMs={AverageReportInterval.TotalMilliseconds:F2} " +
        $"intervalMaxMs={MaximumReportInterval.TotalMilliseconds:F2} " +
        $"writeAvgMs={AverageWriteLatency.TotalMilliseconds:F2} " +
        $"writeMaxMs={MaximumWriteLatency.TotalMilliseconds:F2} " +
        $"output={OutputFramesReceived}/{OutputReportsDecoded} " +
        $"malformed={MalformedOutputReports} outputFailures={OutputDeliveryFailures} mtu={AttMtu}";
}
