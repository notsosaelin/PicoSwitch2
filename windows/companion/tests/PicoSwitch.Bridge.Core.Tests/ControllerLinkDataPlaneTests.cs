using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The Windows half of the Path C wire contract, pinned against the same
/// numbers <c>include/ns2_companion_link.h</c> static-asserts on the firmware
/// side. Both ends are edited together, so agreement here is what catches a
/// change that silently moves the payload.
/// </summary>
public sealed class ControllerLinkDataPlaneTests
{
    [Fact]
    public void FrameGeometryMatchesTheFirmwareContract()
    {
        // The payload IS the encoder's output, not a number that happens to
        // match it today. Contract 4 moved motion, battery and the timestamp by
        // one byte; a constant maintained by hand would have kept building.
        Assert.Equal(ControllerReportEncoder.PayloadSizeV2, ControllerLinkDataPlane.PayloadBytes);
        Assert.Equal(26, ControllerLinkDataPlane.PayloadBytes);
        Assert.Equal(4, ControllerLinkDataPlane.HeaderBytes);
        Assert.Equal(30, ControllerLinkDataPlane.FrameBytes);

        // 30-byte value + 3 bytes ATT Write Command overhead.
        Assert.Equal(33, ControllerLinkDataPlane.MinimumAttMtu);
        Assert.False(ControllerLinkDataPlane.MtuSufficient(23));  // default ATT MTU
        Assert.False(ControllerLinkDataPlane.MtuSufficient(32));
        Assert.True(ControllerLinkDataPlane.MtuSufficient(33));
        Assert.True(ControllerLinkDataPlane.MtuSufficient(517)); // what Windows negotiates

        // The feedback bound is derived from the canonical output body, not
        // chosen. If BridgeOutputCodec grows, this must follow it.
        Assert.Equal(BridgeOutputCodec.BodySize, ControllerLinkDataPlane.OutputMaxPayload);
        Assert.Equal(4, ControllerLinkDataPlane.OutputMaxPayload);
    }

    [Fact]
    public void EncodesAnEncoderPayloadIntoAFrame()
    {
        var payload = ControllerReportEncoder.Encode(new ControllerState());
        var frame = new byte[ControllerLinkDataPlane.FrameBytes];

        ControllerLinkDataPlane.EncodeInput(payload, 0xBEEF, frame);

        Assert.Equal(ControllerLinkDataPlane.Version, frame[0]);
        Assert.Equal(ControllerLinkDataPlane.OpcodeState, frame[1]);
        Assert.Equal(0xEF, frame[2]);   // little-endian sequence
        Assert.Equal(0xBE, frame[3]);
        Assert.Equal(payload, frame[ControllerLinkDataPlane.HeaderBytes..]);
    }

    [Fact]
    public void RefusesAPayloadThatIsNotTheCanonicalReport()
    {
        var frame = new byte[ControllerLinkDataPlane.FrameBytes];

        // Truncating or padding here would put the adapter's decode one byte
        // out on every later field, which reads as working but wrong.
        Assert.Throws<ArgumentException>(() =>
            ControllerLinkDataPlane.EncodeInput(new byte[25], 1, frame));
        Assert.Throws<ArgumentException>(() =>
            ControllerLinkDataPlane.EncodeInput(new byte[27], 1, frame));
        Assert.Throws<ArgumentException>(() =>
            ControllerLinkDataPlane.EncodeInput(new byte[26], 1, new byte[29]));
    }

    [Fact]
    public void SequenceWrapsWithoutSpecialCasing()
    {
        // At 125 Hz a ushort wraps every ~9 minutes, so the wrap is a normal
        // event, not an edge case. The adapter reads it as a signed delta.
        var payload = new byte[ControllerLinkDataPlane.PayloadBytes];
        var frame = new byte[ControllerLinkDataPlane.FrameBytes];

        ControllerLinkDataPlane.EncodeInput(payload, ushort.MaxValue, frame);
        Assert.Equal(0xFF, frame[2]);
        Assert.Equal(0xFF, frame[3]);

        ControllerLinkDataPlane.EncodeInput(payload, 0, frame);
        Assert.Equal(0x00, frame[2]);
        Assert.Equal(0x00, frame[3]);
    }

    [Fact]
    public void DecodesAFeedbackFrameAndFeedsTheSharedCodec()
    {
        // The point of the framing is that what comes out of it goes straight
        // into the existing output codec — no second rumble parser.
        byte[] frame =
        [
            ControllerLinkDataPlane.Version,
            ControllerLinkDataPlane.OpcodeOutput,
            ControllerReportEncoder.OutputReportId,
            0x40, 0x80, 0x02, BridgeOutputCodec.FlagMotionWanted,
        ];

        var decoded = ControllerLinkDataPlane.DecodeOutput(frame);
        Assert.NotNull(decoded);
        Assert.Equal(ControllerReportEncoder.OutputReportId, decoded!.ReportId);

        var output = BridgeOutputCodec.Decode(decoded.Payload, decoded.ReportId);
        Assert.NotNull(output);
        Assert.Equal(0x40, output!.Value.Rumble.Left);
        Assert.Equal(0x80, output.Value.Rumble.Right);
        Assert.Equal(2, output.Value.PlayerIndicator);
        Assert.True(output.Value.MotionRequested);
    }

    [Fact]
    public void RejectsFeedbackFramesThatCannotBeOurs()
    {
        byte[] good =
        [
            ControllerLinkDataPlane.Version, ControllerLinkDataPlane.OpcodeOutput,
            ControllerReportEncoder.OutputReportId, 0, 0, 0, 0,
        ];
        Assert.NotNull(ControllerLinkDataPlane.DecodeOutput(good));

        // A stray notification must be ignored, not applied as rumble and not
        // thrown — throwing would tear down a healthy link over one bad packet.
        Assert.Null(ControllerLinkDataPlane.DecodeOutput([]));
        Assert.Null(ControllerLinkDataPlane.DecodeOutput([1, 2]));

        byte[] wrongVersion = (byte[])good.Clone();
        wrongVersion[0] = ControllerLinkDataPlane.Version + 1;
        Assert.Null(ControllerLinkDataPlane.DecodeOutput(wrongVersion));

        byte[] wrongOpcode = (byte[])good.Clone();
        wrongOpcode[1] = ControllerLinkDataPlane.OpcodeState;
        Assert.Null(ControllerLinkDataPlane.DecodeOutput(wrongOpcode));

        // Longer than the canonical body: something else is talking.
        byte[] tooLong = new byte[ControllerLinkDataPlane.OutputHeaderBytes
                                  + ControllerLinkDataPlane.OutputMaxPayload + 1];
        tooLong[0] = ControllerLinkDataPlane.Version;
        tooLong[1] = ControllerLinkDataPlane.OpcodeOutput;
        Assert.Null(ControllerLinkDataPlane.DecodeOutput(tooLong));
    }

    [Fact]
    public void EveryGoldenPayloadFitsOneFrame()
    {
        // The shared golden fixture is the cross-language contract. Every row
        // must be exactly the width the frame reserves, or the two ends
        // disagree about the report regardless of what the constants say. The
        // firmware test asserts the same thing over the same file.
        var rows = RepositoryFixtures.ReadCsv(RepositoryFixtures.BridgeReportGoldens).ToList();
        Assert.NotEmpty(rows);

        foreach (var fields in rows)
        {
            Assert.Equal(13, fields.Length);
            var v2 = Convert.FromHexString(fields[12]);
            Assert.Equal(ControllerLinkDataPlane.PayloadBytes, v2.Length);

            var frame = new byte[ControllerLinkDataPlane.FrameBytes];
            ControllerLinkDataPlane.EncodeInput(v2, 1, frame);
            Assert.Equal(v2, frame[ControllerLinkDataPlane.HeaderBytes..]);
        }
    }
}
