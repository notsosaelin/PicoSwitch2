using System.Buffers.Binary;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Windows.ControllerLink;
using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

public sealed class ControllerLinkIpcProtocolTests
{
    [Fact]
    public void HostHelloPinsEveryPeerVisibleContract()
    {
        var challenge = Enumerable.Range(0, ControllerLinkIpcProtocol.ChallengeSize)
            .Select(value => (byte)value)
            .ToArray();

        var hello = ControllerLinkIpcProtocol.ParseAndValidateHostHello(
            ControllerLinkIpcProtocol.BuildHostHello(challenge),
            challenge);

        Assert.Equal(BridgeContract.Version, hello.BridgeContract);
        Assert.Equal(161, hello.DescriptorBytes);
        Assert.Equal(ControllerLinkIpcProtocol.InputReportSize, hello.InputReportBytes);
        Assert.Equal(ControllerLinkIpcProtocol.OutputReportSize, hello.OutputReportBytes);
        Assert.Equal(BridgeContract.ExpectedDescriptorDigest, hello.DescriptorSha256);
    }

    [Fact]
    public void WrongChallengeIsRejectedInConstantContractPath()
    {
        var actual = new byte[ControllerLinkIpcProtocol.ChallengeSize];
        var expected = new byte[ControllerLinkIpcProtocol.ChallengeSize];
        expected[^1] = 1;

        var error = Assert.Throws<ControllerLinkProtocolException>(() =>
            ControllerLinkIpcProtocol.ParseAndValidateHostHello(
                ControllerLinkIpcProtocol.BuildHostHello(actual),
                expected));

        Assert.Contains("challenge mismatch", error.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(4, 99)]
    [InlineData(8, 160)]
    [InlineData(12, 25)]
    [InlineData(16, 5)]
    public void ComponentMismatchIsExplicit(int offset, int wrongValue)
    {
        var challenge = new byte[ControllerLinkIpcProtocol.ChallengeSize];
        var payload = ControllerLinkIpcProtocol.BuildHostHello(challenge);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(offset, 4), wrongValue);

        var error = Assert.Throws<ControllerLinkProtocolException>(() =>
            ControllerLinkIpcProtocol.ParseAndValidateHostHello(payload, challenge));

        Assert.Contains("out of sync", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FrameRoundTripPreservesSequenceTimestampAndPayload()
    {
        var original = ControllerLinkIpcProtocol.CreateFrame(
            ControllerLinkMessageType.InputReport,
            sequence: 42,
            payload: [1, 2, 3],
            timestamp: 123456);
        await using var stream = new MemoryStream(ControllerLinkIpcProtocol.Encode(original));

        var decoded = await ControllerLinkIpcProtocol.ReadAsync(stream);

        Assert.Equal(original.Type, decoded.Type);
        Assert.Equal(original.Sequence, decoded.Sequence);
        Assert.Equal(original.Timestamp, decoded.Timestamp);
        Assert.Equal(original.Payload, decoded.Payload);
    }

    [Fact]
    public async Task MalformedLengthIsRejectedBeforeAllocation()
    {
        var bytes = ControllerLinkIpcProtocol.Encode(
            ControllerLinkIpcProtocol.CreateFrame(
                ControllerLinkMessageType.Heartbeat,
                1,
                ReadOnlySpan<byte>.Empty));
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(8, 4), int.MaxValue);
        await using var stream = new MemoryStream(bytes);

        var error = await Assert.ThrowsAsync<ControllerLinkProtocolException>(async () =>
            await ControllerLinkIpcProtocol.ReadAsync(stream));

        Assert.Contains("payload length", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void OversizeWritesAreRejected()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ControllerLinkIpcProtocol.CreateFrame(
                ControllerLinkMessageType.Diagnostics,
                1,
                new byte[ControllerLinkIpcProtocol.MaximumPayloadSize + 1]));
    }
}
