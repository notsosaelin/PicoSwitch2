namespace PicoSwitch.Bridge.Protocol;

/// <summary>
/// Companion audio downlink framing (EXPERIMENTAL) -- the companion half of
/// <c>firmware/src/management/ns2_companion_audio.h</c>.
///
/// One notification carries one complete Opus packet:
/// <c>[version][opcode][seq lo][seq hi]</c> followed by the packet, TOC byte
/// first. The packet is CELT-only, full band, stereo, 20 ms (TOC <c>0xFC</c>),
/// constant bitrate, so it is a valid public Opus packet and a standard decoder
/// decodes it unchanged.
///
/// <para>
/// The packet size is the session's bitrate (CBR). The adapter streams
/// 96 kbit/s, 240-byte packets: the console's own Pro Controller 2 headset
/// rate, which is the quality floor (ns2_companion_audio.h, BITRATE). It
/// reports the session's frame size in <c>caudio status</c>
/// (<c>frame_bytes</c>); every frame of that session has exactly that length,
/// so the receiver is sized from the report rather than from a constant.
/// </para>
///
/// The sequence advances once per 20 ms frame the adapter ENCODES, sent or not.
/// A gap means frames were dropped (shed by the adapter while the link stalled,
/// or lost by the radio); the receiver conceals rather than waits, because
/// audio is never resent. Before 2026-11-07 the adapter counted frames sent, so
/// a frame it shed was invisible (ns2_companion_audio.h).
/// </summary>
public static class CompanionAudioDataPlane
{
    public const byte Version = 1;
    public const byte OpcodeOpus = 0x90;
    public const int HeaderBytes = 4;

    /// <summary>The largest packet: 96 kbit/s, the encoder's validated ceiling.</summary>
    public const int MaxOpusBytes = 240;
    public const int MaxFrameBytes = HeaderBytes + MaxOpusBytes;

    public const int SampleRate = 48_000;
    public const int Channels = 2;
    public const int FrameMillis = 20;
    public const int SamplesPerChannel = SampleRate * FrameMillis / 1000;

    /// <summary>
    /// Whether a reported session frame size is one this receiver can carry: a
    /// header plus a packet of at least two bytes and at most the 96 kbit/s size.
    /// </summary>
    public static bool FrameBytesSupported(int frameBytes) =>
        frameBytes >= HeaderBytes + 2 && frameBytes <= MaxFrameBytes;

    /// <summary>Session bitrate implied by a CBR frame size, in kbit/s.</summary>
    public static int Kbps(int frameBytes) =>
        (frameBytes - HeaderBytes) * 8 * (1000 / FrameMillis) / 1000;

    /// <summary>One notification carries one frame: value &lt;= MTU - 3.</summary>
    public static int MinimumAttMtu(int frameBytes) => frameBytes + 3;

    /// <summary>
    /// Decode one notification of a session whose frames are
    /// <paramref name="frameBytes"/> long, or null when it cannot be one of its
    /// audio frames. A stray or malformed notification is counted and ignored,
    /// never fatal.
    /// </summary>
    public static CompanionAudioFrame? Decode(ReadOnlySpan<byte> frame, int frameBytes)
    {
        if (frame.Length != frameBytes || !FrameBytesSupported(frameBytes))
        {
            return null;
        }

        if (frame[0] != Version || frame[1] != OpcodeOpus)
        {
            return null;
        }

        var sequence = (ushort)(frame[2] | (frame[3] << 8));
        return new CompanionAudioFrame(sequence, frame[HeaderBytes..].ToArray());
    }

    /// <summary>
    /// Frames lost between <paramref name="previous"/> and
    /// <paramref name="current"/>, wrap-safe. Zero for consecutive frames;
    /// negative for a duplicate or a frame older than the last one played,
    /// which the receiver drops.
    /// </summary>
    public static int Gap(ushort previous, ushort current) =>
        (short)(ushort)(current - previous) - 1;
}

/// <summary>One decoded audio notification: its sequence and Opus packet.</summary>
public sealed record CompanionAudioFrame(ushort Sequence, byte[] Opus);
