using PicoSwitch.Bridge.Protocol;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// Pins the companion half of firmware/src/management/ns2_companion_audio.h.
/// The firmware host test pins the same numbers from the other side.
/// </summary>
public sealed class CompanionAudioDataPlaneTests
{
    [Fact]
    public void GeometryMatchesTheFirmwareContract()
    {
        Assert.Equal(244, CompanionAudioDataPlane.MaxFrameBytes);
        Assert.Equal(960, CompanionAudioDataPlane.SamplesPerChannel);
        // CBR: the frame size is the bitrate. The adapter streams 244 B frames
        // (96 kbit/s, the genuine headset rate); the receiver is size-agnostic
        // and sized from the reported frame_bytes.
        Assert.Equal(64, CompanionAudioDataPlane.Kbps(164));
        Assert.Equal(96, CompanionAudioDataPlane.Kbps(244));
        Assert.Equal(48, CompanionAudioDataPlane.Kbps(124));
        Assert.Equal(167, CompanionAudioDataPlane.MinimumAttMtu(164));
        Assert.Equal(247, CompanionAudioDataPlane.MinimumAttMtu(244));
    }

    [Theory]
    [InlineData(124, true)]
    [InlineData(164, true)]
    [InlineData(244, true)]
    [InlineData(245, false)]   // larger than the encoder's frame slot
    [InlineData(5, false)]     // a one-byte packet is not a CELT frame
    [InlineData(0, false)]
    public void OnlyCarriableFrameSizesAreSupported(int frameBytes, bool supported) =>
        Assert.Equal(supported, CompanionAudioDataPlane.FrameBytesSupported(frameBytes));

    [Theory]
    [InlineData(164)]
    [InlineData(244)]
    public void DecodesAFrameExactlyAsTheFirmwareEncodesIt(int frameBytes)
    {
        var frame = new byte[frameBytes];
        frame[0] = 1;
        frame[1] = 0x90;
        frame[2] = 0xEF;
        frame[3] = 0xBE;
        frame[4] = 0xFC;
        frame[^1] = 0x5A;

        var decoded = CompanionAudioDataPlane.Decode(frame, frameBytes);

        Assert.NotNull(decoded);
        Assert.Equal(0xBEEF, decoded.Sequence);
        Assert.Equal(frameBytes - 4, decoded.Opus.Length);
        Assert.Equal(0xFC, decoded.Opus[0]);
        Assert.Equal(0x5A, decoded.Opus[^1]);
    }

    [Theory]
    [InlineData(163)]
    [InlineData(165)]
    [InlineData(244)]   // a 96 kbit/s frame inside a 64 kbit/s session
    [InlineData(0)]
    public void RejectsAnyLengthButOneWholeFrameOfTheSession(int length)
    {
        var frame = new byte[length];
        if (length >= 2)
        {
            frame[0] = 1;
            frame[1] = 0x90;
        }

        Assert.Null(CompanionAudioDataPlane.Decode(frame, 164));
    }

    [Fact]
    public void RejectsWrongVersionOrOpcode()
    {
        var frame = new byte[164];
        frame[0] = 2;
        frame[1] = 0x90;
        Assert.Null(CompanionAudioDataPlane.Decode(frame, 164));
        frame[0] = 1;
        frame[1] = 0x81;  // a Controller Link feedback opcode, not audio
        Assert.Null(CompanionAudioDataPlane.Decode(frame, 164));
    }

    [Theory]
    [InlineData(10, 11, 0)]
    [InlineData(10, 14, 3)]
    [InlineData(0xFFFF, 0, 0)]      // wrap is consecutive, not a rewind
    [InlineData(0xFFFE, 1, 2)]
    [InlineData(10, 10, -1)]        // duplicate
    [InlineData(10, 8, -3)]         // older
    public void GapIsWrapSafe(int previous, int current, int expected) =>
        Assert.Equal(expected, CompanionAudioDataPlane.Gap((ushort)previous, (ushort)current));
}
