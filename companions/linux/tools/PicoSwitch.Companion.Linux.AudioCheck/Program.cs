using System.Diagnostics;
using Concentus;
using Concentus.Enums;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Linux.Audio;
using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Management;
using SDL;

// See the .csproj for what this answers and what it does not.
var options = Options.Parse(args);
Console.WriteLine($"audio check: {options.Seconds}s per scenario, tone {options.Amplitude} peak, SDL {SDL3.SDL_GetVersion()}");

var failures = 0;
if (options.ControlSeconds > 0)
{
    // A listening reference with no PicoSwitch code in it: if this stutters,
    // the platform's audio path does, whatever the pipeline does.
    ControlTone.Play(options.ControlSeconds, options.Amplitude);
    await Task.Delay(TimeSpan.FromSeconds(options.GapSeconds));
}

foreach (var scenario in Scenario.All.Where(s => options.Scenarios.Contains(s.Name)))
{
    failures += await SpeakerCheck.RunAsync(scenario, options) ? 0 : 1;
    await Task.Delay(TimeSpan.FromSeconds(options.GapSeconds));
}

if (options.Mic)
{
    failures += await MicCheck.RunAsync(options) ? 0 : 1;
}

Console.WriteLine(failures == 0 ? "RESULT: PASS" : $"RESULT: FAIL ({failures} check(s))");
return failures == 0 ? 0 : 1;

internal sealed record Options(int Seconds, HashSet<string> Scenarios, bool Mic, int MicSeconds, short Amplitude, int ControlSeconds, int GapSeconds)
{
    public static Options Parse(string[] args)
    {
        var seconds = 30;
        var micSeconds = 10;
        var mic = true;
        var controlSeconds = 0;
        var gapSeconds = 0;
        short amplitude = 330; // about -40 dBFS: quiet on whatever device this lands on
        var scenarios = new HashSet<string>(Scenario.All.Select(s => s.Name));
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--seconds": seconds = int.Parse(args[++i]); break;
                case "--mic-seconds": micSeconds = int.Parse(args[++i]); break;
                case "--no-mic": mic = false; break;
                case "--control": controlSeconds = int.Parse(args[++i]); break;
                case "--gap": gapSeconds = int.Parse(args[++i]); break;
                case "--amplitude": amplitude = short.Parse(args[++i]); break;
                case "--scenario":
                    var name = args[++i];
                    scenarios = name == "all" ? scenarios : [.. name.Split(',')];
                    break;
                default: throw new ArgumentException($"unknown option {args[i]}");
            }
        }

        return new Options(seconds, scenarios, mic, micSeconds, amplitude, controlSeconds, gapSeconds);
    }
}

/// <summary>
/// How the simulated adapter's frames reach the companion.
/// </summary>
/// <param name="ClockPpm">The adapter's frame clock against this device's audio clock.</param>
/// <param name="EmptyEventFraction">Connection events that carry nothing (radio busy).</param>
/// <param name="StallEverySeconds">0, or a periodic outage of <paramref name="StallMillis"/>.</param>
/// <param name="Strict">Whether underruns fail the scenario (an outage beyond the prebuffer may underrun by design).</param>
internal sealed record Scenario(string Name, double ClockPpm, double EmptyEventFraction, int StallEverySeconds, int StallMillis, bool Strict)
{
    /// <summary>The simulations' interval: what the firmware now asks a central for.</summary>
    public const double EventMillis = 15.0;

    public const int NotificationsPerEvent = 2;

    /// <summary>
    /// Frames the adapter holds while it cannot send: three in the controller
    /// (BTstack's MAX_NR_CONTROLLER_ACL_BUFFERS) and two in the encoder queue
    /// (ENCODED_FRAME_QUEUE_DEPTH, oldest dropped). An outage therefore ends in
    /// a burst of at most this many frames, not the whole outage.
    /// </summary>
    public const int AdapterBacklogFrames = 3 + 2;

    // ±500 ppm is several times a real crystal pair's error: it makes the
    // drift control's work visible inside a short run.
    public static readonly Scenario[] All =
    [
        // No empty events: isolates this device's own pull pattern from the link.
        new("ideal", 0, 0, 0, 0, Strict: true),
        new("nominal", 0, 0.05, 0, 0, Strict: true),
        new("fast", +500, 0.05, 0, 0, Strict: true),
        new("slow", -500, 0.05, 0, 0, Strict: true),
        new("stall", +100, 0.05, 10, 150, Strict: false),
    ];
}

/// <summary>
/// The adapter, simulated: CELT 96 kbit/s stereo frames of a 1013 Hz tone, framed
/// as <c>ns2_companion_audio.h</c> frames them, released on BLE connection
/// events.
/// </summary>
internal sealed class SimulatedDownlink(Scenario scenario, short amplitude) : ICompanionAudioDownlink
{
    private readonly CancellationTokenSource stop = new();
    private Thread? thread;
    private long notifications;

    public event Action<byte[]>? FrameReceived;

    public int AttMtu => 247;

    public long NotificationsReceived => Interlocked.Read(ref notifications);

    public long FramesSent { get; private set; }

    /// <summary>Frames the adapter discarded during an outage (its queue was full).</summary>
    public long FramesShed { get; private set; }

    public Task<bool> OpenAsync(CancellationToken cancellationToken = default)
    {
        thread = new Thread(Run) { IsBackground = true, Name = "simulated-adapter", Priority = ThreadPriority.AboveNormal };
        thread.Start();
        return Task.FromResult(true);
    }

    private void Run()
    {
        var encoder = OpusCodecFactory.CreateEncoder(48_000, 2, OpusApplication.OPUS_APPLICATION_RESTRICTED_LOWDELAY);
        encoder.Bitrate = 96_000;
        encoder.UseVBR = false;
        encoder.Complexity = 10;
        encoder.Bandwidth = OpusBandwidth.OPUS_BANDWIDTH_FULLBAND;
        var pcm = new short[CompanionAudioDataPlane.SamplesPerChannel * 2];
        var packet = new byte[1275];
        var random = new Random(7);
        var frameMillis = CompanionAudioDataPlane.FrameMillis * (1.0 - scenario.ClockPpm * 1e-6);
        var pending = new Queue<byte[]>();
        long sample = 0;
        ushort sequence = 0;
        var clock = Stopwatch.StartNew();
        var nextFrame = 0.0;
        var nextEvent = Scenario.EventMillis;

        while (!stop.IsCancellationRequested)
        {
            var now = clock.Elapsed.TotalMilliseconds;
            while (nextFrame <= now)
            {
                for (var i = 0; i < CompanionAudioDataPlane.SamplesPerChannel; i++, sample++)
                {
                    var v = (short)(amplitude * Math.Sin(2 * Math.PI * Tone.Hertz * sample / 48_000.0));
                    pcm[2 * i] = v;
                    pcm[(2 * i) + 1] = v;
                }

                var n = encoder.Encode(pcm, CompanionAudioDataPlane.SamplesPerChannel, packet, packet.Length);
                if (n != CompanionAudioDataPlane.MaxOpusBytes || (packet[0] & 0xFC) != 0xFC)
                {
                    throw new InvalidOperationException($"encoder produced {n} B, TOC 0x{packet[0]:X2}; the adapter's frames are 240 B CELT stereo");
                }

                var frame = new byte[CompanionAudioDataPlane.HeaderBytes + n];
                frame[0] = CompanionAudioDataPlane.Version;
                frame[1] = CompanionAudioDataPlane.OpcodeOpus;
                packet.AsSpan(0, n).CopyTo(frame.AsSpan(CompanionAudioDataPlane.HeaderBytes));
                // Numbered as encoded (ns2_companion_audio_sequence), so a frame
                // shed here is a gap the receiver conceals.
                frame[2] = (byte)sequence;
                frame[3] = (byte)(sequence >> 8);
                sequence++;
                if (pending.Count == Scenario.AdapterBacklogFrames)
                {
                    pending.Dequeue();
                    FramesShed++;
                }

                pending.Enqueue(frame);
                nextFrame += frameMillis;
            }

            if (now >= nextEvent)
            {
                var stalled = scenario.StallEverySeconds > 0 &&
                              now % (scenario.StallEverySeconds * 1000.0) < scenario.StallMillis &&
                              now > 1000;
                if (!stalled && random.NextDouble() >= scenario.EmptyEventFraction)
                {
                    for (var i = 0; i < Scenario.NotificationsPerEvent && pending.Count > 0; i++)
                    {
                        var next = pending.Dequeue();
                        FramesSent++;
                        Interlocked.Increment(ref notifications);
                        FrameReceived?.Invoke(next);
                    }
                }

                nextEvent += Scenario.EventMillis;
            }

            var wait = Math.Min(nextFrame, nextEvent) - clock.Elapsed.TotalMilliseconds;
            if (wait > 1.5)
            {
                Thread.Sleep(1);
            }
            else if (wait > 0)
            {
                Thread.SpinWait(200);
            }
        }
    }

    public ValueTask DisposeAsync()
    {
        stop.Cancel();
        thread?.Join(1000);
        return ValueTask.CompletedTask;
    }
}

/// <summary>Measures what the decoder hands the sink, then passes it on unchanged.</summary>
internal sealed class MeasuringSinkFactory(ICompanionAudioSinkFactory inner, double amplitude) : ICompanionAudioSinkFactory
{
    public MeasuringSink? Last { get; private set; }

    public ICompanionAudioSink? CreateSink() =>
        inner.CreateSink() is { } sink ? Last = new MeasuringSink(sink) { Amplitude = amplitude } : null;
}

internal sealed class MeasuringSink(ICompanionAudioSink inner) : ICompanionAudioSink
{
    private const int SkipBlocks = 25; // decoder warm-up and the session's start
    private long blocks;
    private double sumSquares;
    private long samples;
    private readonly List<double> errors = [];
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private readonly List<double> glitchTimes = [];
    private readonly List<string> glitchKinds = [];
    private double x1;
    private double x2;
    private bool primed;
    private bool dropped;

    /// <summary>The tone's amplitude, which scales the discontinuity threshold.</summary>
    public double Amplitude { get; init; } = 330;

    /// <summary>
    /// Discontinuities in what was actually queued for the device: seconds since
    /// the session started, and whether the block before it was dropped.
    /// A clean sine obeys x[n] = 2cos(w)x[n-1] - x[n-2]; codec noise
    /// leaves a small residual, a splice (a dropped or skipped block, a
    /// concealment seam) a large one.
    /// </summary>
    public (double Seconds, string Kind)[] Glitches
    {
        get { lock (glitchTimes) { return [.. glitchTimes.Zip(glitchKinds)]; } }
    }

    public double MaxResidual { get; private set; }

    private void Inspect(ReadOnlySpan<short> interleaved)
    {
        var c = 2 * Math.Cos(2 * Math.PI * Tone.Hertz / 48_000.0);
        var threshold = 0.3 * Amplitude;
        var glitch = false;
        for (var i = 0; i < interleaved.Length; i += 2)
        {
            double x = interleaved[i];
            if (primed)
            {
                var r = Math.Abs(x - c * x1 + x2);
                if (blocks > SkipBlocks)
                {
                    MaxResidual = Math.Max(MaxResidual, r);
                    glitch |= r > threshold;
                }
            }

            x2 = x1;
            x1 = x;
            primed |= i >= 2;
        }

        if (glitch)
        {
            lock (glitchTimes)
            {
                glitchTimes.Add(clock.Elapsed.TotalSeconds);
                glitchKinds.Add(dropped ? "after-drop" : "in-stream");
            }
        }
    }

    public double Rms => samples == 0 ? 0 : Math.Sqrt(sumSquares / samples);

    /// <summary>
    /// Pre-write depth minus the depth the drift control holds,
    /// (prebuffer - 1 block): CompanionAudioPlayout.TargetMillis.
    /// </summary>
    public double[] DepthErrors { get { lock (errors) { return [.. errors]; } } }

    public void Start(int sampleRate, int channels) => inner.Start(sampleRate, channels);

    public bool Write(ReadOnlySpan<short> interleaved)
    {
        lock (errors)
        {
            errors.Add(inner.QueuedMillis - (inner.PrebufferMillis - CompanionAudioJitterPolicy.BlockMillis));
        }

        if (++blocks > SkipBlocks)
        {
            foreach (var s in interleaved)
            {
                sumSquares += (double)s * s;
            }

            samples += interleaved.Length;
        }

        var accepted = inner.Write(interleaved);
        if (accepted)
        {
            // Only what the device will play: a dropped block is a splice there.
            Inspect(interleaved);
        }

        dropped = !accepted;
        return accepted;
    }

    public void Stop() => inner.Stop();

    public int QueuedMillis => inner.QueuedMillis;

    public long Underruns => inner.Underruns;

    public int PrebufferMillis => inner.PrebufferMillis;

    public bool TakeConcealRequest() => inner.TakeConcealRequest();

    public long Concealments => inner.Concealments;

    public void Dispose() => inner.Dispose();
}

/// <summary>An adapter that admits every session, as a current Pico 2 W build does.</summary>
internal sealed class SimulatedManagement(Func<ICompanionAudioDownlink> downlinks, Func<ICompanionMicUplink?> uplinks) : ICompanionAudioManagement
{
    private bool active;
    private bool mic;

    public bool Ready => true;

    public string? UnavailableReason => null;

    public event Action? Changed { add { } remove { } }

    public Task<CompanionAudioState> StartAsync(bool microphone, CancellationToken cancellationToken = default)
    {
        active = true;
        mic = microphone;
        return Task.FromResult(State());
    }

    public Task<CompanionAudioState> StopAsync(CancellationToken cancellationToken = default)
    {
        active = false;
        mic = false;
        return Task.FromResult(State());
    }

    public Task<CompanionAudioState> StatusAsync(CancellationToken cancellationToken = default) => Task.FromResult(State());

    public ICompanionAudioDownlink? TryCreateDownlink() => downlinks();

    public ICompanionMicUplink? TryCreateMicUplink() => uplinks();

    private CompanionAudioState State() => new(
        Supported: true, Active: active, Subscribed: active, UsbSpeakerActive: active, Version: 1, Codec: "opus",
        SampleRate: 48_000, Channels: 2, FrameMillis: 20, FrameBytes: 244, AttMtu: 247, MinimumAttMtu: 247,
        Admit: "ok", Kbps: 96, Mic: mic, MicSupported: true);
}

internal static class SpeakerCheck
{
    public static async Task<bool> RunAsync(Scenario scenario, Options options)
    {
        SimulatedDownlink? downlink = null;
        var sinks = new MeasuringSinkFactory(new SdlCompanionAudioSinkFactory(), options.Amplitude);
        var management = new SimulatedManagement(() => downlink = new SimulatedDownlink(scenario, options.Amplitude), () => null);
        var log = new DiagnosticLog();
        await using var service = new CompanionAudioService(management, sinks, log);

        await service.StartAsync();
        if (scenario == Scenario.All[0] || !options.Scenarios.Contains(Scenario.All[0].Name))
        {
            // Which backend SDL opened: PipeWire, PulseAudio or ALSA.
            Console.WriteLine($"audio driver: {SDL3.SDL_GetCurrentAudioDriver() ?? "(none open)"}");
        }

        if (service.View.Value.Phase is not CompanionAudioPhase.Playing)
        {
            Console.WriteLine($"[{scenario.Name}] FAIL: did not start ({service.View.Value.Phase}: {service.View.Value.Detail})");
            DumpLog(log);
            return false;
        }

        await Task.Delay(TimeSpan.FromSeconds(options.Seconds));

        var m = service.Metrics;
        var sent = downlink?.FramesSent ?? 0;
        var shed = downlink?.FramesShed ?? 0;

        // The drift control's job: hold the pre-write depth at its target,
        // whatever the clocks do. Judged over the last two thirds, and on the
        // mean, since single blocks swing with each connection event.
        var errors = sinks.Last?.DepthErrors ?? [];
        var held = errors.Skip(errors.Length / 3).ToArray();
        var meanError = held.Length == 0 ? double.NaN : held.Average();
        var half = held.Length / 2;
        var trend = held.Length < 2 ? double.NaN : held.Skip(half).Average() - held.Take(half).Average();

        // Where it ends: a drift the control cannot hold grows without bound,
        // so it shows here; a perturbation it is still absorbing does not.
        var tail = errors.Skip(errors.Length * 4 / 5).ToArray();
        var finalError = tail.Length == 0 ? double.NaN : tail.Average();
        var rms = sinks.Last?.Rms ?? 0;
        var expectedRms = options.Amplitude / Math.Sqrt(2);
        var levelDb = rms <= 0 ? double.NegativeInfinity : 20 * Math.Log10(rms / expectedRms);
        await service.StopAsync();

        var checks = new List<(string Name, bool Ok)>
        {
            // Every gap is a frame the simulated adapter shed; nothing else is lost.
            ("lost == shed at adapter", m.FramesLost == shed),
            ("malformed/decode errors 0", m.FramesMalformed == 0 && m.DecodeErrors == 0),
            ("decoded >= 99% of sent", m.FramesDecoded >= sent * 0.99),
            ("tone level within 1 dB", Math.Abs(levelDb) <= 1.0),
        };
        if (scenario.Strict)
        {
            checks.Add(("underruns 0", m.SinkUnderruns == 0));
            checks.Add(("no sink drops", m.SinkDrops == 0));
            checks.Add(("pre-write depth held within ±8 ms of target", Math.Abs(meanError) <= 8));
            checks.Add(("final depth within ±8 ms of target", Math.Abs(finalError) <= 8));
        }

        var ok = checks.All(c => c.Ok);
        Console.WriteLine(
            $"[{scenario.Name}] {(ok ? "PASS" : "FAIL")} ppm={scenario.ClockPpm:+0;-0;0} sent={sent} shed_at_adapter={shed} " +
            $"notified={m.Notifications} decoded={m.FramesDecoded} lost={m.FramesLost} concealed={m.FramesConcealed} " +
            $"stale={m.FramesStale} sink_drops={m.SinkDrops} underruns={m.SinkUnderruns} growths={m.BufferGrowths} " +
            $"prebuffer={m.PrebufferMillis}ms depth_error[mean/final/trend]={meanError:+0.0;-0.0}/{finalError:+0.0;-0.0}/{trend:+0.0;-0.0}ms " +
            $"level={levelDb:+0.00;-0.00;0.00}dB");
        foreach (var failed in checks.Where(c => !c.Ok))
        {
            Console.WriteLine($"[{scenario.Name}]   failed: {failed.Name}");
        }

        var glitches = sinks.Last?.Glitches ?? [];
        Console.WriteLine(
            $"[{scenario.Name}]   splices in the queued output: {glitches.Length} " +
            $"(max residual {sinks.Last?.MaxResidual ?? 0:0}, threshold {0.3 * options.Amplitude:0})" +
            (glitches.Length == 0 ? "" : " at " + string.Join(", ", glitches.Take(20).Select(g => $"{g.Seconds:0.0}s/{g.Kind}"))));

        if (!ok)
        {
            DumpLog(log);
        }

        return ok;
    }

    internal static void DumpLog(DiagnosticLog log)
    {
        foreach (var entry in log.Snapshot().TakeLast(15))
        {
            Console.WriteLine($"    log: {entry}");
        }
    }
}

/// <summary>Counts and validates what the shipped capture and encoder would write to MIC-UP.</summary>
internal sealed class CountingUplink : ICompanionMicUplink
{
    private long written;
    private long invalid;
    private long gaps;
    private int? lastSequence;
    private readonly List<double> arrivals = [];
    private readonly Stopwatch clock = Stopwatch.StartNew();

    public CompanionMicWriter.Counters Stats => new(Interlocked.Read(ref written), Interlocked.Read(ref written), 0, 0);

    public long Invalid => Interlocked.Read(ref invalid);

    public long Gaps => Interlocked.Read(ref gaps);

    public double[] Arrivals { get { lock (arrivals) { return [.. arrivals]; } } }

    public Task<bool> OpenAsync(CancellationToken cancellationToken = default) => Task.FromResult(true);

    public void Send(byte[] frame)
    {
        // [version][0x91][seq lo][seq hi] + a 160-byte CELT mono packet (TOC 0xF8).
        if (frame.Length != 4 + 160 || frame[1] != 0x91 || !CompanionMicDataPlane.IsValidPacket(frame.AsSpan(4)))
        {
            Interlocked.Increment(ref invalid);
            return;
        }

        var sequence = frame[2] | (frame[3] << 8);
        if (lastSequence is { } last && sequence != ((last + 1) & 0xFFFF))
        {
            Interlocked.Increment(ref gaps);
        }

        lastSequence = sequence;
        Interlocked.Increment(ref written);
        lock (arrivals)
        {
            arrivals.Add(clock.Elapsed.TotalMilliseconds);
        }
    }

    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}

internal static class MicCheck
{
    public static async Task<bool> RunAsync(Options options)
    {
        var uplink = new CountingUplink();
        var management = new SimulatedManagement(
            () => new SimulatedDownlink(Scenario.All[0], options.Amplitude), () => uplink);
        var log = new DiagnosticLog();
        await using var service = new CompanionAudioService(
            management, new SdlCompanionAudioSinkFactory(), log, micCaptures: new SdlCompanionMicCaptureFactory());

        await service.SetMicrophoneAsync(true);
        await service.StartAsync();
        var view = service.View.Value;
        if (view.Phase is not CompanionAudioPhase.Playing || !view.Microphone)
        {
            Console.WriteLine($"[mic] FAIL: microphone not running ({view.Phase}, mic={view.Microphone}: {view.MicrophoneDetail ?? view.Detail})");
            SpeakerCheck.DumpLog(log);
            return false;
        }

        await Task.Delay(TimeSpan.FromSeconds(options.MicSeconds));
        var metrics = service.MicMetrics;
        await service.StopAsync();

        // The steady rate, from the first frame on: the device's start-up
        // latency is not a rate problem.
        var arrivals = uplink.Arrivals;
        var span = arrivals.Length > 1 ? arrivals[^1] - arrivals[0] : 0;
        var rate = span > 0 ? (arrivals.Length - 1) * 1000.0 / span : 0;
        var maxGap = arrivals.Zip(arrivals.Skip(1), (a, b) => b - a).DefaultIfEmpty(0).Max();
        var checks = new List<(string Name, bool Ok)>
        {
            ("steady rate 50 ±1 frames/s", Math.Abs(rate - 50) <= 1),
            ("every frame valid", uplink.Invalid == 0),
            ("no sequence gaps", uplink.Gaps == 0),
            ("no encode rejections", metrics.EncodeRejected == 0),
        };
        var ok = checks.All(c => c.Ok);
        Console.WriteLine(
            $"[mic] {(ok ? "PASS" : "FAIL")} captured={metrics.BlocksCaptured} encoded={metrics.FramesEncoded} " +
            $"written={uplink.Stats.Written} first_after={(arrivals.Length > 0 ? arrivals[0] : 0):0}ms " +
            $"rate={rate:0.00}/s max_gap={maxGap:0}ms invalid={uplink.Invalid} gaps={uplink.Gaps}");
        foreach (var failed in checks.Where(c => !c.Ok))
        {
            Console.WriteLine($"[mic]   failed: {failed.Name}");
        }

        return ok;
    }
}

/// <summary>
/// A 1 kHz tone straight into an SDL3 stream, kept 200 ms ahead: no decoder,
/// no playout policy, no drift control. The listening reference.
/// </summary>
internal static unsafe class ControlTone
{
    public static void Play(int seconds, short amplitude)
    {
        if (!SDL3.SDL_InitSubSystem(SDL_InitFlags.SDL_INIT_AUDIO))
        {
            Console.WriteLine($"[control] FAIL: {SDL3.SDL_GetError()}");
            return;
        }

        var spec = new SDL_AudioSpec { format = SDL_AudioFormat.SDL_AUDIO_S16LE, channels = 2, freq = 48_000 };
        var stream = SDL3.SDL_OpenAudioDeviceStream(SDL3.SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, null, IntPtr.Zero);
        if (stream is null)
        {
            Console.WriteLine($"[control] FAIL: {SDL3.SDL_GetError()}");
            SDL3.SDL_QuitSubSystem(SDL_InitFlags.SDL_INIT_AUDIO);
            return;
        }

        Console.WriteLine($"[control] {seconds}s plain tone, driver {SDL3.SDL_GetCurrentAudioDriver()}");
        var block = new short[960 * 2];
        long sample = 0;
        var clock = Stopwatch.StartNew();
        var dry = 0;
        SDL3.SDL_ResumeAudioStreamDevice(stream);
        while (clock.Elapsed.TotalSeconds < seconds)
        {
            var queued = SDL3.SDL_GetAudioStreamQueued(stream) / 4;
            if (queued == 0 && sample > 0)
            {
                dry++;
            }

            while (queued < 48 * 200)
            {
                for (var i = 0; i < 960; i++, sample++)
                {
                    var v = (short)(amplitude * Math.Sin(2 * Math.PI * Tone.Hertz * sample / 48_000.0));
                    block[2 * i] = v;
                    block[(2 * i) + 1] = v;
                }

                fixed (short* data = block)
                {
                    SDL3.SDL_PutAudioStreamData(stream, (IntPtr)data, block.Length * sizeof(short));
                }

                queued += 960;
            }

            Thread.Sleep(10);
        }

        SDL3.SDL_DestroyAudioStream(stream);
        SDL3.SDL_QuitSubSystem(SDL_InitFlags.SDL_INIT_AUDIO);
        Console.WriteLine($"[control] done; stream ran dry {dry} time(s)");
    }
}

internal static class Tone
{
    /// <summary>
    /// Not a multiple of 50 Hz on purpose: 1 kHz is exactly 20 cycles per 20 ms
    /// block, so a whole dropped or concealed block would leave the sine
    /// continuous and invisible to the splice detector.
    /// </summary>
    public const double Hertz = 1013;
}
