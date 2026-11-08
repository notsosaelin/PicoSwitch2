using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Input;
using SDL;

namespace PicoSwitch.Companion.Linux.Input;

/// <summary>
/// SDL3 gamepad state -> the bridge's normalized controller state. Pure: no SDL
/// call happens here, so every rule is unit-tested without the native library.
///
/// ## Buttons
///
/// Positional, exactly as the Windows backend maps an Xbox-layout pad: the
/// SOUTH face button is <see cref="ControllerButton.A"/>, EAST is B, WEST is X,
/// NORTH is Y. Label-versus-position for Nintendo-style pads is decided later,
/// by <c>ControllerFaceLayout</c>, from the source's vendor and product, so it
/// is not decided twice. SDL exposes more than Windows.Gaming.Input does, and
/// the extra controls map to the console controls they physically correspond
/// to: GUIDE -> Home, MISC1 (Share/Capture/Mic) -> Capture, and the first pair
/// of back paddles (Steam Deck L4/R4, Xbox Elite P3/P1, DualSense Edge
/// Fn/back) -> GL/GR, the Pro Controller 2 back buttons.
///
/// ## Axes
///
/// SDL reports sticks as -32768..32767 with +Y DOWN, and triggers as 0..32767.
/// The bridge's 0..255 stick convention is also +Y down (128 neutral), so no
/// inversion is applied; Windows inverts only because its API reports +Y up.
///
/// ## Motion
///
/// See <see cref="Motion"/>.
/// </summary>
internal static class SdlGamepadMapping
{
    public static readonly AxisRange StickRange = new(-32768f, 32767f);
    public static readonly AxisRange TriggerRange = new(0f, 32767f);

    /// <summary>Valve's USB vendor id.</summary>
    public const ushort ValveVendor = 0x28DE;

    /// <summary>The Steam Deck's built-in controller (LCD and OLED).</summary>
    public const ushort SteamDeckProduct = 0x1205;

    /// <summary>
    /// Steam Input's virtual Xbox 360 pad. Steam creates one per controller it
    /// manages, so on a Steam Deck the built-in controls appear twice: once as
    /// the real device and once as this.
    /// </summary>
    public const ushort SteamVirtualGamepadProduct = 0x11FF;

    public static ControllerButtonSet Buttons(Func<SDL_GamepadButton, bool> pressed)
    {
        var result = ControllerButtonSet.Empty;
        result = result.With(ControllerButton.A, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_SOUTH));
        result = result.With(ControllerButton.B, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_EAST));
        result = result.With(ControllerButton.X, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_WEST));
        result = result.With(ControllerButton.Y, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_NORTH));
        result = result.With(ControllerButton.L1, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
        result = result.With(ControllerButton.R1, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
        result = result.With(ControllerButton.Select, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_BACK));
        result = result.With(ControllerButton.Start, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_START));
        result = result.With(ControllerButton.LeftStick, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_STICK));
        result = result.With(ControllerButton.RightStick, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_STICK));
        result = result.With(ControllerButton.Home, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_GUIDE));
        result = result.With(ControllerButton.Capture, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_MISC1));
        result = result.With(ControllerButton.GL, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_LEFT_PADDLE1));
        result = result.With(ControllerButton.GR, pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1));
        return result;
    }

    public static AnalogFrame Analog(Func<SDL_GamepadAxis, short> axis, Func<SDL_GamepadButton, bool> pressed) =>
        new(
            StickRange.Stick(axis(SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTX)),
            StickRange.Stick(axis(SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFTY)),
            StickRange.Stick(axis(SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHTX)),
            StickRange.Stick(axis(SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHTY)),
            TriggerRange.Trigger(axis(SDL_GamepadAxis.SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
            TriggerRange.Trigger(axis(SDL_GamepadAxis.SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)),
            new DpadState(
                pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_UP),
                pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
                pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                pressed(SDL_GamepadButton.SDL_GAMEPAD_BUTTON_DPAD_LEFT)));

    /// <summary>
    /// One SDL accelerometer + gyroscope pair -> canonical bridge motion.
    ///
    /// ## The frame transform
    ///
    /// SDL defines gamepad sensors for a controller "held in front of you"
    /// (SDL_sensor.h): +X right, +Y up out of the face, +Z toward the player;
    /// gyro positive counter-clockwise (right-hand rule); accelerometer in m/s^2
    /// INCLUDING gravity, so a pad lying flat reads about +9.8 on Y.
    ///
    /// The bridge's canonical frame (<see cref="MotionConvention"/>) is a
    /// handheld held as a controller: +X right, +Y toward the far (top) edge,
    /// +Z out of the face; flat on a table reads +1 g on Z. So:
    ///
    ///     canonical X =  SDL X
    ///     canonical Y = -SDL Z   (the far edge is away from the player)
    ///     canonical Z =  SDL Y   (out of the face)
    ///
    /// A proper rotation (determinant +1), applied identically to both sensors
    /// because angular rate transforms as a vector under rotation. The adapter
    /// then applies its identity companion seam row (SWITCH_MOTION_SOURCE_ANDROID),
    /// unchanged; no firmware change is involved.
    ///
    /// Status: **Hypothesis.** Pinned by gravity-anchor and rotation-sense unit
    /// tests against SDL's documented convention; not yet confirmed with a
    /// console gyro-aim check on a Steam Deck or an external pad.
    ///
    /// ## Timestamp
    ///
    /// The SENSOR timestamp (nanoseconds), never the send time; the adapter
    /// de-duplicates on it. <paramref name="timestampNanos"/> is the gyro
    /// reading's, because angular rate is what the console integrates.
    /// </summary>
    public static ControllerMotion Motion(
        ReadOnlySpan<float> accelMs2,
        ReadOnlySpan<float> gyroRads,
        ulong timestampNanos) =>
        new(
            GyroX: MotionScale.GyroCounts(gyroRads[0]),
            GyroY: MotionScale.GyroCounts(-gyroRads[2]),
            GyroZ: MotionScale.GyroCounts(gyroRads[1]),
            AccelX: MotionScale.AccelCounts(accelMs2[0]),
            AccelY: MotionScale.AccelCounts(-accelMs2[2]),
            AccelZ: MotionScale.AccelCounts(accelMs2[1]),
            TimestampTicks: MotionScale.TimestampTicks((long)(timestampNanos & long.MaxValue)),
            Valid: true);

    /// <summary>
    /// A stable identifier for one physical controller, used to remember the
    /// user's choice across reconnects and restarts. SDL instance ids are
    /// per-connection, so they cannot be it.
    /// </summary>
    public static string Descriptor(ushort vendor, ushort product, string? serial, string? name) =>
        $"sdl:{vendor:x4}:{product:x4}:{(string.IsNullOrWhiteSpace(serial) ? name ?? "unknown" : serial)}";

    public static ControllerAttachment Attachment(ushort vendor, ushort product) =>
        vendor == ValveVendor && product == SteamDeckProduct
            ? ControllerAttachment.BuiltIn
            : ControllerAttachment.External;

    /// <summary>Steam Input's virtual pad: a duplicate of a controller that is listed in its own right.</summary>
    public static bool IsSteamVirtualPad(ushort vendor, ushort product) =>
        vendor == ValveVendor && product == SteamVirtualGamepadProduct;

    public static ControllerConnection Connection(SDL_JoystickConnectionState state) => state switch
    {
        SDL_JoystickConnectionState.SDL_JOYSTICK_CONNECTION_WIRED => ControllerConnection.Usb,
        SDL_JoystickConnectionState.SDL_JOYSTICK_CONNECTION_WIRELESS => ControllerConnection.Bluetooth,
        _ => ControllerConnection.Unknown,
    };

    /// <summary>
    /// SDL power info -> the bridge battery reading. Only a real percentage on a
    /// battery-powered pad is valid; a wired pad or an unreported level stays
    /// <see cref="ControllerBattery.Unknown"/>, so the console never shows a
    /// confident 0% for a controller that never said.
    /// </summary>
    public static ControllerBattery Battery(SDL_PowerState state, int percent) => state switch
    {
        SDL_PowerState.SDL_POWERSTATE_ON_BATTERY when percent >= 0 =>
            new ControllerBattery(Math.Clamp(percent, 0, 100), Charging: false, Valid: true),
        SDL_PowerState.SDL_POWERSTATE_CHARGING when percent >= 0 =>
            new ControllerBattery(Math.Clamp(percent, 0, 100), Charging: true, Valid: true),
        SDL_PowerState.SDL_POWERSTATE_CHARGED =>
            new ControllerBattery(percent >= 0 ? Math.Clamp(percent, 0, 100) : 100, Charging: false, Valid: true),
        _ => ControllerBattery.Unknown,
    };

    /// <summary>Bridge rumble 0..255 -> SDL motor intensity 0..65535.</summary>
    public static ushort Motor(int value) => (ushort)(Math.Clamp(value, 0, 255) * 257);
}
