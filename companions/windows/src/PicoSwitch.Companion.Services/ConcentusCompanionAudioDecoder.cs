using Concentus;
using PicoSwitch.Bridge.Protocol;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Standard Opus decode of the companion audio downlink, via Concentus (a
/// managed port of libopus). The adapter's packets are ordinary public Opus
/// packets (TOC <c>0xFC</c>: CELT-only, full band, stereo, 20 ms), so no
/// firmware-specific decoding is needed.
/// </summary>
public sealed class ConcentusCompanionAudioDecoder : ICompanionAudioDecoder
{
    private readonly IOpusDecoder decoder = OpusCodecFactory.CreateDecoder(
        CompanionAudioDataPlane.SampleRate, CompanionAudioDataPlane.Channels);

    public int Decode(byte[]? opus, short[] pcm) =>
        // An empty input asks the decoder for packet-loss concealment.
        decoder.Decode(
            opus is null ? ReadOnlySpan<byte>.Empty : opus,
            pcm,
            CompanionAudioDataPlane.SamplesPerChannel,
            false);

    public void Reset() => decoder.ResetState();
}
