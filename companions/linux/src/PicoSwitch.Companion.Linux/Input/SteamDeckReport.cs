using System.Buffers.Binary;
using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Companion.Linux.Input;

/// <summary>
/// Valve's Steam Deck controller input report, parsed straight from hidraw. Pure:
/// no I/O, so every rule is unit-tested against captured bytes.
///
/// ## Why it exists
///
/// SDL's Steam Deck driver accepts only the Deck's own product id (0x1205). A
/// controller that speaks the same report format under another id (on Bazzite,
/// InputPlumber's "deck-uhid" target, 28de:12fd, for the ROG Xbox Ally) is left
/// undriven: SDL 3.5.0 lists it `driver = NONE (DISABLED)`. The only pad SDL then
/// offers is Steam Input's virtual Xbox 360 pad, which has NO back-button codes
/// (its evdev key bitmap, read on the Ally 2026-11-06, carries only the eleven
/// Xbox buttons) and no motion. Reading the report ourselves recovers the back
/// buttons, the second back pair, and the IMU.
///
/// ## Layout (64 bytes)
///
///     0      0x01        report id
///     1      0x00
///     2      0x09        message type: controller state
///     3      0x40        payload length (64)
///     4..7   u32         packet number
///     8..11  u32 LE      buttons, low word
///     12..15 u32 LE      buttons, high word
///     16..23 i16 x4      left/right trackpad X, Y
///     24..29 i16 x3      accelerometer X, Y, Z  (+-2 g full scale)
///     30..35 i16 x3      gyroscope X, Y, Z      (+-2000 dps full scale)
///     36..43 i16 x4      orientation quaternion W, X, Y, Z (unused here)
///     44..47 u16 x2      triggers L, R          (0..32767)
///     48..51 i16 x2      left stick X, Y        (+Y UP)
///     52..55 i16 x2      right stick X, Y       (+Y UP)
///     56..59 u16 x2      trackpad pressure L, R
///
/// Evidence: the header, the packet number, the back buttons (high word bit 9 =
/// left, bit 10 = right) and an accelerometer reading 1 g = ~16 670 counts at
/// rest are **Confirmed** by a hidraw capture of 28de:12fd on the Ally while
/// each back button was held (2026-11-06). Every other offset and bit is from
/// SDL's `controller_structs.h` / `SDL_hidapi_steamdeck.c` (the layout SDL
/// drives a real Deck with): **Strong evidence**, not yet checked one control at
/// a time on this hardware.
/// </summary>
internal static class SteamDeckReport
{
    public const int Length = 64;
    public const byte ReportId = 0x01;
    public const byte StateMessage = 0x09;
    public const byte StatePayloadLength = 0x40;

    // Low word (SDL SteamDeckButtons).
    public const uint R2Digital = 0x0000_0001;
    public const uint L2Digital = 0x0000_0002;
    public const uint R1 = 0x0000_0004;
    public const uint L1 = 0x0000_0008;
    public const uint Y = 0x0000_0010;
    public const uint B = 0x0000_0020;
    public const uint X = 0x0000_0040;
    public const uint A = 0x0000_0080;
    public const uint DpadUp = 0x0000_0100;
    public const uint DpadRight = 0x0000_0200;
    public const uint DpadLeft = 0x0000_0400;
    public const uint DpadDown = 0x0000_0800;
    public const uint View = 0x0000_1000;
    public const uint Steam = 0x0000_2000;
    public const uint Menu = 0x0000_4000;
    public const uint L5 = 0x0000_8000;
    public const uint R5 = 0x0001_0000;
    public const uint L3 = 0x0040_0000;
    public const uint R3 = 0x0400_0000;

    // High word. L4/R4 Confirmed on hardware (see the class summary).
    public const uint L4 = 0x0000_0200;
    public const uint R4 = 0x0000_0400;
    public const uint QuickAccess = 0x0004_0000;

    public static readonly AxisRange StickRange = new(-32768f, 32767f);
    public static readonly AxisRange TriggerRange = new(0f, 32767f);

    private const float StandardGravity = 9.80665f;
    private const float GyroRadsPerCount = 2000f / 32768f * (MathF.PI / 180f);
    private const float AccelMs2PerCount = 2f * StandardGravity / 32768f;

    /// <summary>One decoded controller-state report.</summary>
    internal readonly record struct State(
        uint Packet,
        uint ButtonsLow,
        uint ButtonsHigh,
        short AccelX,
        short AccelY,
        short AccelZ,
        short GyroX,
        short GyroY,
        short GyroZ,
        ushort TriggerLeft,
        ushort TriggerRight,
        short LeftX,
        short LeftY,
        short RightX,
        short RightY);

    /// <summary>True when the bytes are a Steam Deck controller-state report.</summary>
    public static bool IsState(ReadOnlySpan<byte> report) =>
        report.Length >= Length &&
        report[0] == ReportId &&
        report[2] == StateMessage &&
        report[3] == StatePayloadLength;

    public static bool TryParse(ReadOnlySpan<byte> report, out State state)
    {
        if (!IsState(report))
        {
            state = default;
            return false;
        }

        state = new State(
            Packet: BinaryPrimitives.ReadUInt32LittleEndian(report[4..]),
            ButtonsLow: BinaryPrimitives.ReadUInt32LittleEndian(report[8..]),
            ButtonsHigh: BinaryPrimitives.ReadUInt32LittleEndian(report[12..]),
            AccelX: S(report, 24),
            AccelY: S(report, 26),
            AccelZ: S(report, 28),
            GyroX: S(report, 30),
            GyroY: S(report, 32),
            GyroZ: S(report, 34),
            TriggerLeft: U(report, 44),
            TriggerRight: U(report, 46),
            LeftX: S(report, 48),
            LeftY: S(report, 50),
            RightX: S(report, 52),
            RightY: S(report, 54));
        return true;

        static short S(ReadOnlySpan<byte> r, int offset) => BinaryPrimitives.ReadInt16LittleEndian(r[offset..]);
        static ushort U(ReadOnlySpan<byte> r, int offset) => BinaryPrimitives.ReadUInt16LittleEndian(r[offset..]);
    }

    /// <summary>
    /// Positional, as SDL maps the Deck: the A label is the SOUTH face button, so
    /// it is <see cref="ControllerButton.A"/>, and so on, exactly like
    /// <see cref="SdlGamepadMapping.Buttons"/>. Back buttons follow SDL's paddle
    /// order: L4/R4 are PADDLE1, the console's GL/GR.
    /// </summary>
    public static ControllerButtonSet Buttons(in State state)
    {
        var low = state.ButtonsLow;
        var high = state.ButtonsHigh;
        bool Low(uint bit) => (low & bit) != 0;
        bool High(uint bit) => (high & bit) != 0;

        var result = ControllerButtonSet.Empty;
        result = result.With(ControllerButton.A, Low(A));
        result = result.With(ControllerButton.B, Low(B));
        result = result.With(ControllerButton.X, Low(X));
        result = result.With(ControllerButton.Y, Low(Y));
        result = result.With(ControllerButton.L1, Low(L1));
        result = result.With(ControllerButton.R1, Low(R1));
        result = result.With(ControllerButton.Select, Low(View));
        result = result.With(ControllerButton.Start, Low(Menu));
        result = result.With(ControllerButton.LeftStick, Low(L3));
        result = result.With(ControllerButton.RightStick, Low(R3));
        result = result.With(ControllerButton.Home, Low(Steam));
        result = result.With(ControllerButton.Capture, High(QuickAccess));
        result = result.With(ControllerButton.GL, High(L4));
        result = result.With(ControllerButton.GR, High(R4));
        return result;
    }

    /// <summary>
    /// Sticks to the bridge's +Y-DOWN convention: the report is +Y UP, so Y is
    /// inverted here (SDL's Deck driver inverts the same way). Triggers are the
    /// analog values; the digital trigger bits are not needed.
    /// </summary>
    public static AnalogFrame Analog(in State state) =>
        new(
            StickRange.Stick(state.LeftX),
            StickRange.Stick(state.LeftY, invert: true),
            StickRange.Stick(state.RightX),
            StickRange.Stick(state.RightY, invert: true),
            TriggerRange.Trigger(state.TriggerLeft),
            TriggerRange.Trigger(state.TriggerRight),
            new DpadState(
                (state.ButtonsLow & DpadUp) != 0,
                (state.ButtonsLow & DpadRight) != 0,
                (state.ButtonsLow & DpadDown) != 0,
                (state.ButtonsLow & DpadLeft) != 0));

    /// <summary>
    /// Accelerometer (m/s^2) and gyroscope (rad/s) in SDL's gamepad sensor frame,
    /// converted exactly as SDL's Deck driver does (X, Z, -Y), so the result goes
    /// through the one SDL -> canonical transform in
    /// <see cref="SdlGamepadMapping.Motion"/> rather than a second one.
    /// </summary>
    public static void Sensors(in State state, Span<float> accelMs2, Span<float> gyroRads)
    {
        accelMs2[0] = state.AccelX * AccelMs2PerCount;
        accelMs2[1] = state.AccelZ * AccelMs2PerCount;
        accelMs2[2] = -state.AccelY * AccelMs2PerCount;
        gyroRads[0] = state.GyroX * GyroRadsPerCount;
        gyroRads[1] = state.GyroZ * GyroRadsPerCount;
        gyroRads[2] = -state.GyroY * GyroRadsPerCount;
    }
}
