using Concentus;
using Concentus.Enums;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Platform.CompanionAudio;

namespace PicoSwitch.Companion.Services;

/// <summary>One 20 ms microphone block to one uplink Opus packet.</summary>
public interface ICompanionMicEncoder
{
    /// <summary>
    /// <paramref name="pcm"/> is <see cref="CompanionMicDataPlane.SamplesPerFrame"/>
    /// mono samples. Returns the packet, or null when the encoder did not
    /// produce exactly the uplink's packet (it is then not sent).
    /// </summary>
    byte[]? Encode(short[] pcm);
}

/// <summary>
/// The uplink's encoder, via Concentus: restricted-low-delay (CELT only),
/// 48 kHz mono, 64 kbit/s CBR, full band, 20 ms, complexity 10. Every packet is
/// exactly 160 bytes with TOC <c>0xF8</c>, which is what the adapter's direct
/// CELT decoder accepts (ns2_companion_mic.h).
/// </summary>
public sealed class ConcentusCompanionMicEncoder : ICompanionMicEncoder
{
    private readonly IOpusEncoder encoder;
    private readonly byte[] packet = new byte[1275];

    public ConcentusCompanionMicEncoder()
    {
        encoder = OpusCodecFactory.CreateEncoder(
            CompanionMicDataPlane.SampleRate,
            CompanionMicDataPlane.Channels,
            OpusApplication.OPUS_APPLICATION_RESTRICTED_LOWDELAY);
        encoder.Bitrate = CompanionMicDataPlane.Kbps * 1000;
        encoder.UseVBR = false;
        encoder.Complexity = 10;
        encoder.Bandwidth = OpusBandwidth.OPUS_BANDWIDTH_FULLBAND;
    }

    public byte[]? Encode(short[] pcm)
    {
        var n = encoder.Encode(pcm, CompanionMicDataPlane.SamplesPerFrame, packet, packet.Length);
        var output = packet.AsSpan(0, Math.Max(n, 0));
        return CompanionMicDataPlane.IsValidPacket(output) ? output.ToArray() : null;
    }
}

/// <summary>Microphone uplink counters for one session.</summary>
public sealed record CompanionMicMetrics(
    long BlocksCaptured,
    long FramesEncoded,
    long EncodeRejected,
    long FramesWritten,
    long FramesDisplaced,
    long WriteFailures)
{
    public static CompanionMicMetrics Empty { get; } = new(0, 0, 0, 0, 0, 0);
}

/// <summary>
/// One microphone session: capture -> encode -> frame -> bounded writer. The
/// sequence advances once per captured block, encoded or not, so every block
/// the adapter does not receive is a gap it conceals.
/// </summary>
public sealed class CompanionMicSession : IAsyncDisposable
{
    private readonly ICompanionMicCapture capture;
    private readonly ICompanionMicEncoder encoder;
    private readonly ICompanionMicUplink uplink;
    private readonly object gate = new();
    private ushort sequence;
    private long encoded;
    private long rejected;
    private bool disposed;

    private CompanionMicSession(ICompanionMicCapture capture, ICompanionMicEncoder encoder, ICompanionMicUplink uplink)
    {
        this.capture = capture;
        this.encoder = encoder;
        this.uplink = uplink;
    }

    /// <summary>
    /// Open the uplink and start the microphone. Returns the session, or the
    /// reason there is none. Nothing is left open on failure.
    /// </summary>
    public static async Task<(CompanionMicSession? Session, string? Problem)> OpenAsync(
        ICompanionMicUplink? uplink,
        ICompanionMicCaptureFactory? captures,
        ICompanionMicEncoder encoder,
        CancellationToken cancellationToken = default)
    {
        if (uplink is null)
        {
            return (null, "The adapter connection has no microphone channel.");
        }

        if (!await uplink.OpenAsync(cancellationToken).ConfigureAwait(false))
        {
            await uplink.DisposeAsync().ConfigureAwait(false);
            return (null, "This adapter's firmware does not accept a microphone.");
        }

        var capture = captures?.CreateCapture();
        if (capture is null)
        {
            await uplink.DisposeAsync().ConfigureAwait(false);
            return (null, "No microphone is available on this device.");
        }

        var session = new CompanionMicSession(capture, encoder, uplink);
        try
        {
            capture.Start(session.OnBlock);
        }
        catch (Exception ex)
        {
            capture.Dispose();
            await uplink.DisposeAsync().ConfigureAwait(false);
            return (null, $"Could not open the microphone: {ex.Message}");
        }

        return (session, null);
    }

    public CompanionMicMetrics Metrics
    {
        get
        {
            var writes = uplink.Stats;
            lock (gate)
            {
                return new CompanionMicMetrics(
                    capture.BlocksCaptured, encoded, rejected, writes.Written, writes.Displaced, writes.Failed);
            }
        }
    }

    /// <summary>One captured block (capture thread). Public for tests.</summary>
    public void OnBlock(short[] pcm)
    {
        byte[]? frame;
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            var seq = sequence++;
            var opus = pcm.Length == CompanionMicDataPlane.SamplesPerFrame ? encoder.Encode(pcm) : null;
            frame = opus is null ? null : CompanionMicDataPlane.Encode(seq, opus);
            if (frame is null)
            {
                rejected++;
                return;
            }

            encoded++;
        }

        uplink.Send(frame);
    }

    public async ValueTask DisposeAsync()
    {
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
        }

        try
        {
            capture.Stop();
        }
        finally
        {
            capture.Dispose();
            await uplink.DisposeAsync().ConfigureAwait(false);
        }
    }
}
