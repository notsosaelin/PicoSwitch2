using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using PicoSwitch.Bridge.Protocol;

namespace PicoSwitch.Companion.Windows.ControllerLink;

/// <summary>
/// Fixed binary contract between the full-trust companion and the same-package
/// AppContainer HOGP host. This contract is intentionally independent of WinRT
/// and GATT so framing, compatibility, and queue semantics can be tested without
/// activating Bluetooth.
/// </summary>
public static class ControllerLinkIpcProtocol
{
    public const uint Magic = 0x314C4350; // "PCL1" in little-endian memory.
    public const ushort Version = 1;
    public const int HeaderSize = 28;
    public const int MaximumPayloadSize = 512;
    public const int ChallengeSize = 32;
    public const int DescriptorDigestSize = 32;
    public const int HostHelloSize = 4 + 4 + 4 + 4 + 4 + DescriptorDigestSize + ChallengeSize;
    public const int InputReportSize = ControllerReportEncoder.PayloadSizeV2;
    public const int OutputReportSize = BridgeOutputCodec.BodySize;
    public const uint HelperBuild = 0x0001_0000;

    public static byte[] CreateChallenge() => RandomNumberGenerator.GetBytes(ChallengeSize);

    public static byte[] BuildHostHello(ReadOnlySpan<byte> challenge)
    {
        if (challenge.Length != ChallengeSize)
        {
            throw new ArgumentException($"Challenge must be {ChallengeSize} bytes.", nameof(challenge));
        }

        var payload = new byte[HostHelloSize];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), HelperBuild);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(4, 4), BridgeContract.Version);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(8, 4), BridgeHidDescriptor.Bytes.Length);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(12, 4), InputReportSize);
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(16, 4), OutputReportSize);
        Convert.FromHexString(BridgeContract.ExpectedDescriptorDigest!).CopyTo(payload, 20);
        challenge.CopyTo(payload.AsSpan(20 + DescriptorDigestSize));
        return payload;
    }

    public static HostHello ParseAndValidateHostHello(
        ReadOnlySpan<byte> payload,
        ReadOnlySpan<byte> expectedChallenge)
    {
        if (payload.Length != HostHelloSize)
        {
            throw new ControllerLinkProtocolException(
                $"Host hello is {payload.Length} bytes; expected {HostHelloSize}.");
        }

        if (expectedChallenge.Length != ChallengeSize)
        {
            throw new ArgumentException($"Challenge must be {ChallengeSize} bytes.", nameof(expectedChallenge));
        }

        var hello = new HostHello(
            HelperBuild: BinaryPrimitives.ReadUInt32LittleEndian(payload[0..4]),
            BridgeContract: BinaryPrimitives.ReadInt32LittleEndian(payload[4..8]),
            DescriptorBytes: BinaryPrimitives.ReadInt32LittleEndian(payload[8..12]),
            InputReportBytes: BinaryPrimitives.ReadInt32LittleEndian(payload[12..16]),
            OutputReportBytes: BinaryPrimitives.ReadInt32LittleEndian(payload[16..20]),
            DescriptorSha256: Convert.ToHexString(payload.Slice(20, DescriptorDigestSize)).ToLowerInvariant());

        var challenge = payload.Slice(20 + DescriptorDigestSize, ChallengeSize);
        if (!CryptographicOperations.FixedTimeEquals(challenge, expectedChallenge))
        {
            throw new ControllerLinkProtocolException("Controller Link host challenge mismatch.");
        }

        if (hello.HelperBuild != HelperBuild ||
            hello.BridgeContract != BridgeContract.Version ||
            hello.DescriptorBytes != BridgeHidDescriptor.Bytes.Length ||
            hello.InputReportBytes != InputReportSize ||
            hello.OutputReportBytes != OutputReportSize ||
            !string.Equals(
                hello.DescriptorSha256,
                BridgeContract.ExpectedDescriptorDigest,
                StringComparison.Ordinal))
        {
            throw new ControllerLinkProtocolException(
                "Controller Link unavailable: installed components are out of sync " +
                $"(helperBuild={hello.HelperBuild:x8}, bridge={hello.BridgeContract}, " +
                $"descriptorBytes={hello.DescriptorBytes}, descriptorSha256={hello.DescriptorSha256}, " +
                $"inputBytes={hello.InputReportBytes}, outputBytes={hello.OutputReportBytes}).");
        }

        return hello;
    }

    public static ControllerLinkFrame CreateFrame(
        ControllerLinkMessageType type,
        ulong sequence,
        ReadOnlySpan<byte> payload,
        long? timestamp = null)
    {
        if (payload.Length > MaximumPayloadSize)
        {
            throw new ArgumentOutOfRangeException(
                nameof(payload), payload.Length, $"Maximum payload is {MaximumPayloadSize} bytes.");
        }

        return new ControllerLinkFrame(
            type,
            sequence,
            timestamp ?? Stopwatch.GetTimestamp(),
            payload.ToArray());
    }

    public static byte[] Encode(ControllerLinkFrame frame)
    {
        ArgumentNullException.ThrowIfNull(frame.Payload);
        if (frame.Payload.Length > MaximumPayloadSize)
        {
            throw new ControllerLinkProtocolException(
                $"Frame payload is {frame.Payload.Length} bytes; maximum is {MaximumPayloadSize}.");
        }

        var bytes = new byte[HeaderSize + frame.Payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(0, 4), Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4, 2), Version);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6, 2), (ushort)frame.Type);
        BinaryPrimitives.WriteInt32LittleEndian(bytes.AsSpan(8, 4), frame.Payload.Length);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(12, 8), frame.Sequence);
        BinaryPrimitives.WriteInt64LittleEndian(bytes.AsSpan(20, 8), frame.Timestamp);
        frame.Payload.CopyTo(bytes, HeaderSize);
        return bytes;
    }

    public static async ValueTask<ControllerLinkFrame> ReadAsync(
        Stream stream,
        CancellationToken cancellationToken = default)
    {
        var header = new byte[HeaderSize];
        await stream.ReadExactlyAsync(header, cancellationToken).ConfigureAwait(false);

        var magic = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(0, 4));
        var version = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(4, 2));
        var typeValue = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(6, 2));
        var payloadLength = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(8, 4));
        if (magic != Magic)
        {
            throw new ControllerLinkProtocolException($"Invalid pipe magic 0x{magic:x8}.");
        }

        if (version != Version)
        {
            throw new ControllerLinkProtocolException(
                $"IPC protocol mismatch: helper sent {version}, companion expects {Version}.");
        }

        if (!Enum.IsDefined(typeof(ControllerLinkMessageType), typeValue))
        {
            throw new ControllerLinkProtocolException($"Unknown pipe message type {typeValue}.");
        }

        if (payloadLength is < 0 or > MaximumPayloadSize)
        {
            throw new ControllerLinkProtocolException($"Invalid pipe payload length {payloadLength}.");
        }

        var payload = new byte[payloadLength];
        await stream.ReadExactlyAsync(payload, cancellationToken).ConfigureAwait(false);
        return new ControllerLinkFrame(
            (ControllerLinkMessageType)typeValue,
            BinaryPrimitives.ReadUInt64LittleEndian(header.AsSpan(12, 8)),
            BinaryPrimitives.ReadInt64LittleEndian(header.AsSpan(20, 8)),
            payload);
    }
}

public enum ControllerLinkMessageType : ushort
{
    HostHello = 1,
    MainHello = 2,
    InputReport = 16,
    Heartbeat = 17,
    Stop = 18,
    HostState = 32,
    OutputReport = 33,
    Diagnostics = 34,
}

public enum ControllerLinkHostState : byte
{
    Ready = 0,
    Starting = 1,
    Advertising = 2,
    WaitingForConnection = 3,
    Connected = 4,
    Disconnected = 5,
    Stopped = 6,
    Error = 7,
}

public sealed record ControllerLinkFrame(
    ControllerLinkMessageType Type,
    ulong Sequence,
    long Timestamp,
    byte[] Payload);

public sealed record HostHello(
    uint HelperBuild,
    int BridgeContract,
    int DescriptorBytes,
    int InputReportBytes,
    int OutputReportBytes,
    string DescriptorSha256);

public sealed class ControllerLinkProtocolException(string message) : Exception(message);
