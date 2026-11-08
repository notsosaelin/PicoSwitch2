using PicoSwitch.Bridge.Protocol;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Turns companion audio notifications into PCM blocks, with gap concealment.
///
/// Pure and radio-free so its policy is testable: give it notification bytes,
/// it calls the decoder and hands PCM to <paramref name="emit"/>.
///
/// Policy:
/// <list type="bullet">
/// <item>A malformed notification is counted and ignored.</item>
/// <item>A duplicate or older sequence is dropped -- playing it would rewind.</item>
/// <item>A forward gap is concealed with the decoder's packet-loss concealment,
/// one 20 ms block per missing frame, capped at <see cref="MaxConcealedFrames"/>.
/// Beyond the cap the stream simply resumes: concealing a long outage would only
/// add latency the bounded sink then has to drop.</item>
/// </list>
/// </summary>
/// <param name="frameBytes">The session's frame size, from <c>caudio status</c>
/// (<c>frame_bytes</c>). A notification of any other length is malformed.</param>
public sealed class CompanionAudioReceiver(
    ICompanionAudioDecoder decoder,
    Action<ReadOnlySpan<short>> emit,
    int frameBytes)
{
    public const int MaxConcealedFrames = 3;

    private readonly short[] pcm =
        new short[CompanionAudioDataPlane.SamplesPerChannel * CompanionAudioDataPlane.Channels];
    private bool haveLast;
    private ushort lastSequence;

    public long FramesDecoded { get; private set; }

    public long FramesMalformed { get; private set; }

    public long FramesStale { get; private set; }

    public long FramesLost { get; private set; }

    public long FramesConcealed { get; private set; }

    public long DecodeErrors { get; private set; }

    /// <summary>Concealment blocks emitted to deepen the sink's buffer.</summary>
    public long FramesGrown { get; private set; }

    /// <summary>
    /// Emit one packet-loss-concealment block after the last frame, so the
    /// sink's buffer grows by a block without a gap. The decoder continues
    /// from it into the next real frame as it would after a lost packet.
    /// </summary>
    public void ConcealOne()
    {
        if (haveLast && TryDecode(null))
        {
            FramesGrown++;
            emit(pcm);
        }
    }

    public void Accept(ReadOnlySpan<byte> notification)
    {
        var frame = CompanionAudioDataPlane.Decode(notification, frameBytes);
        if (frame is null)
        {
            FramesMalformed++;
            return;
        }

        if (haveLast)
        {
            var gap = CompanionAudioDataPlane.Gap(lastSequence, frame.Sequence);
            if (gap < 0)
            {
                FramesStale++;
                return;
            }

            if (gap > 0)
            {
                FramesLost += gap;
                var conceal = Math.Min(gap, MaxConcealedFrames);
                for (var i = 0; i < conceal; i++)
                {
                    if (TryDecode(null))
                    {
                        FramesConcealed++;
                        emit(pcm);
                    }
                }
            }
        }

        haveLast = true;
        lastSequence = frame.Sequence;
        if (TryDecode(frame.Opus))
        {
            FramesDecoded++;
            emit(pcm);
        }
    }

    /// <summary>Forget sequence history, e.g. for a new adapter session.</summary>
    public void Reset()
    {
        haveLast = false;
        decoder.Reset();
    }

    private bool TryDecode(byte[]? opus)
    {
        int samples;
        try
        {
            samples = decoder.Decode(opus, pcm);
        }
        catch (Exception)
        {
            DecodeErrors++;
            return false;
        }

        if (samples != CompanionAudioDataPlane.SamplesPerChannel)
        {
            DecodeErrors++;
            return false;
        }

        return true;
    }
}

/// <summary>
/// The Opus decoder seam. <paramref name="opus"/> null asks for packet-loss
/// concealment. Returns samples per channel written into the interleaved
/// <paramref name="pcm"/> buffer.
/// </summary>
public interface ICompanionAudioDecoder
{
    int Decode(byte[]? opus, short[] pcm);

    void Reset();
}
