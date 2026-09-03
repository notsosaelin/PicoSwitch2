using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Protocol;

/// <summary>
/// Normalized controller state -> PicoSwitch Bridge input report.
///
/// The only place in the bridge that knows the wire layout. Platform backends
/// never build report bytes; they produce a <see cref="ControllerState"/> and the
/// session encodes it, so a new platform cannot introduce a second, subtly
/// different encoding of the same contract.
///
/// Full field documentation: <c>docs/bridge/PROTOCOL.md</c> and the C-side source
/// of truth <c>tools/fixtures/android_controller_hid.h</c>.
/// </summary>
public static class ControllerReportEncoder
{
    public const int ReportId = 1;
    public const int OutputReportId = 2;

    /// <summary>v1 payload length, retained so the compatibility test can pin it.</summary>
    public const int PayloadSize = 9;

    /// <summary>
    /// v2 payload: v1 fields plus motion, battery, flags and timestamp.
    ///
    /// One byte longer since bridge contract 4, which grew the button field from
    /// two bytes to three so it could carry GL/GR.
    /// </summary>
    public const int PayloadSizeV2 = 26;

    // Wire offsets WITHIN THE PAYLOAD. The report ID is not part of the payload a
    // transport sends, so these are the C contract offsets minus one.
    private const int OffGyro = 10;
    private const int OffAccel = 16;
    /// <summary>
    /// Battery level and the flags byte beside it.
    ///
    /// Public so a test can assert what actually lands on the wire rather than
    /// re-deriving the offsets, which is how a golden and its subject drift
    /// apart.
    /// </summary>
    public const int OffBattery = 22;

    public const int OffFlags = 23;
    private const int OffTimestamp = 24;

    public const int FlagCharging = 0x01;
    public const int FlagMotionValid = 0x02;
    public const int FlagBatteryValid = 0x04;

    /// <summary>Full v2 report. The first nine bytes are NOT byte-identical to v1; see <see cref="EncodeV1"/>.</summary>
    public static byte[] Encode(ControllerState state)
    {
        var output = new byte[PayloadSizeV2];
        EncodeCore(state, output);

        if (state.Motion.Valid)
        {
            PutLe16(output, OffGyro + 0, state.Motion.GyroX);
            PutLe16(output, OffGyro + 2, state.Motion.GyroY);
            PutLe16(output, OffGyro + 4, state.Motion.GyroZ);
            PutLe16(output, OffAccel + 0, state.Motion.AccelX);
            PutLe16(output, OffAccel + 2, state.Motion.AccelY);
            PutLe16(output, OffAccel + 4, state.Motion.AccelZ);
            PutLe16(output, OffTimestamp, state.Motion.TimestampTicks & 0xFFFF);
        }

        output[OffBattery] = (byte)Math.Clamp(state.Battery.LevelPercent, 0, 100);

        var flags = 0;
        if (state.Motion.Valid)
        {
            flags |= FlagMotionValid;
        }

        if (state.Battery.Valid)
        {
            flags |= FlagBatteryValid;
            if (state.Battery.Charging)
            {
                flags |= FlagCharging;
            }
        }

        output[OffFlags] = (byte)flags;
        return output;
    }

    /// <summary>
    /// The original nine-byte report, kept for the v1 compatibility test.
    ///
    /// No longer a prefix of <see cref="Encode"/>. Through contract 3 the first
    /// nine bytes of both were identical, because every button still fitted in two
    /// bytes; contract 4 needed a third for GL/GR, which moved the hat and
    /// everything after it by one. A v1 peer is unaffected — it reads the v1
    /// descriptor, whose items still describe exactly these nine bytes — but the
    /// two layouts are now genuinely different and are written separately rather
    /// than one pretending to be a truncation of the other.
    /// </summary>
    public static byte[] EncodeV1(ControllerState state)
    {
        var output = new byte[PayloadSize];
        EncodeAxes(state, output);
        var bits = state.Buttons.Bits;
        output[6] = (byte)(bits & 0xFF);

        // 0x7F, not 0x3F: bit 14 is C / GameChat, the fifteenth button.
        output[7] = (byte)((bits >> 8) & 0x7F);
        output[8] = (byte)Hat(state);
        return output;
    }

    /// <summary>The four retained D-pad directions collapsed to a HID hat code; 8 = neutral.</summary>
    public static int Hat(ControllerState state)
    {
        var vertical = (state.DpadDown ? 1 : 0) - (state.DpadUp ? 1 : 0);
        var horizontal = (state.DpadRight ? 1 : 0) - (state.DpadLeft ? 1 : 0);
        return (horizontal, vertical) switch
        {
            (0, -1) => 0,
            (1, -1) => 1,
            (1, 0) => 2,
            (1, 1) => 3,
            (0, 1) => 4,
            (-1, 1) => 5,
            (-1, 0) => 6,
            (-1, -1) => 7,
            _ => 8,
        };
    }

    private static void EncodeCore(ControllerState state, byte[] output)
    {
        EncodeAxes(state, output);
        var bits = state.Buttons.Bits;
        output[6] = (byte)(bits & 0xFF);
        output[7] = (byte)((bits >> 8) & 0xFF);

        // Bit 16 is GR, the seventeenth and last button; the remaining seven bits
        // of this byte are the descriptor's padding and must stay clear.
        output[8] = (byte)((bits >> 16) & 0x01);
        output[9] = (byte)Hat(state);
    }

    private static void EncodeAxes(ControllerState state, byte[] output)
    {
        output[0] = U8(state.LeftX);
        output[1] = U8(state.LeftY);
        output[2] = U8(state.RightX);
        output[3] = U8(state.RightY);
        output[4] = U8(state.LeftTrigger);
        output[5] = U8(state.RightTrigger);
    }

    private static void PutLe16(byte[] output, int offset, int value)
    {
        output[offset] = (byte)(value & 0xFF);
        output[offset + 1] = (byte)((value >> 8) & 0xFF);
    }

    private static byte U8(int value) => (byte)Math.Clamp(value, 0, 255);
}
