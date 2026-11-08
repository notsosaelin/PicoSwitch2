using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.Input;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// Steam Deck-format hidraw reports -> bridge state, and the rules for which
/// hidraw nodes are read that way.
///
/// The fixtures are real reports captured from /dev/hidraw8 (InputPlumber
/// deck-uhid, 28de:12fd) on a ROG Xbox Ally X, 2026-11-06, with the left back
/// button held, then the right one; both copied verbatim from the capture log.
/// </summary>
public sealed class SteamDeckReportTests
{
    private const string LeftBackHeld =
        "01000940992000000000000000020000000000000000000034ffb72fb5d30300faff0400" +
        "00000000000000000000000000000000000000000000000000000000";

    private const string RightBackHeld =
        "01000940692b00000000000000040000000000000000000024fa333215c5d2fee6feacff" +
        "00000000000000000000000000000000000000000000000000000000";

    private static byte[] Report(string hex)
    {
        var bytes = Convert.FromHexString(hex);
        Assert.Equal(SteamDeckReport.Length, bytes.Length);
        return bytes;
    }

    private static byte[] Neutral()
    {
        var report = new byte[SteamDeckReport.Length];
        report[0] = 0x01;
        report[2] = 0x09;
        report[3] = 0x40;
        return report;
    }

    private static SteamDeckReport.State Parse(byte[] report)
    {
        Assert.True(SteamDeckReport.TryParse(report, out var state));
        return state;
    }

    [Fact]
    public void TheCapturedLeftBackButtonIsGl()
    {
        var state = Parse(Report(LeftBackHeld));

        Assert.Equal(0x2099u, state.Packet);
        Assert.Equal(ControllerButtonSet.Empty.With(ControllerButton.GL, true), SteamDeckReport.Buttons(state));
    }

    [Fact]
    public void TheCapturedRightBackButtonIsGr()
    {
        Assert.Equal(
            ControllerButtonSet.Empty.With(ControllerButton.GR, true),
            SteamDeckReport.Buttons(Parse(Report(RightBackHeld))));
    }

    [Fact]
    public void TheCapturedRestingAccelerometerReadsAboutOneG()
    {
        var state = Parse(Report(LeftBackHeld));
        Span<float> accel = stackalloc float[3];
        Span<float> gyro = stackalloc float[3];
        SteamDeckReport.Sensors(state, accel, gyro);

        var g = MathF.Sqrt((accel[0] * accel[0]) + (accel[1] * accel[1]) + (accel[2] * accel[2]));
        Assert.InRange(g, 9.3f, 10.3f);

        // Resting gyro counts are a few LSB: well under 1 dps.
        Assert.All(gyro.ToArray(), rate => Assert.InRange(MathF.Abs(rate), 0f, 0.02f));
    }

    [Fact]
    public void LyingFlatIsOneGOutOfTheFaceInTheCanonicalFrame()
    {
        // The Deck reports flat-on-a-table gravity on its +Z (SDL's driver maps
        // report Z to SDL +Y, "out of the face"); canonical is +Z out of the face.
        var report = Neutral();
        BitConverter.TryWriteBytes(report.AsSpan(28), (short)16384);
        Span<float> accel = stackalloc float[3];
        Span<float> gyro = stackalloc float[3];
        SteamDeckReport.Sensors(Parse(report), accel, gyro);

        var motion = SdlGamepadMapping.Motion(accel, gyro, 0);
        Assert.Equal(0, motion.AccelX);
        Assert.Equal(0, motion.AccelY);
        Assert.Equal(8192, motion.AccelZ);
    }

    [Fact]
    public void ANonStateReportIsRejected()
    {
        var report = Neutral();
        report[2] = 0x01; // a Steam Controller-style message type
        Assert.False(SteamDeckReport.TryParse(report, out _));
        Assert.False(SteamDeckReport.IsState(report.AsSpan(0, 32)));
    }

    [Theory]
    [InlineData(SteamDeckReport.A, ControllerButton.A)]
    [InlineData(SteamDeckReport.B, ControllerButton.B)]
    [InlineData(SteamDeckReport.X, ControllerButton.X)]
    [InlineData(SteamDeckReport.Y, ControllerButton.Y)]
    [InlineData(SteamDeckReport.L1, ControllerButton.L1)]
    [InlineData(SteamDeckReport.R1, ControllerButton.R1)]
    [InlineData(SteamDeckReport.View, ControllerButton.Select)]
    [InlineData(SteamDeckReport.Menu, ControllerButton.Start)]
    [InlineData(SteamDeckReport.Steam, ControllerButton.Home)]
    [InlineData(SteamDeckReport.L3, ControllerButton.LeftStick)]
    [InlineData(SteamDeckReport.R3, ControllerButton.RightStick)]
    public void LowWordButtonsMapPositionally(uint bit, ControllerButton expected)
    {
        var report = Neutral();
        BitConverter.TryWriteBytes(report.AsSpan(8), bit);

        Assert.Equal(ControllerButtonSet.Empty.With(expected, true), SteamDeckReport.Buttons(Parse(report)));
    }

    [Fact]
    public void QuickAccessIsCapture()
    {
        var report = Neutral();
        BitConverter.TryWriteBytes(report.AsSpan(12), SteamDeckReport.QuickAccess);

        Assert.Equal(
            ControllerButtonSet.Empty.With(ControllerButton.Capture, true),
            SteamDeckReport.Buttons(Parse(report)));
    }

    [Fact]
    public void StickUpIsBridgeUpAndTriggersAreFullRange()
    {
        var report = Neutral();
        BitConverter.TryWriteBytes(report.AsSpan(50), (short)32767);  // left stick fully up (+Y up)
        BitConverter.TryWriteBytes(report.AsSpan(52), (short)32767);  // right stick fully right
        BitConverter.TryWriteBytes(report.AsSpan(44), (ushort)32767); // left trigger full
        BitConverter.TryWriteBytes(report.AsSpan(8), SteamDeckReport.DpadLeft);

        var analog = SteamDeckReport.Analog(Parse(report));
        Assert.Equal(128, analog.LeftX);
        Assert.Equal(1, analog.LeftY); // bridge is +Y down
        Assert.Equal(255, analog.RightX);
        Assert.Equal(255, analog.LeftTrigger);
        Assert.Equal(0, analog.RightTrigger);
        Assert.True(analog.Dpad!.Value.Left);
        Assert.False(analog.Dpad!.Value.Up);
    }

    [Fact]
    public void TheNeutralReportIsNeutral()
    {
        var state = Parse(Neutral());
        Assert.Equal(ControllerButtonSet.Empty, SteamDeckReport.Buttons(state));
        Assert.Equal(AnalogFrame.Neutral with { Dpad = DpadState.None }, SteamDeckReport.Analog(state));
    }

    /* ----------------------------------------------------- source rules */

    private static HidrawNode Node(ushort vendor, ushort product, string? uniq = null) =>
        new("/dev/hidraw8", 8, vendor, product, "ROG Xbox Ally Controller", uniq);

    [Theory]
    [InlineData(0x28DE, 0x12FD, true)]  // InputPlumber's emulated Deck
    [InlineData(0x28DE, 0x1205, false)] // a real Deck: SDL's HIDAPI driver reads it
    [InlineData(0x054C, 0x0CE6, false)] // DualSense: not Valve
    public void OnlyValveIdsSdlDoesNotDriveAreProbed(int vendor, int product, bool probed) =>
        Assert.Equal(probed, SteamDeckSources.IsCandidate((ushort)vendor, (ushort)product));

    [Fact]
    public void SteamsVirtualPadCopyingTheIdsIsAMirror()
    {
        var node = Node(0x28DE, 0x12FD);
        Assert.Same(node, SteamDeckSources.MirrorOf(0x28DE, 0x12FD, [node]));
        Assert.Null(SteamDeckSources.MirrorOf(0x28DE, 0x11FF, [node]));
        Assert.Null(SteamDeckSources.MirrorOf(0x045E, 0x12FD, [node]));
    }

    [Fact]
    public void TheDescriptorDoesNotDependOnTheHidrawIndex()
    {
        var a = Node(0x28DE, 0x12FD);
        var b = a with { Index = 3, DevicePath = "/dev/hidraw3" };
        Assert.Equal(SteamDeckSources.Descriptor(a), SteamDeckSources.Descriptor(b));
        Assert.Equal("hidraw:28de:12fd:ROG Xbox Ally Controller", SteamDeckSources.Descriptor(a));
    }

    [Fact]
    public void UeventIdentityIsParsed()
    {
        const string uevent =
            "DRIVER=hid-generic\nHID_ID=0003:000028DE:000012FD\nHID_NAME=ROG Xbox Ally Controller\n" +
            "HID_PHYS=\nHID_UNIQ=\nMODALIAS=hid:b0003g0001v000028DEp000012FD\n";

        var node = Hidraw.ParseUevent(uevent, "/dev/hidraw8", 8);
        Assert.NotNull(node);
        Assert.Equal((ushort)0x28DE, node.Vendor);
        Assert.Equal((ushort)0x12FD, node.Product);
        Assert.Equal("ROG Xbox Ally Controller", node.Name);
        Assert.Null(node.Uniq);
        Assert.Null(Hidraw.ParseUevent("HID_NAME=x\n", "/dev/hidraw0", 0));
    }
}
