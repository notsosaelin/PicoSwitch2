using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

public enum CompanionAudioPhase
{
    /// <summary>No trusted adapter session.</summary>
    Unavailable,

    /// <summary>Ready to start; console audio plays on the console.</summary>
    Off,

    Starting,

    /// <summary>Subscribed and playing console audio on this device.</summary>
    Playing,

    /// <summary>The adapter refused; <see cref="CompanionAudioView.Detail"/> says why.</summary>
    Refused,

    /// <summary>
    /// The adapter ended a playing session on its own -- typically because a
    /// headset was plugged into the controller in use, which always takes the
    /// console's audio. <see cref="CompanionAudioView.Detail"/> says why.
    /// </summary>
    Ended,

    Error,
}

/// <param name="ControllerLink">Whether this device is the console's controller
/// right now (Controller Link streaming). Companion audio is the headset jack
/// OF that controller, so it can only be turned on while it is.</param>
/// <param name="AwaitingConsole">Playing, but the console has not opened its
/// speaker stream yet. It does so only once it has registered the controller,
/// which takes one input -- a button press or a stick movement.</param>
/// <param name="Microphone">This device's microphone is live: the adapter
/// advertises a headset (not headphones) and receives the uplink.</param>
/// <param name="MicrophoneDetail">Why the microphone the player asked for is
/// not live, or null.</param>
public sealed record CompanionAudioView(
    CompanionAudioPhase Phase,
    string? Detail = null,
    bool ControllerLink = true,
    bool AwaitingConsole = false,
    bool Microphone = false,
    string? MicrophoneDetail = null)
{
    public bool On => Phase is CompanionAudioPhase.Starting or CompanionAudioPhase.Playing;

    /// <summary>Turning off is always allowed; turning on needs Controller Link.</summary>
    public bool CanToggle =>
        Phase is not CompanionAudioPhase.Unavailable and not CompanionAudioPhase.Starting &&
        (On || ControllerLink);
}

public sealed record CompanionAudioMetrics(
    long Notifications,
    long FramesDecoded,
    long FramesLost,
    long FramesConcealed,
    long FramesStale,
    long FramesMalformed,
    long DecodeErrors,
    long SinkDrops,
    long SinkUnderruns,
    int QueuedMillis,
    int PrebufferMillis = 0,
    long BufferGrowths = 0)
{
    public static CompanionAudioMetrics Empty { get; } = new(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/// <summary>
/// The only management surface companion audio may consume -- the audio
/// equivalent of <see cref="IControllerLinkManagement"/>.
/// </summary>
public interface ICompanionAudioManagement
{
    bool Ready { get; }

    string? UnavailableReason { get; }

    event Action? Changed;

    /// <summary>
    /// <c>caudio start</c> or <c>caudio start mic</c>. On a running session it
    /// changes the accessory in place (headphones/headset re-plug).
    /// </summary>
    Task<CompanionAudioState> StartAsync(bool microphone, CancellationToken cancellationToken = default);

    Task<CompanionAudioState> StopAsync(CancellationToken cancellationToken = default);

    Task<CompanionAudioState> StatusAsync(CancellationToken cancellationToken = default);

    ICompanionAudioDownlink? TryCreateDownlink();

    ICompanionMicUplink? TryCreateMicUplink();
}

public sealed class CompanionAudioManagement(AdapterConnectionService adapters)
    : ICompanionAudioManagement
{
    private readonly ControllerLinkManagement readiness = new(adapters);

    public event Action? Changed;

    public bool Ready => readiness.Ready;

    public string? UnavailableReason => readiness.UnavailableReason;

    public CompanionAudioManagement Subscribe()
    {
        adapters.Connection.Changed += OnChanged;
        adapters.Relationship.Changed += OnChanged;
        adapters.Snapshot.Changed += OnChanged;
        return this;
    }

    public Task<CompanionAudioState> StartAsync(bool microphone, CancellationToken cancellationToken = default) =>
        adapters.StartCompanionAudioAsync(microphone, cancellationToken);

    public Task<CompanionAudioState> StopAsync(CancellationToken cancellationToken = default) =>
        adapters.StopCompanionAudioAsync(cancellationToken);

    public Task<CompanionAudioState> StatusAsync(CancellationToken cancellationToken = default) =>
        adapters.CompanionAudioStatusAsync(cancellationToken);

    public ICompanionAudioDownlink? TryCreateDownlink() =>
        adapters.AudioDownlinks?.TryCreateAudioDownlink();

    public ICompanionMicUplink? TryCreateMicUplink() =>
        adapters.MicUplinks?.TryCreateMicUplink();

    private void OnChanged() => Changed?.Invoke();
}

/// <summary>
/// Companion audio (EXPERIMENTAL): plays the console's audio on this device.
///
/// Turning it on asks the adapter to advertise a headphone jack, which makes
/// the console route its audio to the adapter's USB speaker stream; the adapter
/// encodes that as Opus and notifies it here over the management link. Turning
/// it off -- or losing the management session -- returns audio to the console.
///
/// The session never outlives management: on management loss the local half
/// is torn down, and the adapter ends its half on its own (it stops on
/// management loss too, see <c>btstack_host_companion_audio_stop</c>).
///
/// Companion audio is the headset jack of the controller this device IS: it
/// starts only while Controller Link is streaming and ends when Controller Link
/// stops (the adapter enforces the same rule, <c>ns2_companion_audio.h</c> SCOPE).
///
/// The adapter can also end a session while management stays up: a headset
/// plugged into the controller in use takes the audio. The data plane carries
/// no end-of-stream message, so the service watches for silence instead.
/// After <see cref="SilenceBeforeCheckMillis"/> without a frame it asks
/// <c>caudio status</c> once; an inactive session is ended locally and the
/// reason shown. Silence alone ends nothing: the console pauses its stream
/// for many seconds in quiet menus while the session stays active.
///
/// Microphone: with <see cref="MicrophoneEnabled"/> the session starts as
/// <c>caudio start mic</c> -- the adapter shows the console a headset rather
/// than headphones -- and a <see cref="CompanionMicSession"/> streams this
/// device's microphone up. Changing the setting while playing re-plugs the
/// accessory without ending the session. Any microphone failure falls back to
/// headphones (speaker only) and says why in
/// <see cref="CompanionAudioView.MicrophoneDetail"/>: the microphone never
/// costs the player the speaker.
/// </summary>
public sealed class CompanionAudioService : IAsyncDisposable
{
    public const int SilenceBeforeCheckMillis = 2_000;
    private const int WatchdogPeriodMillis = 1_000;

    private readonly ICompanionAudioManagement management;
    private readonly ICompanionAudioSinkFactory sinks;
    private readonly Func<ICompanionAudioDecoder> decoders;
    private readonly bool startWatchdogTimer;
    private readonly DiagnosticLog diagnostics;
    private readonly StateValue<CompanionAudioView> view;
    private readonly SemaphoreSlim lifecycle = new(1, 1);
    private readonly object receiveGate = new();
    private readonly Func<long> clockMillis;
    private readonly IReadOnlyStateValue<bool>? controllerLink;
    private readonly ICompanionMicCaptureFactory? micCaptures;
    private readonly Func<ICompanionMicEncoder> micEncoders;

    private ICompanionAudioDownlink? downlink;
    private CompanionMicSession? micSession;
    private CompanionMicMetrics finalMicMetrics = CompanionMicMetrics.Empty;
    private volatile bool microphoneEnabled;
    private string? microphoneDetail;
    private ICompanionAudioSink? sink;
    private CompanionAudioReceiver? receiver;
    private long sinkDrops;
    private CompanionAudioMetrics finalMetrics = CompanionAudioMetrics.Empty;
    private bool disposed;
    private Timer? watchdog;
    private long lastFrameMillis;
    private int watchdogBusy;
    private volatile bool awaitingConsole;

    /// <param name="clockMillis">Monotonic milliseconds; injectable so the
    /// silence watchdog is testable. Defaults to <see cref="Environment.TickCount64"/>.</param>
    /// <param name="startWatchdogTimer">False in tests, which drive
    /// <see cref="CheckAdapterSessionAsync"/> directly.</param>
    /// <param name="controllerLink">True while Controller Link is streaming.
    /// Null means "always": only tests that are not about the gate omit it.</param>
    /// <param name="micCaptures">The platform microphone; null disables the
    /// microphone (the setting then reports why).</param>
    public CompanionAudioService(
        ICompanionAudioManagement management,
        ICompanionAudioSinkFactory sinks,
        DiagnosticLog diagnostics,
        Func<ICompanionAudioDecoder>? decoders = null,
        Func<long>? clockMillis = null,
        bool startWatchdogTimer = true,
        IReadOnlyStateValue<bool>? controllerLink = null,
        ICompanionMicCaptureFactory? micCaptures = null,
        Func<ICompanionMicEncoder>? micEncoders = null)
    {
        this.micCaptures = micCaptures;
        this.micEncoders = micEncoders ?? (() => new ConcentusCompanionMicEncoder());
        this.management = management;
        this.sinks = sinks;
        this.diagnostics = diagnostics;
        this.decoders = decoders ?? (() => new ConcentusCompanionAudioDecoder());
        this.clockMillis = clockMillis ?? (() => Environment.TickCount64);
        this.startWatchdogTimer = startWatchdogTimer;
        this.controllerLink = controllerLink;
        view = new StateValue<CompanionAudioView>(InitialView());
        management.Changed += OnManagementChanged;
        if (controllerLink is not null)
        {
            controllerLink.Changed += OnControllerLinkChanged;
        }
    }

    private bool ControllerLinkActive => controllerLink?.Value ?? true;

    public IReadOnlyStateValue<CompanionAudioView> View => view;

    public CompanionAudioMetrics Metrics
    {
        get
        {
            lock (receiveGate)
            {
                return receiver is null ? finalMetrics : Snapshot();
            }
        }
    }

    public CompanionMicMetrics MicMetrics => micSession?.Metrics ?? finalMicMetrics;

    /// <summary>Whether the player wants this device's microphone in the headset.</summary>
    public bool MicrophoneEnabled => microphoneEnabled;

    public Task ToggleAsync(CancellationToken cancellationToken = default) =>
        view.Value.On ? StopAsync(cancellationToken) : StartAsync(cancellationToken);

    /// <summary>
    /// Set the microphone preference. While playing, re-plugs the accessory on
    /// the adapter (headphones to headset or back) and opens or closes the
    /// uplink; otherwise it applies to the next start.
    /// </summary>
    public async Task SetMicrophoneAsync(bool enabled, CancellationToken cancellationToken = default)
    {
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            microphoneEnabled = enabled;
            microphoneDetail = null;
            if (disposed || downlink is null)
            {
                view.Set(view.Value with { MicrophoneDetail = null });
                return;
            }

            if (enabled == (micSession is not null))
            {
                PublishPlaying();
                return;
            }

            if (!enabled)
            {
                await CloseMicAsync().ConfigureAwait(false);
                await RestartAccessoryAsync(microphone: false, cancellationToken).ConfigureAwait(false);
                PublishPlaying();
                return;
            }

            CompanionAudioState state;
            try
            {
                state = await management.StartAsync(microphone: true, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                microphoneDetail = $"The adapter did not answer the microphone request: {ex.Message}";
                PublishPlaying();
                return;
            }

            if (!state.Active)
            {
                // The session itself ended between ticks; the watchdog reports it.
                PublishPlaying();
                return;
            }

            if (state.Mic)
            {
                await OpenMicAsync(state, cancellationToken).ConfigureAwait(false);
            }
            else
            {
                // A running session answers a refused re-plug with its status
                // (still active, admit = mic_unsupported): stay headphones.
                microphoneDetail = RefusalText(state);
            }

            PublishPlaying();
        }
        finally
        {
            lifecycle.Release();
        }
    }

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        await lifecycle.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (disposed || downlink is not null)
            {
                return;
            }

            if (!management.Ready)
            {
                view.Set(new CompanionAudioView(
                    CompanionAudioPhase.Unavailable, management.UnavailableReason, ControllerLinkActive));
                return;
            }

            if (!ControllerLinkActive)
            {
                view.Set(InitialView());
                return;
            }

            view.Set(new CompanionAudioView(CompanionAudioPhase.Starting));
            microphoneDetail = null;

            CompanionAudioState state;
            try
            {
                var wantMic = microphoneEnabled;
                state = await management.StartAsync(wantMic, cancellationToken).ConfigureAwait(false);
                if (wantMic && !state.Active && state.Admit == "mic_unsupported")
                {
                    // The speaker is not the microphone's hostage: play without it.
                    microphoneDetail = RefusalText(state);
                    state = await management.StartAsync(microphone: false, cancellationToken).ConfigureAwait(false);
                }
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Fail($"The adapter did not answer the audio request: {ex.Message}");
                return;
            }

            if (!state.Active)
            {
                view.Set(new CompanionAudioView(
                    CompanionAudioPhase.Refused, RefusalText(state), ControllerLinkActive));
                diagnostics.Info("companion-audio", $"refused: admit={state.Admit}");
                return;
            }

            var geometry = GeometryProblem(state);
            if (geometry is not null)
            {
                await TellAdapterStopAsync().ConfigureAwait(false);
                Fail(geometry);
                return;
            }

            var plane = management.TryCreateDownlink();
            if (plane is null || !await plane.OpenAsync(cancellationToken).ConfigureAwait(false))
            {
                if (plane is not null)
                {
                    await plane.DisposeAsync().ConfigureAwait(false);
                }

                await TellAdapterStopAsync().ConfigureAwait(false);
                Fail("This adapter's firmware does not expose the audio channel.");
                return;
            }

            var output = sinks.CreateSink();
            if (output is null)
            {
                await plane.DisposeAsync().ConfigureAwait(false);
                await TellAdapterStopAsync().ConfigureAwait(false);
                Fail("No audio output device is available on this computer.");
                return;
            }

            try
            {
                output.Start(CompanionAudioDataPlane.SampleRate, CompanionAudioDataPlane.Channels);
            }
            catch (Exception ex)
            {
                output.Dispose();
                await plane.DisposeAsync().ConfigureAwait(false);
                await TellAdapterStopAsync().ConfigureAwait(false);
                Fail($"Could not open the audio output: {ex.Message}");
                return;
            }

            lock (receiveGate)
            {
                sink = output;
                sinkDrops = 0;
                receiver = new CompanionAudioReceiver(decoders(), Emit, state.FrameBytes);
            }

            Interlocked.Exchange(ref lastFrameMillis, clockMillis());
            // Frames exist only while the console streams to the adapter's USB
            // speaker, so the first frame is what clears this.
            awaitingConsole = !state.UsbSpeakerActive;
            downlink = plane;
            plane.FrameReceived += OnFrame;
            if (startWatchdogTimer)
            {
                watchdog = new Timer(
                    _ => _ = CheckAdapterSessionAsync(), null, WatchdogPeriodMillis, WatchdogPeriodMillis);
            }

            if (state.Mic)
            {
                await OpenMicAsync(state, cancellationToken).ConfigureAwait(false);
            }

            PublishPlaying();
            diagnostics.Info(
                "companion-audio",
                $"playing: att_mtu={plane.AttMtu} frame_bytes={state.FrameBytes} " +
                $"kbps={CompanionAudioDataPlane.Kbps(state.FrameBytes)} " +
                $"usb_speaker={state.UsbSpeakerActive} mic={micSession is not null}");
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
            var hadSession = await TearDownLocalAsync().ConfigureAwait(false);
            if (hadSession || view.Value.Phase is CompanionAudioPhase.Error)
            {
                await TellAdapterStopAsync().ConfigureAwait(false);
            }

            view.Set(InitialView());
        }
        finally
        {
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
        if (controllerLink is not null)
        {
            controllerLink.Changed -= OnControllerLinkChanged;
        }

        await StopAsync().ConfigureAwait(false);
        disposed = true;
    }

    private void OnFrame(byte[] notification)
    {
        Interlocked.Exchange(ref lastFrameMillis, clockMillis());
        lock (receiveGate)
        {
            receiver?.Accept(notification);
            if (sink?.TakeConcealRequest() == true)
            {
                receiver?.ConcealOne();
            }
        }

        if (awaitingConsole)
        {
            awaitingConsole = false;
            var current = view.Value;
            if (current.Phase is CompanionAudioPhase.Playing)
            {
                view.Set(current with { AwaitingConsole = false });
            }
        }
    }

    /// <summary>
    /// The silence watchdog's check, run every second while playing. Asks the
    /// adapter only after <see cref="SilenceBeforeCheckMillis"/> with no frame,
    /// and then at most once per silence window, so a quiet console costs one
    /// small status exchange every two seconds and a playing one costs nothing.
    /// </summary>
    public async Task CheckAdapterSessionAsync()
    {
        if (Interlocked.Exchange(ref watchdogBusy, 1) == 1)
        {
            return;
        }

        try
        {
            if (downlink is null ||
                clockMillis() - Interlocked.Read(ref lastFrameMillis) < SilenceBeforeCheckMillis)
            {
                return;
            }

            CompanionAudioState state;
            try
            {
                state = await management.StatusAsync().ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                // A busy or lost management channel is handled by its own
                // lifecycle; try again on the next tick.
                return;
            }

            // Start the next silence window from now, so a quiet but active
            // session is re-checked every two seconds, not every tick.
            Interlocked.Exchange(ref lastFrameMillis, clockMillis());
            if (!state.Active)
            {
                await EndForAdapterStopAsync(state.LastStop).ConfigureAwait(false);
            }
        }
        finally
        {
            Interlocked.Exchange(ref watchdogBusy, 0);
        }
    }

    private async Task EndForAdapterStopAsync(string reason)
    {
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            // The adapter has already ended its half; telling it to stop again
            // would be harmless but says nothing.
            if (!await TearDownLocalAsync().ConfigureAwait(false))
            {
                return;
            }

            diagnostics.Info("companion-audio", $"ended by adapter: last_stop={reason}");
            view.Set(new CompanionAudioView(
                CompanionAudioPhase.Ended, EndedText(reason), ControllerLinkActive));
        }
        finally
        {
            lifecycle.Release();
        }
    }

    // Called under receiveGate from the receiver.
    private void Emit(ReadOnlySpan<short> pcm)
    {
        if (sink is not null && !sink.Write(pcm))
        {
            sinkDrops++;
        }
    }

    private void OnManagementChanged()
    {
        if (management.Ready)
        {
            if (downlink is null && view.Value.Phase is CompanionAudioPhase.Unavailable)
            {
                view.Set(InitialView());
            }

            return;
        }

        _ = StopForManagementLossAsync();
    }

    private void OnControllerLinkChanged()
    {
        if (ControllerLinkActive)
        {
            // Nothing to start: the player turns the headset on. Just re-enable it.
            var current = view.Value;
            if (!current.On && current.Phase is not CompanionAudioPhase.Unavailable)
            {
                view.Set(current with { ControllerLink = true });
            }

            return;
        }

        _ = StopForControllerLinkEndAsync();
    }

    /// <summary>
    /// Controller Link stopped, so this device is no longer the controller and
    /// its headset goes with it. The adapter ends its half on its own as well
    /// (<c>controller_link_ended</c>); the stop is sent anyway so the jack is
    /// withdrawn at once rather than on the adapter's next service tick.
    /// </summary>
    private async Task StopForControllerLinkEndAsync()
    {
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            if (await TearDownLocalAsync().ConfigureAwait(false))
            {
                await TellAdapterStopAsync().ConfigureAwait(false);
                diagnostics.Info("companion-audio", "stopped: controller link ended");
                view.Set(new CompanionAudioView(
                    CompanionAudioPhase.Ended, EndedText("controller_link_ended"), ControllerLink: false));
                return;
            }

            var current = view.Value;
            view.Set(current.Phase is CompanionAudioPhase.Unavailable or CompanionAudioPhase.Off
                ? InitialView()
                : current with { ControllerLink = false });
        }
        finally
        {
            lifecycle.Release();
        }
    }

    private async Task StopForManagementLossAsync()
    {
        await lifecycle.WaitAsync().ConfigureAwait(false);
        try
        {
            // The adapter ends its half on management loss by itself; there is
            // nobody to send `caudio stop` to.
            if (await TearDownLocalAsync().ConfigureAwait(false))
            {
                diagnostics.Info("companion-audio", "stopped: management lost");
            }

            view.Set(new CompanionAudioView(
                CompanionAudioPhase.Unavailable, management.UnavailableReason, ControllerLinkActive));
        }
        finally
        {
            lifecycle.Release();
        }
    }

    /// <summary>
    /// Open the uplink for a session the adapter has started with the
    /// microphone. On failure the adapter is re-plugged as headphones so the
    /// console does not wait on a microphone that will never speak.
    /// </summary>
    private async Task OpenMicAsync(CompanionAudioState state, CancellationToken cancellationToken)
    {
        var (session, problem) = await CompanionMicSession.OpenAsync(
            management.TryCreateMicUplink(), micCaptures, micEncoders(), cancellationToken).ConfigureAwait(false);
        if (session is not null)
        {
            micSession = session;
            diagnostics.Info("companion-audio", "microphone: live");
            return;
        }

        microphoneDetail = problem;
        diagnostics.Info("companion-audio", $"microphone: {problem}");
        if (state.Mic)
        {
            await RestartAccessoryAsync(microphone: false, cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task RestartAccessoryAsync(bool microphone, CancellationToken cancellationToken)
    {
        try
        {
            await management.StartAsync(microphone, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            diagnostics.Info("companion-audio", $"re-plug not acknowledged: {ex.Message}");
        }
    }

    private async Task CloseMicAsync()
    {
        var session = micSession;
        micSession = null;
        if (session is null)
        {
            return;
        }

        finalMicMetrics = session.Metrics;
        await session.DisposeAsync().ConfigureAwait(false);
        var m = finalMicMetrics;
        diagnostics.Info(
            "companion-audio",
            $"microphone end: captured={m.BlocksCaptured} encoded={m.FramesEncoded} " +
            $"rejected={m.EncodeRejected} written={m.FramesWritten} " +
            $"displaced={m.FramesDisplaced} failed={m.WriteFailures}");
    }

    private void PublishPlaying() =>
        view.Set(new CompanionAudioView(
            CompanionAudioPhase.Playing,
            AwaitingConsole: awaitingConsole,
            Microphone: micSession is not null,
            MicrophoneDetail: microphoneDetail));

    private async Task<bool> TearDownLocalAsync()
    {
        awaitingConsole = false;
        await CloseMicAsync().ConfigureAwait(false);
        var timer = watchdog;
        watchdog = null;
        if (timer is not null)
        {
            await timer.DisposeAsync().ConfigureAwait(false);
        }

        var plane = downlink;
        downlink = null;
        if (plane is not null)
        {
            plane.FrameReceived -= OnFrame;
            await plane.DisposeAsync().ConfigureAwait(false);
        }

        ICompanionAudioSink? output;
        lock (receiveGate)
        {
            if (receiver is not null)
            {
                finalMetrics = Snapshot(plane);
            }

            output = sink;
            sink = null;
            receiver = null;
        }

        if (output is not null)
        {
            output.Stop();
            output.Dispose();
        }

        if (plane is not null)
        {
            var m = finalMetrics;
            diagnostics.Info(
                "companion-audio",
                $"session end: notifications={m.Notifications} decoded={m.FramesDecoded} " +
                $"lost={m.FramesLost} concealed={m.FramesConcealed} stale={m.FramesStale} " +
                $"malformed={m.FramesMalformed} decode_errors={m.DecodeErrors} " +
                $"sink_drops={m.SinkDrops} underruns={m.SinkUnderruns} " +
                $"prebuffer_ms={m.PrebufferMillis} buffer_growths={m.BufferGrowths}");
        }

        return plane is not null;
    }

    private async Task TellAdapterStopAsync()
    {
        if (!management.Ready)
        {
            return;
        }

        try
        {
            await management.StopAsync().ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            // Best effort: the adapter also ends the session on management loss.
            diagnostics.Info("companion-audio", $"stop not acknowledged: {ex.Message}");
        }
    }

    // Called under receiveGate.
    private CompanionAudioMetrics Snapshot(ICompanionAudioDownlink? plane = null)
    {
        var r = receiver!;
        return new CompanionAudioMetrics(
            Notifications: (plane ?? downlink)?.NotificationsReceived ?? 0,
            FramesDecoded: r.FramesDecoded,
            FramesLost: r.FramesLost,
            FramesConcealed: r.FramesConcealed,
            FramesStale: r.FramesStale,
            FramesMalformed: r.FramesMalformed,
            DecodeErrors: r.DecodeErrors,
            SinkDrops: sinkDrops,
            SinkUnderruns: sink?.Underruns ?? 0,
            QueuedMillis: sink?.QueuedMillis ?? 0,
            PrebufferMillis: sink?.PrebufferMillis ?? 0,
            BufferGrowths: r.FramesGrown);
    }

    private void Fail(string detail)
    {
        view.Set(new CompanionAudioView(CompanionAudioPhase.Error, detail, ControllerLinkActive));
        diagnostics.Info("companion-audio", $"error: {detail}");
    }

    private CompanionAudioView InitialView() =>
        management.Ready
            ? new CompanionAudioView(CompanionAudioPhase.Off, ControllerLink: ControllerLinkActive)
            : new CompanionAudioView(
                CompanionAudioPhase.Unavailable, management.UnavailableReason, ControllerLinkActive);

    /// <summary>
    /// The packets carry no container, so the reported geometry must be one this
    /// decoder was built for. Refuse a mismatch rather than play noise. The frame
    /// size varies with the session's bitrate and sizes the receiver.
    /// </summary>
    public static string? GeometryProblem(CompanionAudioState state) =>
        state.Version == CompanionAudioDataPlane.Version &&
        state.Codec == "opus" &&
        state.SampleRate == CompanionAudioDataPlane.SampleRate &&
        state.Channels == CompanionAudioDataPlane.Channels &&
        state.FrameMillis == CompanionAudioDataPlane.FrameMillis &&
        CompanionAudioDataPlane.FrameBytesSupported(state.FrameBytes)
            ? null
            : $"The adapter's audio format (v{state.Version} {state.Codec} {state.SampleRate} Hz, " +
              $"{state.Channels} ch, {state.FrameMillis} ms, {state.FrameBytes} B) is not one this app can play.";

    public static string RefusalText(CompanionAudioState state) => state.Admit switch
    {
        "unsupported" => "This adapter board cannot stream audio. Audio needs a Pico 2 W.",
        "mtu" => $"The Bluetooth link is too small for audio (MTU {state.AttMtu}, needs {state.MinimumAttMtu}).",
        "encoder_busy" => "A controller headset is already using the adapter's audio.",
        "controller_headset" =>
            "A headset is plugged into your controller, so the console's audio plays there. " +
            "Unplug it to play audio on this PC.",
        "rate" => "The adapter offered an audio bitrate this app does not play. Update the app or the adapter.",
        "no_controller_link" => "Start Controller Link first: the headset belongs to the controller this PC is driving.",
        "other_controller" =>
            "Another controller is driving the console. The headset works only while this device is the controller.",
        "untrusted" => "The adapter connection is not trusted yet.",
        "no_link" => "The adapter is not connected.",
        "mic_unsupported" => "This adapter build cannot take a microphone, so the headset is speaker only.",
        _ when !state.Supported => "This adapter cannot stream audio.",
        _ => $"The adapter refused audio ({state.Admit}).",
    };

    public static string EndedText(string lastStop) => lastStop switch
    {
        "preempted_controller_headset" or "preempted_pro2" =>
            "Audio moved to the headset plugged into your controller.",
        "stopped" => "Audio was turned off on the adapter.",
        "controller_link_ended" => "Controller Link stopped, so console audio is back on the console.",
        "source_changed" => "Another controller took over, so console audio is back on the console.",
        _ => $"The adapter stopped audio ({lastStop}).",
    };
}
