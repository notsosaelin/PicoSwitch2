namespace PicoSwitch.Bridge.Protocol;

/// <summary>
/// Companion microphone uplink framing (EXPERIMENTAL) -- the companion half of
/// <c>firmware/src/management/ns2_companion_mic.h</c>.
///
/// One GATT write-without-response carries one complete Opus packet:
/// <c>[version][opcode][seq lo][seq hi]</c> followed by the packet, TOC byte
/// first. The packet is CELT-only, full band, MONO, 20 ms (TOC <c>0xF8</c>),
/// 64 kbit/s constant bitrate: exactly 160 bytes. The adapter decodes it with
/// the CELT decoder underneath libopus's public one and refuses any other
/// shape, so the encoder here must produce exactly this.
///
/// <para>
/// Why mono at 64 kbit/s is not a reduction: a headset microphone is one
/// capsule, and 64 kbit/s is above the DualSense's own microphone stream
/// (56.8 kbit/s) and, per channel, above the 96 kbit/s stereo downlink.
/// </para>
///
/// The sequence advances once per block CAPTURED, so a block the radio or the
/// bounded writer dropped shows up at the adapter as a gap it conceals.
/// </summary>
public static class CompanionMicDataPlane
{
    public const byte Version = 1;
    public const byte OpcodeOpus = 0x91;
    public const int HeaderBytes = 4;

    public const int Kbps = 64;
    public const int OpusBytes = Kbps * 1000 / 8 / (1000 / FrameMillis);
    public const int FrameBytes = HeaderBytes + OpusBytes;

    /// <summary>Config 31 (CELT-only, full band, 20 ms), mono, one frame.</summary>
    public const byte Toc = 0xF8;

    public const int SampleRate = 48_000;
    public const int Channels = 1;
    public const int FrameMillis = 20;
    public const int SamplesPerFrame = SampleRate * FrameMillis / 1000;

    /// <summary>One write carries one frame: value &lt;= MTU - 3.</summary>
    public const int MinimumAttMtu = FrameBytes + 3;

    /// <summary>Whether an encoder's output is exactly one packet the adapter accepts.</summary>
    public static bool IsValidPacket(ReadOnlySpan<byte> opus) =>
        opus.Length == OpusBytes && opus[0] == Toc;

    /// <summary>
    /// Frame one packet, or null when it is not exactly the uplink's packet --
    /// the adapter would refuse it, so it is never sent.
    /// </summary>
    public static byte[]? Encode(ushort sequence, ReadOnlySpan<byte> opus)
    {
        if (!IsValidPacket(opus))
        {
            return null;
        }

        var frame = new byte[FrameBytes];
        frame[0] = Version;
        frame[1] = OpcodeOpus;
        frame[2] = (byte)(sequence & 0xFF);
        frame[3] = (byte)(sequence >> 8);
        opus.CopyTo(frame.AsSpan(HeaderBytes));
        return frame;
    }
}
