using System.Diagnostics;
using PicoSwitch.Bridge.Core;
using Windows.Gaming.Input;

namespace PicoSwitch.Companion.Windows.Input;

/// <summary>
/// Windows.Gaming.Input adapter for Controller Link. One selected gamepad is
/// sampled at the bridge cadence and published as one complete normalized frame.
/// The renderer and AppContainer helper never poll physical controllers.
/// </summary>
public sealed class WindowsGamepadInputSource : IControllerOutputBackend, IAsyncDisposable
{
    private static readonly AxisRange StickRange = new(-1f, 1f);
    private static readonly AxisRange TriggerRange = new(0f, 1f);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(8);

    private readonly Lock gate = new();
    private readonly CancellationTokenSource lifetime = new();
    private readonly Task pollTask;
    private Gamepad? selected;
    private ControllerSourceIdentity? selectedIdentity;
    private long framesPublished;
    private long pollFailures;
    private bool disposed;

    public WindowsGamepadInputSource()
    {
        Gamepad.GamepadAdded += OnGamepadAdded;
        Gamepad.GamepadRemoved += OnGamepadRemoved;
        lock (gate)
        {
            SelectFirstAvailableLocked();
        }

        pollTask = PollAsync(lifetime.Token);
    }

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

    private async Task PollAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(PollInterval);
        while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
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
                continue;
            }

            try
            {
                var reading = gamepad.GetCurrentReading();
                Frame?.Invoke(identity, Buttons(reading.Buttons), Analog(reading));
                Interlocked.Increment(ref framesPublished);
            }
            catch (Exception) when (!cancellationToken.IsCancellationRequested)
            {
                Interlocked.Increment(ref pollFailures);
            }
        }
    }

    private void OnGamepadAdded(object? sender, Gamepad gamepad)
    {
        lock (gate)
        {
            selected ??= gamepad;
            selectedIdentity ??= Identity(gamepad);
        }
    }

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
            SelectFirstAvailableLocked();
        }

        if (removed is not null)
        {
            Removed?.Invoke(removed);
        }
    }

    private void SelectFirstAvailableLocked()
    {
        selected = Gamepad.Gamepads.FirstOrDefault();
        selectedIdentity = selected is null ? null : Identity(selected);
    }

    private static ControllerSourceIdentity Identity(Gamepad gamepad)
    {
        var raw = RawGameController.FromGameController(gamepad);
        var id = raw?.NonRoamableId;
        if (string.IsNullOrWhiteSpace(id))
        {
            id = $"windows-gamepad-{System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(gamepad):x8}";
        }

        return new ControllerSourceIdentity(
            id,
            raw?.DisplayName ?? "Windows gamepad",
            raw?.HardwareVendorId ?? 0,
            raw?.HardwareProductId ?? 0);
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
