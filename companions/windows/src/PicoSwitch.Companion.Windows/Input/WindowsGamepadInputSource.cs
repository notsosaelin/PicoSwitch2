using System.Diagnostics;
using PicoSwitch.Bridge.Core;
using Windows.Devices.Power;
using Windows.Gaming.Input;

namespace PicoSwitch.Companion.Windows.Input;

/// <summary>
/// Windows.Gaming.Input adapter for Controller Link. One selected controller is
/// sampled at the bridge cadence and published as one complete normalized frame.
///
/// ## Which controller, and how it is chosen
///
/// Enumeration and selection belong to
/// <see cref="WindowsControllerSourceCatalog"/> and
/// <see cref="ControllerSourceSelection"/>: RawGameController is the superset
/// surface, HID supplies real names and the built-in/external distinction, and
/// the shared ambiguity rule decides when the user must choose. This class does
/// not re-decide any of that — it holds the resolved selection and polls it.
///
/// ## What it can and cannot read
///
/// Reading named buttons and axes needs <c>Windows.Gaming.Input.Gamepad</c>,
/// which covers XInput-class devices only. A selected source that is not
/// gamepad-class is held and reported through <see cref="UnreadableReason"/>
/// rather than silently producing nothing — which is what the first
/// implementation did for every non-Xbox controller.
/// </summary>
public sealed class WindowsGamepadInputSource
    : IControllerOutputBackend, IControllerInputSampler, IAsyncDisposable
{
    private static readonly AxisRange StickRange = new(-1f, 1f);
    private static readonly AxisRange TriggerRange = new(0f, 1f);
    /// <summary>
    /// Fallback cadence for keeping state fresh when NOTHING is streaming.
    ///
    /// Not the realtime path. While Controller Link streams, the publisher calls
    /// <see cref="Sample"/> immediately before it encodes, so the controller is
    /// read at the moment its state is used and this loop stands down. It exists
    /// so the UI can show a controller responding before Start is pressed, which
    /// does not need a tight period.
    /// </summary>
    private static readonly TimeSpan IdlePollInterval = TimeSpan.FromMilliseconds(16);

    /// <summary>
    /// How often the battery is read. Minutes-scale data, polled accordingly.
    /// </summary>
    private const double BatteryPollMs = 30_000;

    private readonly Lock gate = new();
    private readonly CancellationTokenSource lifetime = new();
    private readonly WindowsControllerSourceCatalog catalog = new();
    private readonly Task pollTask;
    private Gamepad? selected;
    private ControllerSourceIdentity? selectedIdentity;
    private WindowsControllerSource? selectedSource;
    private IReadOnlyList<WindowsControllerSource> sources = [];
    private string? rememberedId;
    private string? unresolvedReason;
    private long framesPublished;
    private long pollFailures;
    private long lastSampleTicks;
    private long lastBatteryTicks;
    private ControllerBattery lastBattery = ControllerBattery.Unknown;
    private bool disposed;

    public WindowsGamepadInputSource()
    {
        Gamepad.GamepadAdded += OnGamepadAdded;
        Gamepad.GamepadRemoved += OnGamepadRemoved;
        pollTask = PollAsync(lifetime.Token);
    }

    /// <summary>Everything Windows currently offers, usable or not.</summary>
    public IReadOnlyList<WindowsControllerSource> Sources
    {
        get { lock (gate) { return sources; } }
    }

    /// <summary>The resolved source, or null when none is usable or the user must choose.</summary>
    public WindowsControllerSource? SelectedSource
    {
        get { lock (gate) { return selectedSource; } }
    }

    /// <summary>
    /// Why nothing is selected, or why the selection cannot produce input.
    /// Null when a readable source is held.
    /// </summary>
    public string? UnreadableReason
    {
        get
        {
            lock (gate)
            {
                return selectedSource?.UnreadableReason ?? unresolvedReason;
            }
        }
    }

    public event Action? SourcesChanged;

    /// <summary>
    /// Re-enumerate and re-resolve. Safe to call at any time; the poll loop
    /// picks up the new selection on its next tick.
    /// </summary>
    /// <param name="adapterPersonality">
    /// What the connected adapter is emulating, so its own USB output is kept
    /// out of automatic selection.
    /// </param>
    public async Task RefreshAsync(
        string? adapterPersonality = null,
        CancellationToken cancellationToken = default)
    {
        var found = await catalog.EnumerateAsync(adapterPersonality, cancellationToken)
            .ConfigureAwait(false);

        string? remembered;
        lock (gate)
        {
            remembered = rememberedId;
        }

        var resolved = ControllerSourceSelection.Resolve(found, remembered);
        var reason = ControllerSourceSelection.UnresolvedReason(found, resolved);

        lock (gate)
        {
            sources = found;
            selectedSource = resolved;
            unresolvedReason = reason;
            BindLocked(resolved);
        }

        SourcesChanged?.Invoke();
    }

    /// <summary>
    /// Remember an explicit choice. It outranks every heuristic, including the
    /// adapter-echo guard — someone may be testing that loop on purpose. Pass
    /// null to return to automatic selection.
    /// </summary>
    public Task SelectAsync(string? sourceId, string? adapterPersonality = null)
    {
        lock (gate)
        {
            rememberedId = sourceId;
        }

        return RefreshAsync(adapterPersonality);
    }

    /// <summary>
    /// Attach the WinRT Gamepad projection for the resolved source, when there
    /// is one. A non-gamepad-class source still resolves and is still reported;
    /// it simply has no projection to read.
    /// </summary>
    private void BindLocked(WindowsControllerSource? source)
    {
        selected = null;
        selectedIdentity = null;
        if (source is null || !source.IsGamepadClass)
        {
            return;
        }

        foreach (var pad in Gamepad.Gamepads)
        {
            var raw = RawGameController.FromGameController(pad);
            if (raw?.NonRoamableId != source.Id)
            {
                continue;
            }

            selected = pad;
            selectedIdentity = new ControllerSourceIdentity(
                source.Id,
                source.Name,
                (ushort)source.Candidate.VendorId,
                (ushort)source.Candidate.ProductId);
            return;
        }
    }

    /// <summary>
    /// The selected controller's battery, when Windows will tell us.
    ///
    /// Separate from <see cref="Frame"/> and raised far more rarely: a charge
    /// level changes on the scale of minutes, and folding it into the gameplay
    /// frame would put a WinRT battery query on the realtime path for a value
    /// that almost never differs.
    /// </summary>
    public event Action<ControllerBattery>? BatteryChanged;

    public event Action<ControllerSourceIdentity, ControllerButtonSet, AnalogFrame>? Frame;

    public event Action<ControllerSourceIdentity>? Removed;

    public long FramesPublished => Interlocked.Read(ref framesPublished);

    public long PollFailures => Interlocked.Read(ref pollFailures);

    public ControllerSourceIdentity? Selected
    {
        get
        {
            lock (gate)
            {
                return selectedIdentity;
            }
        }
    }

    public void Apply(RumbleRequest request)
    {
        Gamepad? gamepad;
        lock (gate)
        {
            gamepad = selected;
        }

        if (gamepad is null)
        {
            return;
        }

        try
        {
            gamepad.Vibration = new GamepadVibration
            {
                LeftMotor = Math.Clamp(request.Left / 255d, 0d, 1d),
                RightMotor = Math.Clamp(request.Right / 255d, 0d, 1d),
            };
        }
        catch (Exception) when (!disposed)
        {
            Interlocked.Increment(ref pollFailures);
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (disposed)
        {
            return;
        }

        Gamepad.GamepadAdded -= OnGamepadAdded;
        Gamepad.GamepadRemoved -= OnGamepadRemoved;
        Apply(RumbleRequest.None);
        disposed = true;
        lifetime.Cancel();
        try
        {
            await pollTask.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
        }

        lifetime.Dispose();
    }

    /// <summary>
    /// Read the selected controller NOW and publish one frame.
    ///
    /// Called by the Controller Link publisher immediately before it encodes, so
    /// the state that goes on the wire is the state at encode time rather than
    /// whatever a separate timer last left behind.
    /// <c>GetCurrentReading</c> is a cheap local read of state the driver has
    /// already delivered — this does not talk to the radio.
    /// </summary>
    public void Sample()
    {
        Gamepad? gamepad;
        ControllerSourceIdentity? identity;
        lock (gate)
        {
            gamepad = selected;
            identity = selectedIdentity;
        }

        if (gamepad is null || identity is null)
        {
            return;
        }

        try
        {
            var reading = gamepad.GetCurrentReading();
            Frame?.Invoke(identity, Buttons(reading.Buttons), Analog(reading));
            Interlocked.Increment(ref framesPublished);
            Volatile.Write(ref lastSampleTicks, Stopwatch.GetTimestamp());
        }
        catch (Exception) when (!disposed)
        {
            Interlocked.Increment(ref pollFailures);
        }
    }

    /// <summary>
    /// Idle freshness only.
    /// </summary>
    /// <remarks>
    /// While Controller Link streams, the publisher calls <see cref="Sample"/>
    /// on every tick and this loop has nothing to add — so it stands down rather
    /// than doing the same read twice and racing itself for the newest frame.
    /// It exists for everything else that wants live state without a stream
    /// running (the UI showing a controller responding before Start is pressed).
    /// </remarks>
    private async Task PollAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(IdlePollInterval);
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
        {
            var sinceMs = (Stopwatch.GetTimestamp() - Volatile.Read(ref lastSampleTicks))
                          * 1000.0 / Stopwatch.Frequency;
            if (sinceMs < IdlePollInterval.TotalMilliseconds)
            {
                // A just-in-time consumer is driving; leave it alone rather than
                // doing the same read twice and racing it for the newest frame.
                continue;
            }

            Sample();
            SampleBattery();
        }
    }

    /// <summary>
    /// Poll the selected controller's battery, and report only real changes.
    /// </summary>
    /// <remarks>
    /// Windows reports capacity in milliwatt-hours, not percent, and reports
    /// NOTHING at all for plenty of real controllers -- a wired pad has no
    /// battery, and several wireless ones decline to say. That is why the reading
    /// carries a Valid flag rather than defaulting to zero: a console showing a
    /// confident 0% for a controller that never reported is worse than one
    /// showing nothing, and the encoder only sets FlagBatteryValid when we
    /// genuinely know.
    /// </remarks>
    private void SampleBattery()
    {
        var now = Stopwatch.GetTimestamp();
        if ((now - lastBatteryTicks) * 1000.0 / Stopwatch.Frequency < BatteryPollMs)
        {
            return;
        }

        lastBatteryTicks = now;

        Gamepad? gamepad;
        lock (gate)
        {
            gamepad = selected;
        }

        var reading = ReadBattery(gamepad);
        if (reading == lastBattery)
        {
            return;
        }

        lastBattery = reading;
        BatteryChanged?.Invoke(reading);
    }

    private static ControllerBattery ReadBattery(Gamepad? gamepad)
    {
        if (gamepad is null)
        {
            return ControllerBattery.Unknown;
        }

        try
        {
            var report = gamepad.TryGetBatteryReport();
            if (report?.RemainingCapacityInMilliwattHours is not { } remaining ||
                report.FullChargeCapacityInMilliwattHours is not { } full ||
                full <= 0)
            {
                return ControllerBattery.Unknown;
            }

            var percent = (int)Math.Round(100.0 * remaining / full);
            return new ControllerBattery(
                Math.Clamp(percent, 0, 100),
                Charging: report.Status == global::Windows.System.Power.BatteryStatus.Charging,
                Valid: true);
        }
        catch (Exception)
        {
            // A controller that vanished mid-query must not take the poll loop
            // with it; the next tick reports Unknown through the ordinary path.
            return ControllerBattery.Unknown;
        }
    }

    // Arrival re-resolves through the catalog rather than grabbing whatever
    // appeared: "first gamepad wins" is exactly the rule that made the adapter's
    // own USB echo selectable.
    private void OnGamepadAdded(object? sender, Gamepad gamepad) => _ = RefreshAsync();

    private void OnGamepadRemoved(object? sender, Gamepad gamepad)
    {
        ControllerSourceIdentity? removed = null;
        lock (gate)
        {
            if (!ReferenceEquals(selected, gamepad))
            {
                return;
            }

            try
            {
                gamepad.Vibration = new GamepadVibration();
            }
            catch
            {
            }

            removed = selectedIdentity;
            selected = null;
            selectedIdentity = null;
        }

        // Tell downstream before re-resolving, so a late poll cannot publish a
        // frame attributed to a controller that has already gone.
        if (removed is not null)
        {
            Removed?.Invoke(removed);
        }

        _ = RefreshAsync();
    }

    private static ControllerButtonSet Buttons(GamepadButtons buttons)
    {
        var result = ControllerButtonSet.Empty;
        result = result.With(ControllerButton.A, buttons.HasFlag(GamepadButtons.A));
        result = result.With(ControllerButton.B, buttons.HasFlag(GamepadButtons.B));
        result = result.With(ControllerButton.X, buttons.HasFlag(GamepadButtons.X));
        result = result.With(ControllerButton.Y, buttons.HasFlag(GamepadButtons.Y));
        result = result.With(ControllerButton.L1, buttons.HasFlag(GamepadButtons.LeftShoulder));
        result = result.With(ControllerButton.R1, buttons.HasFlag(GamepadButtons.RightShoulder));
        result = result.With(ControllerButton.Select, buttons.HasFlag(GamepadButtons.View));
        result = result.With(ControllerButton.Start, buttons.HasFlag(GamepadButtons.Menu));
        result = result.With(ControllerButton.LeftStick, buttons.HasFlag(GamepadButtons.LeftThumbstick));
        result = result.With(ControllerButton.RightStick, buttons.HasFlag(GamepadButtons.RightThumbstick));
        return result;
    }

    private static AnalogFrame Analog(GamepadReading reading) => new(
        StickRange.Stick((float)reading.LeftThumbstickX),
        StickRange.Stick((float)reading.LeftThumbstickY, invert: true),
        StickRange.Stick((float)reading.RightThumbstickX),
        StickRange.Stick((float)reading.RightThumbstickY, invert: true),
        TriggerRange.Trigger((float)reading.LeftTrigger),
        TriggerRange.Trigger((float)reading.RightTrigger),
        new DpadState(
            reading.Buttons.HasFlag(GamepadButtons.DPadUp),
            reading.Buttons.HasFlag(GamepadButtons.DPadRight),
            reading.Buttons.HasFlag(GamepadButtons.DPadDown),
            reading.Buttons.HasFlag(GamepadButtons.DPadLeft)));
}
