using System.Diagnostics;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.ControllerLink;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Production Controller Link orchestration. The full-trust app owns trusted
/// management and normalized input; the same-package AppContainer owns only the
/// Windows HOGP peripheral. This class is the one lifetime boundary between them.
/// </summary>
public sealed class ControllerLinkService : IAsyncDisposable
{
    private static readonly TimeSpan ReportInterval = TimeSpan.FromMilliseconds(8);
    private static readonly TimeSpan RememberedReconnectGrace = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan PairingPollInterval = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan PairingDeadline = TimeSpan.FromSeconds(32);

    private readonly IControllerLinkManagement management;
    private readonly ControllerInputSession input;
    private readonly IControllerOutputBackend output;
    private readonly IControllerLinkHostFactory hostFactory;
    private readonly DiagnosticLog diagnostics;
    private readonly StateValue<ControllerLinkView> view;
    private readonly SemaphoreSlim lifecycle = new(1, 1);
    private readonly object stateGate = new();

    private IControllerLinkHostConnection? host;
    private CancellationTokenSource? sessionCancellation;
    private Task? publisherTask;
    private int stopping;
    private int connectionGeneration;
    private bool hostConnected;
    private int shapedLeft;
    private int shapedRight;
    private long reportsGenerated;
    private long reportIntervals;
    private long totalReportIntervalTicks;
    private long maximumReportIntervalTicks;
    private long outputReportsDecoded;
    private long malformedOutputReports;
    private long outputDeliveryFailures;
    private long totalOutputLatencyTicks;
    private long maximumOutputLatencyTicks;
    private long finalReportsQueued;
    private long finalReportsSent;
    private long finalReportsCoalesced;
    private long finalOutputReportsReceived;
    private int remotePairingActive;
    private bool disposed;

    public ControllerLinkService(
        IControllerLinkManagement management,
        ControllerInputSession input,
        IControllerOutputBackend output,
        DiagnosticLog diagnostics,
        IControllerLinkHostFactory? hostFactory = null)
    {
        this.management = management;
        this.input = input;
        this.output = output;
        this.diagnostics = diagnostics;
        this.hostFactory = hostFactory ?? ControllerLinkHostFactory.Instance;
        view = new StateValue<ControllerLinkView>(ReadyView());
        management.Changed += OnManagementChanged;
    }

    public IReadOnlyStateValue<ControllerLinkView> View => view;

    public ControllerLinkMetrics Metrics
    {
        get
        {
            var current = host;
            var intervals = Interlocked.Read(ref reportIntervals);
            var outputs = Interlocked.Read(ref outputReportsDecoded);
            return new ControllerLinkMetrics(
                Interlocked.Read(ref reportsGenerated),
                current?.InputReportsQueued ?? Interlocked.Read(ref finalReportsQueued),
                current?.InputReportsSent ?? Interlocked.Read(ref finalReportsSent),
                current?.InputReportsCoalesced ?? Interlocked.Read(ref finalReportsCoalesced),
                intervals == 0 ? TimeSpan.Zero : TicksToTimeSpan(
                    Interlocked.Read(ref totalReportIntervalTicks) / intervals),
                TicksToTimeSpan(Interlocked.Read(ref maximumReportIntervalTicks)),
                current?.OutputReportsReceived ?? Interlocked.Read(ref finalOutputReportsReceived),
                outputs,
                Interlocked.Read(ref malformedOutputReports),
                Interlocked.Read(ref outputDeliveryFailures),
                outputs == 0 ? TimeSpan.Zero : TicksToTimeSpan(
                    Interlocked.Read(ref totalOutputLatencyTicks) / outputs),
                TicksToTimeSpan(Interlocked.Read(ref maximumOutputLatencyTicks)));
        }
    }

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (host is not null)
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
            diagnostics.Info("controller-link", "activating same-package Bluetooth host");

            var opened = await hostFactory.OpenAsync(cancellationToken).ConfigureAwait(false);
            host = opened;
            opened.StateChanged += OnHostStateChanged;
            opened.OutputReportReceived += OnOutputReport;
            opened.Closed += OnHostClosed;

            if (opened.Handshake is { } hello)
            {
                diagnostics.Info(
                    "controller-link",
                    $"helper handshake: build={hello.HelperBuild:x8} ipc={ControllerLinkHostConnection.IpcVersion} " +
                    $"bridge={hello.BridgeContract} descriptor={hello.DescriptorBytes}/" +
                    $"{hello.DescriptorSha256} input={hello.InputReportBytes} output={hello.OutputReportBytes}");
            }

            await opened.StartAsync(cancellationToken).ConfigureAwait(false);
            sessionCancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            publisherTask = PublishReportsAsync(sessionCancellation.Token);
            if (view.Value.Phase == ControllerLinkPhase.Starting)
            {
                SetView(ControllerLinkPhase.WaitingForConnection);
            }

            BeginConnectionAttempt();
            diagnostics.Info("controller-link", "advertising settled at Started; report scheduler active");
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            diagnostics.Error("controller-link", $"start failed: {error.Message}");
            await TearDownLockedAsync(callHostStop: false).ConfigureAwait(false);
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
            if (host is null)
            {
                SetReadyOrUnavailable();
                return;
            }

            Interlocked.Exchange(ref stopping, 1);
            SetView(ControllerLinkPhase.Stopping);
            input.Neutralize();
            ApplyOutput(RumbleRequest.None);
            await TearDownLockedAsync(callHostStop: true, cancellationToken).ConfigureAwait(false);
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
                host?.PublishInput(ControllerReportEncoder.Encode(input.Snapshot));
                Interlocked.Increment(ref reportsGenerated);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            OnHostClosed($"Controller report channel failed: {error.Message}");
        }
    }

    private void OnHostStateChanged(ControllerLinkHostState state, string? detail)
    {
        diagnostics.Info(
            "controller-link",
            $"host state={state}" + (string.IsNullOrWhiteSpace(detail) ? string.Empty : $" detail={detail}"));

        switch (state)
        {
            case ControllerLinkHostState.Ready:
            case ControllerLinkHostState.Starting:
                SetView(ControllerLinkPhase.Starting);
                break;
            case ControllerLinkHostState.Advertising:
                SetView(ControllerLinkPhase.Advertising);
                break;
            case ControllerLinkHostState.WaitingForConnection:
                SetView(ControllerLinkPhase.WaitingForConnection);
                break;
            case ControllerLinkHostState.Connected:
                lock (stateGate)
                {
                    hostConnected = true;
                    connectionGeneration++;
                }

                SetView(ControllerLinkPhase.Connected);
                break;
            case ControllerLinkHostState.Disconnected:
                lock (stateGate)
                {
                    hostConnected = false;
                }

                input.Neutralize();
                ApplyOutput(RumbleRequest.None);
                SetView(ControllerLinkPhase.Reconnecting, detail);
                BeginConnectionAttempt();
                break;
            case ControllerLinkHostState.Stopped:
                if (Volatile.Read(ref stopping) == 0)
                {
                    OnHostClosed(detail ?? "Controller Link host stopped unexpectedly.");
                }

                break;
            case ControllerLinkHostState.Error:
                OnHostClosed(detail ?? "Windows Bluetooth peripheral error.");
                break;
        }
    }

    private void OnOutputReport(ControllerLinkOutputReport report)
    {
        var decoded = BridgeOutputCodec.Decode(
            report.Payload,
            ControllerReportEncoder.OutputReportId);
        if (decoded is null)
        {
            Interlocked.Increment(ref malformedOutputReports);
            return;
        }

        Interlocked.Increment(ref outputReportsDecoded);
        Interlocked.Add(ref totalOutputLatencyTicks, Math.Max(0, report.MainReceiveTimestamp - report.HostTimestamp));
        UpdateMaximum(
            ref maximumOutputLatencyTicks,
            Math.Max(0, report.MainReceiveTimestamp - report.HostTimestamp));

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

    private void BeginConnectionAttempt()
    {
        CancellationToken token;
        int generation;
        lock (stateGate)
        {
            if (host is null || sessionCancellation is null || hostConnected)
            {
                return;
            }

            generation = ++connectionGeneration;
            token = sessionCancellation.Token;
        }

        _ = CoordinateConnectionAsync(generation, token);
    }

    private async Task CoordinateConnectionAsync(int generation, CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(RememberedReconnectGrace, cancellationToken).ConfigureAwait(false);
            if (!AttemptCurrent(generation) || !management.Ready)
            {
                return;
            }

            SetView(ControllerLinkPhase.Connecting, "Asking the adapter to find this controller.");
            var status = await management.StartPairingAsync(cancellationToken).ConfigureAwait(false);
            Interlocked.Exchange(ref remotePairingActive, status.Active ? 1 : 0);
            diagnostics.Info(
                "controller-link",
                $"remote pairing: op={status.Operation} state={status.State.WireName()} " +
                $"reason={status.Reason.WireName()}");

            var deadline = Stopwatch.GetTimestamp() +
                (long)(PairingDeadline.TotalSeconds * Stopwatch.Frequency);
            while (AttemptCurrent(generation) && Stopwatch.GetTimestamp() < deadline)
            {
                if (!status.Active)
                {
                    if (status.State == PairingState.Paired)
                    {
                        await Task.Delay(PairingPollInterval, cancellationToken).ConfigureAwait(false);
                        continue;
                    }

                    SetView(
                        ControllerLinkPhase.WaitingForConnection,
                        PairingProblem(status));
                    return;
                }

                await Task.Delay(PairingPollInterval, cancellationToken).ConfigureAwait(false);
                status = await management.PairingStatusAsync(cancellationToken).ConfigureAwait(false);
                Interlocked.Exchange(ref remotePairingActive, status.Active ? 1 : 0);
            }

            if (AttemptCurrent(generation))
            {
                SetView(
                    ControllerLinkPhase.WaitingForConnection,
                    "The adapter did not connect before its pairing window closed.");
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            diagnostics.Warn("controller-link", $"adapter connect workflow failed: {error.Message}");
            if (AttemptCurrent(generation))
            {
                SetView(ControllerLinkPhase.WaitingForConnection, ProductError(error));
            }
        }
    }

    private bool AttemptCurrent(int generation)
    {
        lock (stateGate)
        {
            return host is not null && !hostConnected && connectionGeneration == generation;
        }
    }

    private void OnHostClosed(string reason)
    {
        if (Volatile.Read(ref stopping) != 0 || host is null)
        {
            return;
        }

        _ = HandleUnexpectedLossAsync(reason);
    }

    private async Task HandleUnexpectedLossAsync(string reason)
    {
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            if (host is null || Volatile.Read(ref stopping) != 0)
            {
                return;
            }

            Interlocked.Exchange(ref stopping, 1);
            input.Neutralize();
            ApplyOutput(RumbleRequest.None);
            await TearDownLockedAsync(callHostStop: false).ConfigureAwait(false);
            diagnostics.Error("controller-link", $"host lost: {reason}; {Metrics.Summary()}");
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
            if (host is null && view.Value.Phase is
                ControllerLinkPhase.Unavailable or ControllerLinkPhase.Stopped)
            {
                SetView(ControllerLinkPhase.Ready);
            }

            return;
        }

        if (host is null)
        {
            SetView(ControllerLinkPhase.Unavailable, management.UnavailableReason);
            return;
        }

        _ = StopForManagementLossAsync();
    }

    private async Task StopForManagementLossAsync()
    {
        diagnostics.Warn("controller-link", "trusted management connection lost; stopping safely");
        await StopAsync().ConfigureAwait(false);
        SetView(ControllerLinkPhase.Unavailable, management.UnavailableReason);
    }

    private async Task TearDownLockedAsync(
        bool callHostStop,
        CancellationToken cancellationToken = default)
    {
        var currentCancellation = sessionCancellation;
        sessionCancellation = null;
        currentCancellation?.Cancel();

        var currentHost = host;
        host = null;
        lock (stateGate)
        {
            hostConnected = false;
            connectionGeneration++;
        }

        if (currentHost is not null)
        {
            Interlocked.Exchange(ref finalReportsQueued, currentHost.InputReportsQueued);
            Interlocked.Exchange(ref finalReportsSent, currentHost.InputReportsSent);
            Interlocked.Exchange(ref finalReportsCoalesced, currentHost.InputReportsCoalesced);
            Interlocked.Exchange(ref finalOutputReportsReceived, currentHost.OutputReportsReceived);
            currentHost.StateChanged -= OnHostStateChanged;
            currentHost.OutputReportReceived -= OnOutputReport;
            currentHost.Closed -= OnHostClosed;
            if (callHostStop)
            {
                try
                {
                    await currentHost.StopAsync(cancellationToken).ConfigureAwait(false);
                }
                catch (Exception error) when (error is not OperationCanceledException)
                {
                    diagnostics.Warn("controller-link", $"helper stop failed: {error.Message}");
                }
            }

            await currentHost.DisposeAsync().ConfigureAwait(false);
        }

        if (Interlocked.Exchange(ref remotePairingActive, 0) != 0 && management.Ready)
        {
            try
            {
                await management.CancelPairingAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                diagnostics.Debug("controller-link", $"pairing cancel after stop failed: {error.Message}");
            }
        }

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

        currentCancellation?.Dispose();
        input.Neutralize();
        ApplyOutput(RumbleRequest.None);
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

    private static string PairingProblem(PairingStatus status) => status.State switch
    {
        PairingState.Blocked => status.Reason switch
        {
            PairingReason.StorageFull => "The adapter's controller bond store is full.",
            PairingReason.ManagementDisabled => "Controller pairing is disabled on the adapter.",
            PairingReason.LockedOut => "The adapter temporarily locked controller pairing.",
            PairingReason.Busy => "The adapter is busy with another pairing request.",
            _ => "The adapter refused to start controller pairing.",
        },
        PairingState.TimedOut => "The adapter did not find this controller before pairing timed out.",
        PairingState.Cancelled => "Controller pairing was cancelled.",
        _ => "The adapter is waiting for this controller.",
    };

    private static string ProductError(Exception error)
    {
        var message = error.Message;
        if (message.Contains("packaged", StringComparison.OrdinalIgnoreCase))
        {
            return "Controller Link requires the packaged PicoSwitch Companion build.";
        }

        if (message.Contains("out of sync", StringComparison.OrdinalIgnoreCase))
        {
            return "Installed Controller Link components are out of sync.";
        }

        if (message.Contains("advertis", StringComparison.OrdinalIgnoreCase))
        {
            return "Windows could not start Bluetooth controller advertising.";
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
        totalOutputLatencyTicks = 0;
        maximumOutputLatencyTicks = 0;
        shapedLeft = 0;
        shapedRight = 0;
        finalReportsQueued = 0;
        finalReportsSent = 0;
        finalReportsCoalesced = 0;
        finalOutputReportsReceived = 0;
        remotePairingActive = 0;
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

public sealed record ControllerLinkMetrics(
    long ReportsGenerated,
    long ReportsQueued,
    long ReportsSent,
    long ReportsCoalesced,
    TimeSpan AverageReportInterval,
    TimeSpan MaximumReportInterval,
    long OutputReportsReceived,
    long OutputReportsDecoded,
    long MalformedOutputReports,
    long OutputDeliveryFailures,
    TimeSpan AverageOutputPipeLatency,
    TimeSpan MaximumOutputPipeLatency)
{
    public string Summary() =>
        $"generated={ReportsGenerated} queued={ReportsQueued} sent={ReportsSent} " +
        $"coalesced={ReportsCoalesced} intervalAvgMs={AverageReportInterval.TotalMilliseconds:F2} " +
        $"intervalMaxMs={MaximumReportInterval.TotalMilliseconds:F2} output={OutputReportsReceived}/" +
        $"{OutputReportsDecoded} malformed={MalformedOutputReports} outputFailures={OutputDeliveryFailures} " +
        $"outputPipeAvgMs={AverageOutputPipeLatency.TotalMilliseconds:F2} " +
        $"outputPipeMaxMs={MaximumOutputPipeLatency.TotalMilliseconds:F2}";
}
