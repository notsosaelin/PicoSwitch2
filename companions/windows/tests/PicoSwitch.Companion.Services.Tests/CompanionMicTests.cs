using Concentus;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Companion.Services;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The companion microphone uplink (EXPERIMENTAL): framing, the encoder's
/// packet shape, the bounded writer, the session, and the shared vector fixture
/// the firmware's CELT decoder test consumes (contracts/companion_mic_vectors.txt).
/// </summary>
public sealed class CompanionMicTests
{
    // ------------------------------------------------------------- data plane

    [Fact]
    public void TheWireGeometryMatchesTheFirmwareContract()
    {
        // firmware/src/management/ns2_companion_mic.h
        Assert.Equal(1, CompanionMicDataPlane.Version);
        Assert.Equal(0x91, CompanionMicDataPlane.OpcodeOpus);
        Assert.Equal(160, CompanionMicDataPlane.OpusBytes);
        Assert.Equal(164, CompanionMicDataPlane.FrameBytes);
        Assert.Equal(0xF8, CompanionMicDataPlane.Toc);
        Assert.Equal(960, CompanionMicDataPlane.SamplesPerFrame);
        Assert.Equal(167, CompanionMicDataPlane.MinimumAttMtu);
    }

    [Fact]
    public void FramesOnlyTheExactPacketShape()
    {
        var packet = new byte[160];
        packet[0] = 0xF8;
        var frame = CompanionMicDataPlane.Encode(0x1234, packet)!;
        Assert.Equal([1, 0x91, 0x34, 0x12, 0xF8], frame[..5]);

        Assert.Null(CompanionMicDataPlane.Encode(0, packet.AsSpan(0, 159)));
        packet[0] = 0xFC; // stereo TOC
        Assert.Null(CompanionMicDataPlane.Encode(0, packet));
    }

    // ---------------------------------------------------------------- encoder

    [Fact]
    public void TheEncoderProducesExactlyTheUplinksPacketForSpeechAndSilence()
    {
        var encoder = new ConcentusCompanionMicEncoder();
        for (var f = 0; f < 50; f++)
        {
            var pcm = f % 10 == 0 ? new short[960] : Tone(f, 440, 20_000);
            var packet = encoder.Encode(pcm);
            Assert.NotNull(packet);
            Assert.Equal(160, packet!.Length);
            Assert.Equal(0xF8, packet[0]);
        }
    }

    [Fact]
    public void EncodedPacketsDecodeBackToTheToneThatWasCaptured()
    {
        var encoder = new ConcentusCompanionMicEncoder();
        var decoder = OpusCodecFactory.CreateDecoder(48_000, 1);
        var pcm = new short[960];
        double tone = 0, total = 0;
        for (var f = 0; f < 25; f++)
        {
            var packet = encoder.Encode(Tone(f, 1000, 12_000))!;
            Assert.Equal(960, decoder.Decode(packet, pcm, 960));
            if (f < 5)
            {
                continue; // codec warm-up
            }

            tone += Goertzel(pcm, 1000);
            foreach (var s in pcm)
            {
                total += (double)s * s;
            }
        }

        // Nearly all energy at 1 kHz, and the level preserved (12000 peak sine
        // = 8485 RMS) within 1 dB.
        var rms = Math.Sqrt(total / (20 * 960));
        Assert.InRange(rms, 8485 * 0.89, 8485 * 1.12);
        Assert.True(tone / total > 0.9, $"tone share {tone / total:F3}");
    }

    // ----------------------------------------------------------------- writer

    [Fact]
    public async Task TheWriterKeepsOrderWithOneWriteInFlight()
    {
        var gate = new Writes();
        var writer = new CompanionMicWriter(gate.WriteAsync);
        for (var i = 0; i < 5; i++)
        {
            writer.Enqueue(Frame(i));
        }

        Assert.Equal(1, gate.InFlight);
        Assert.Equal(4, writer.Waiting);
        await gate.CompleteAll();

        Assert.Equal([0, 1, 2, 3, 4], gate.Order);
        Assert.Equal(new CompanionMicWriter.Counters(5, 5, 0, 0), writer.Stats);
        Assert.Equal(1, gate.MaxInFlight);
    }

    [Fact]
    public async Task ABackedUpRadioDisplacesTheOldestWaitingFrame()
    {
        var gate = new Writes();
        var writer = new CompanionMicWriter(gate.WriteAsync);
        for (var i = 0; i < 1 + CompanionMicWriter.Depth + 3; i++)
        {
            writer.Enqueue(Frame(i));
        }

        Assert.Equal(CompanionMicWriter.Depth, writer.Waiting);
        await gate.CompleteAll();

        // Frame 0 was in flight; 1..3 gave way; the newest Depth survived.
        Assert.Equal([0, 4, 5, 6, 7, 8, 9, 10, 11], gate.Order);
        Assert.Equal(3, writer.Stats.Displaced);
    }

    [Fact]
    public async Task AFailedWriteIsCountedAndNeverRetried()
    {
        var gate = new Writes { Fail = true };
        var writer = new CompanionMicWriter(gate.WriteAsync);
        writer.Enqueue(Frame(0));
        writer.Enqueue(Frame(1));
        await gate.CompleteAll();

        Assert.Equal([0, 1], gate.Order);
        Assert.Equal(2, writer.Stats.Failed);
        Assert.Equal(0, writer.Stats.Written);
    }

    [Fact]
    public async Task AClosedWriterSendsNothingMore()
    {
        var gate = new Writes();
        var writer = new CompanionMicWriter(gate.WriteAsync);
        writer.Enqueue(Frame(0));
        writer.Enqueue(Frame(1));
        writer.Close();
        writer.Enqueue(Frame(2));
        await gate.CompleteAll();

        Assert.Equal([0], gate.Order);
    }

    // ---------------------------------------------------------------- session

    [Fact]
    public async Task TheSequenceAdvancesPerCapturedBlockSoARejectIsAGap()
    {
        var uplink = new RecordingUplink();
        var capture = new ManualCapture();
        var encoder = new ScriptedEncoder();
        var (session, problem) = await CompanionMicSession.OpenAsync(
            uplink, new SingleCapture(capture), encoder);
        Assert.Null(problem);

        capture.Deliver();
        encoder.Reject = true;
        capture.Deliver();
        encoder.Reject = false;
        capture.Deliver();

        Assert.Equal([0, 2], uplink.Sent.Select(f => f[2] | (f[3] << 8)));
        var metrics = session!.Metrics;
        Assert.Equal(2, metrics.FramesEncoded);
        Assert.Equal(1, metrics.EncodeRejected);

        await session.DisposeAsync();
        capture.Deliver();
        Assert.Equal(2, uplink.Sent.Count);
        Assert.True(uplink.Disposed);
        Assert.True(capture.Stopped);
    }

    [Fact]
    public async Task AMicrophoneThatWillNotOpenLeavesNothingOpen()
    {
        var uplink = new RecordingUplink();
        var capture = new ManualCapture { FailStart = true };
        var (session, problem) = await CompanionMicSession.OpenAsync(
            uplink, new SingleCapture(capture), new ScriptedEncoder());

        Assert.Null(session);
        Assert.Contains("Could not open the microphone", problem);
        Assert.True(uplink.Disposed);
    }

    // ---------------------------------------------------------------- fixture

    private const string FixtureSignal = "signal sine 1000 Hz amplitude 12000 frames 25";

    /// <summary>
    /// contracts/companion_mic_vectors.txt: packets this companion's encoder
    /// produced, decoded on the host by the firmware's own CELT decoder
    /// (tests/firmware/test_companion_mic_decode.c). Regenerate with
    /// PICOSWITCH_WRITE_MIC_VECTORS=1. Byte equality with a fresh encode is
    /// deliberately NOT asserted -- an encoder update may change bits, not
    /// validity; the check is that every vector is a valid uplink packet that
    /// decodes to the signal line's tone.
    /// </summary>
    [Fact]
    public void TheSharedVectorFixtureIsValidUplinkAudio()
    {
        var path = FixturePath();
        if (Environment.GetEnvironmentVariable("PICOSWITCH_WRITE_MIC_VECTORS") == "1")
        {
            var encoder = new ConcentusCompanionMicEncoder();
            var lines = new List<string>
            {
                "# Companion microphone uplink vectors: Concentus encoder, firmware CELT decoder.",
                "# Generated by CompanionMicTests.TheSharedVectorFixtureIsValidUplinkAudio.",
                FixtureSignal,
            };
            for (var f = 0; f < 25; f++)
            {
                lines.Add("packet " + Convert.ToHexString(encoder.Encode(Tone(f, 1000, 12_000))!).ToLowerInvariant());
            }

            File.WriteAllText(path, string.Join("\n", lines) + "\n");
        }

        var text = File.ReadAllLines(path);
        Assert.Contains(FixtureSignal, text);
        var packets = text.Where(l => l.StartsWith("packet ", StringComparison.Ordinal))
            .Select(l => Convert.FromHexString(l["packet ".Length..])).ToList();
        Assert.Equal(25, packets.Count);
        var decoder = OpusCodecFactory.CreateDecoder(48_000, 1);
        var pcm = new short[960];
        double tone = 0, total = 0;
        for (var f = 0; f < packets.Count; f++)
        {
            Assert.True(CompanionMicDataPlane.IsValidPacket(packets[f]));
            decoder.Decode(packets[f], pcm, 960);
            if (f >= 5)
            {
                tone += Goertzel(pcm, 1000);
                total += pcm.Sum(s => (double)s * s);
            }
        }

        Assert.True(tone / total > 0.9);
    }

    // ---------------------------------------------------------------- helpers

    private static string FixturePath()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "contracts", "companion_mic_vectors.txt");
            if (Directory.Exists(Path.Combine(dir.FullName, "contracts")) &&
                Directory.Exists(Path.Combine(dir.FullName, "firmware")))
            {
                return candidate;
            }
        }

        throw new InvalidOperationException("repository root not found");
    }

    private static short[] Tone(int frame, double hz, double amplitude)
    {
        var pcm = new short[960];
        for (var i = 0; i < 960; i++)
        {
            var t = (frame * 960 + i) / 48_000.0;
            pcm[i] = (short)Math.Round(Math.Sin(2 * Math.PI * hz * t) * amplitude);
        }

        return pcm;
    }

    /// <summary>Energy at <paramref name="hz"/>, scaled to compare with the sum of squares.</summary>
    private static double Goertzel(short[] pcm, double hz)
    {
        var w = 2 * Math.PI * hz / 48_000;
        var coeff = 2 * Math.Cos(w);
        double s1 = 0, s2 = 0;
        foreach (var x in pcm)
        {
            var s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        var power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2 * power / pcm.Length;
    }

    private static byte[] Frame(int id) => [(byte)id];

    private sealed class Writes
    {
        private readonly Queue<(byte[] Frame, TaskCompletionSource<bool> Done)> pending = new();

        public bool Fail { get; init; }

        public List<int> Order { get; } = [];

        public int InFlight => pending.Count;

        public int MaxInFlight { get; private set; }

        public Task<bool> WriteAsync(byte[] frame)
        {
            var done = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            lock (pending)
            {
                pending.Enqueue((frame, done));
                MaxInFlight = Math.Max(MaxInFlight, pending.Count);
            }

            return done.Task;
        }

        public async Task CompleteAll()
        {
            for (var idle = 0; idle < 50;)
            {
                (byte[] Frame, TaskCompletionSource<bool> Done)? next = null;
                lock (pending)
                {
                    if (pending.Count > 0)
                    {
                        next = pending.Dequeue();
                    }
                }

                if (next is { } write)
                {
                    idle = 0;
                    Order.Add(write.Frame[0]);
                    write.Done.SetResult(!Fail);
                }
                else
                {
                    idle++;
                }

                await Task.Delay(2);
            }
        }
    }

    private sealed class RecordingUplink : ICompanionMicUplink
    {
        public List<byte[]> Sent { get; } = [];

        public bool Disposed { get; private set; }

        public CompanionMicWriter.Counters Stats => new(Sent.Count, Sent.Count, 0, 0);

        public Task<bool> OpenAsync(CancellationToken cancellationToken = default) => Task.FromResult(true);

        public void Send(byte[] frame) => Sent.Add(frame);

        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ManualCapture : ICompanionMicCapture
    {
        private Action<short[]>? onBlock;

        public bool FailStart { get; init; }

        public bool Stopped { get; private set; }

        public long BlocksCaptured { get; private set; }

        public void Start(Action<short[]> onBlock)
        {
            if (FailStart)
            {
                throw new InvalidOperationException("device busy");
            }

            this.onBlock = onBlock;
        }

        public void Deliver()
        {
            BlocksCaptured++;
            onBlock?.Invoke(new short[960]);
        }

        public void Stop() => Stopped = true;

        public void Dispose()
        {
        }
    }

    private sealed class SingleCapture(ICompanionMicCapture capture) : ICompanionMicCaptureFactory
    {
        public ICompanionMicCapture? CreateCapture() => capture;
    }

    private sealed class ScriptedEncoder : ICompanionMicEncoder
    {
        public bool Reject { get; set; }

        public byte[]? Encode(short[] pcm)
        {
            if (Reject)
            {
                return null;
            }

            var packet = new byte[160];
            packet[0] = 0xF8;
            return packet;
        }
    }
}
