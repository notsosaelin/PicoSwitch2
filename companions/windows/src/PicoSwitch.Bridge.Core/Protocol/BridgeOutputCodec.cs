using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Bridge.Protocol;

/// <summary>
/// PicoSwitch Bridge output report -> normalized <see cref="BridgeOutput"/>.
///
/// The framing tolerance here is protocol, not a platform workaround: HID hosts
/// legitimately deliver an output report either on the interrupt channel or as a
/// control-channel SET_REPORT, and stacks differ on whether the report ID is
/// included in the payload. Both framings carry the same five bytes, so the
/// decoder accepts both and every backend gets the tolerance for free.
///
/// Returns null when the payload cannot be a bridge output report, so a stray
/// report can never be applied as rumble.
/// </summary>
public static class BridgeOutputCodec
{
    /// <summary><c>[id][rumble L][rumble R][player][flags]</c></summary>
    public const int ReportSizeWithId = 5;

    public const int BodySize = 4;

    public const int FlagMotionWanted = 0x01;

    public static BridgeOutput? Decode(byte[]? data, int? reportId = null)
    {
        if (data is null)
        {
            return null;
        }

        if (reportId is not null && reportId != ControllerReportEncoder.OutputReportId)
        {
            return null;
        }

        var body = new ReadOnlySpan<byte>(data);

        // Tolerate an embedded report ID: some stacks include it in `data`.
        if (body.Length == ReportSizeWithId && body[0] == ControllerReportEncoder.OutputReportId)
        {
            body = body[1..];
        }

        if (body.Length < BodySize)
        {
            return null;
        }

        return new BridgeOutput(
            Rumble: new RumbleRequest(Left: body[0], Right: body[1]),
            PlayerIndicator: body[2],
            MotionRequested: (body[3] & FlagMotionWanted) != 0);
    }
}
