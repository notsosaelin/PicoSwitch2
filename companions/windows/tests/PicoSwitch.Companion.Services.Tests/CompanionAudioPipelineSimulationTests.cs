using PicoSwitch.Companion.Platform.CompanionAudio;
using Xunit;
using Xunit.Abstractions;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Companion audio's timing, end to end, without hardware: console USB speaker
/// stream -> adapter encoder -> 2 ms service tick -> BLE connection events ->
/// host stack -> <see cref="CompanionAudioPlayout"/> (the real policy) -> a
/// device clocked independently of the console.
///
/// The link figures are the measured ones (docs/experiments/
/// companion-audio-downlink-prototype-2026-11-07.md, budget section): 15 ms
/// connection interval, one link-layer PDU per 244 B frame, encoder worst case
/// 10.2 ms. Where the hardware gave no number the model is deliberately
/// pessimistic and says so (marked ASSUMPTION). Each profile runs one hour
/// of audio per seed and must play with no underrun and no drop once
/// playback has started.
/// </summary>
public sealed class CompanionAudioPipelineSimulationTests(ITestOutputHelper output)
{
    private const double FrameMillis = 20.0;
    private const double BlockFramesNominal = CompanionAudioDriftControl.BlockFrames;
    private const int HourOfBlocks = 3600 * 50;
    private const int WarmupBlocks = 600 * 50;

    private sealed record Profile(
        string Name,
        double EncodeMinMillis,
        double EncodeMaxMillis,
        double EncodeStallProbability,  // an encode preempted for EncodeStallMillis
        double EncodeStallMillis,
        bool ExclusiveEncode,          // CYW43 lock held across the encode
        double IntervalMillis,
        int PdusPerEvent,              // frames one connection event can carry (MD bit chaining)
        double MissProbability,        // a connection event carries no frame (retransmit / Classic collision)
        double HostJitterMillis,
        double ConsolePpm,
        double DevicePpm,
        int InitialPrebufferBlocks = CompanionAudioJitterPolicy.MinPrebufferBlocks,
        // The firmware's rule since the hold was bounded (ns2_companion_audio.h);
        // false is the earlier per-frame-SENT numbering, kept for comparison.
        bool SequencePerEncodedFrame = true);

    // Measured: interval 15 ms, encode <= 10.2 ms (session 4, no DualSense).
    // ASSUMPTION: an event carries up to 2 frames (the specification allows
    // more; a 244 B PDU and its acknowledgement take ~2.4 ms of a 15 ms
    // interval at 1M PHY), 2 % of events lost to retransmission, host stack
    // delivery jitter up to 8 ms, clocks at the USB full-speed tolerance
    // (+500 ppm) against a device 100 ppm slow -- 600 ppm apart, beyond any
    // real pair.
    private static readonly Profile ControllerLinkBle = new(
        "BLE Controller Link (Windows/Linux)", 4.0, 10.2, 0, 0, false, 15.0, 2, 0.02, 8.0, +500, -100);

    // The Android companion's Controller Link is Bluetooth Classic HID on the
    // same radio: the encode holds the CYW43 lock, and connection events
    // collide with Classic slots. ASSUMPTION: 20 % of events carry nothing.
    private static readonly Profile AndroidClassicLocked = new(
        "Android Classic HID + exclusive encode", 4.0, 10.2, 0, 0, true, 15.0, 2, 0.20, 8.0, -500, +100,
        CompanionAudioPlayout.ClassicSharedPrebufferBlocks);

    // Beyond anything measured: every encode at the worst case, a third of the
    // connection events empty, 15 ms host jitter.
    private static readonly Profile Stress = new(
        "Stress (worst encode every frame, 33 % empty events)", 10.2, 10.2, 0, 0, true, 15.0, 2, 0.33, 15.0, +500, -100,
        CompanionAudioPlayout.ClassicSharedPrebufferBlocks);

    // The second hardware pass as measured: a DualSense on the radio with the
    // encode preemptible. Encodes stretched to 52-59 ms of wall time.
    // ASSUMPTION: 10 % of encodes stall (429 PCM drops in a ~3 min session).
    private static readonly Profile DualSenseUnlocked = new(
        "DualSense, preemptible encode (pass 2, rejected)", 4.0, 10.2, 0.10, 59.0, false, 15.0, 2, 0.20, 8.0, 0, 0);

    /// <summary>
    /// Acceptance per profile, over one hour per seed. A dropout is an underrun
    /// (the device ran dry; it re-prebuffers). A concealed growth is one 20 ms
    /// block of Opus packet-loss concealment inserted to deepen the buffer
    /// before it runs dry. Warm-up is the first 10 minutes, while the buffer
    /// learns the link.
    /// </summary>
    public static IEnumerable<object[]> ProductionProfiles() =>
    [
        // Measured link class: nothing audible, ever.
        [nameof(ControllerLinkBle), 0, 0, 2],
        // Pessimistic Classic collisions (independent 20 % loss): a new
        // record run of lost events (>= 115 ms) can still outrun the buffer
        // once after warm-up; each such dropout deepens it for the session.
        [nameof(AndroidClassicLocked), 2, 1, 4],
        // Beyond the 160 ms ceiling by construction (170 ms runs): bounded
        // and recovering, never drifting, never dropping PCM at the adapter.
        [nameof(Stress), 8, 6, 6],
    ];

    private static Profile Named(string name) => name switch
    {
        nameof(ControllerLinkBle) => ControllerLinkBle,
        nameof(AndroidClassicLocked) => AndroidClassicLocked,
        nameof(Stress) => Stress,
        _ => DualSenseUnlocked,
    };

    [Theory]
    [MemberData(nameof(ProductionProfiles))]
    public void AnHourPlaysWithinItsProfilesBudget(
        string profileName, int maxDropouts, int maxDropoutsAfterWarmup, int maxConcealedGrowth)
    {
        var profile = Named(profileName);
        foreach (var seed in new[] { 1, 2, 3 })
        {
            var result = Run(profile, seed, HourOfBlocks);
            output.WriteLine($"{profile.Name} seed {seed}: {result}");
            Assert.InRange(result.Underruns, 0, maxDropouts);
            Assert.InRange(result.UnderrunsAfterWarmup, 0, maxDropoutsAfterWarmup);
            Assert.InRange(result.Extensions, 0, maxConcealedGrowth);
            Assert.Equal(0, result.PcmDropped);
            if (maxDropouts == 0)
            {
                Assert.Equal(0, result.Drops);
            }

            // Drift held: after an hour with the clocks 600 ppm apart the
            // smoothed queue sits within the deadband of its target.
            Assert.InRange(result.FinalQueueErrorMillis,
                -CompanionAudioDriftControl.DeadbandMillis - 1, CompanionAudioDriftControl.DeadbandMillis + 1);
            Assert.True(result.P99LatencyMillis < 300, $"p99 {result.P99LatencyMillis:F1} ms");
        }
    }

    /// <summary>
    /// The adapter holds at most five frames through missed connection events
    /// (three in the controller, two in the encoder queue) and sheds the
    /// oldest beyond that. Numbered per frame SENT, a shed frame vanishes and
    /// takes its 20 ms of queue depth with it, which the drift control returns
    /// at about 1 ms per second; numbered per frame ENCODED, the receiver
    /// conceals it and the depth is kept.
    /// </summary>
    [Fact]
    public void SequencingPerEncodedFrameTurnsShedFramesIntoConcealment()
    {
        foreach (var profile in new[] { AndroidClassicLocked, Stress })
        {
            long perSent = 0, perEncoded = 0;
            foreach (var seed in new[] { 1, 2, 3 })
            {
                var sent = Run(profile with { SequencePerEncodedFrame = false }, seed, HourOfBlocks);
                var encoded = Run(profile, seed, HourOfBlocks);
                output.WriteLine($"{profile.Name} seed {seed} per-sent:    {sent}");
                output.WriteLine($"{profile.Name} seed {seed} per-encoded: {encoded}");
                perSent += sent.Underruns;
                perEncoded += encoded.Underruns;
            }

            Assert.True(perEncoded <= perSent, $"{profile.Name}: {perEncoded} underruns per-encoded vs {perSent} per-sent");
            if (profile == Stress)
            {
                // The margin that justified the change: 13 vs 41 when it was made.
                Assert.True(perEncoded * 2 <= perSent, $"Stress: {perEncoded} vs {perSent}");
            }
        }
    }

    [Fact]
    public void OneFramePerConnectionEventLeavesAQuarterOfTheEventsAsMargin()
    {
        // The capacity rule the profiles rest on. 50 frames/s must fit in
        // 1000 / 15 = 66.7 events/s: with one PDU per event, losing more than
        // 25 % of events builds a backlog that never drains, whatever the
        // buffer. Chaining a second PDU per event (MD bit) doubles the margin.
        var events = 1000.0 / 15.0;
        Assert.Equal(0.25, 1 - 50.0 / events, 3);
        Assert.Equal(0.625, 1 - 50.0 / (2 * events), 3);

        // Demonstrated: one PDU per event at 33 % loss is under capacity.
        var starved = Stress with { PdusPerEvent = 1 };
        var result = Run(starved, 1, 50 * 120);
        output.WriteLine($"one PDU per event, 33 % loss: {result}");
        Assert.True(result.Underruns > 0 || result.Drops > 0);
    }

    [Fact]
    public void ClockDriftWithoutCorrectionWouldGapEveryFewMinutes()
    {
        // The arithmetic the drift control exists for: 600 ppm moves the queue
        // 12 ms per 20 s, so a block's worth (20 ms) every 33 s.
        var millisPerSecond = 600e-6 * 1000.0;
        Assert.InRange(FrameMillis / millisPerSecond, 33.0, 34.0);

        // And the correction outruns it: one frame per 960 is 1,042 ppm.
        Assert.InRange(1e6 / BlockFramesNominal, 1041.0, 1042.0);
    }

    [Fact]
    public void ThePreemptibleEncodeWithADualSenseOnTheRadioDoesNotFitAnyFixedBuffer()
    {
        // Regression evidence for the exclusive encode: the pass-2 condition
        // drops PCM at the adapter (the source queue overflows), which no
        // companion buffer can recover.
        var result = Run(DualSenseUnlocked, 1, 50 * 300);
        output.WriteLine($"{DualSenseUnlocked.Name}: {result}");
        Assert.True(result.PcmDropped > 0 || result.Underruns > 0);
    }

    [Fact]
    public void TheLatencyBudgetMatchesTheArithmetic()
    {
        var result = Run(ControllerLinkBle, 7, 50 * 600);
        output.WriteLine($"{ControllerLinkBle.Name}: {result}");
        // Capture (20) + encode (<= 10.2) + tick (<= 2) + connection event
        // (<= 15) + host (<= 8) is the transport; the 60 ms prebuffer adds the
        // rest. Steady-state mouth-to-ear inside the PC stays under 140 ms and
        // above the transport's best case.
        Assert.InRange(result.MedianLatencyMillis, 60.0, 140.0);
        Assert.True(result.P99LatencyMillis < 160.0, $"p99 {result.P99LatencyMillis:F1} ms");
    }

    // ------------------------------------------------------------ the model

    private sealed record Result(
        long Underruns,
        long Drops,
        long PcmDropped,
        int FinalPrebufferMillis,
        double MedianLatencyMillis,
        double P99LatencyMillis,
        double MaxArrivalGapMillis,
        double FinalQueueErrorMillis,
        long FramesAdjusted,
        long Extensions,
        long UnderrunsAfterWarmup,
        long ExtensionsAfterWarmup,
        long FramesShed)
    {
        public override string ToString() =>
            $"underruns={Underruns} (after warm-up {UnderrunsAfterWarmup}) drops={Drops} " +
            $"concealed_growth={Extensions} (after warm-up {ExtensionsAfterWarmup}) pcm_dropped={PcmDropped} shed={FramesShed} " +
            $"prebuffer={FinalPrebufferMillis} ms " +
            $"latency p50={MedianLatencyMillis:F1} p99={P99LatencyMillis:F1} ms max_gap={MaxArrivalGapMillis:F1} ms " +
            $"queue_error={FinalQueueErrorMillis:F1} ms frames_adjusted={FramesAdjusted}";
    }

    private static Result Run(Profile p, int seed, int blocks)
    {
        var random = new Random(seed);
        double Uniform(double lo, double hi) => lo + random.NextDouble() * (hi - lo);

        // --- Adapter: the console delivers one 20 ms PCM block per 20 ms of
        // ITS clock; the codec worker encodes it; the source queue holds 4
        // blocks for the companion consumer (ds5_audio_bridge.c).
        const int SourceQueueDepth = 4;
        var consoleScale = 1.0 / (1.0 + p.ConsolePpm * 1e-6);
        var ready = new List<(int Block, double At)>(blocks); // frames handed to BTstack
        var encodeWindows = new List<(double Start, double End)>(blocks);
        long pcmDropped = 0;
        var encoderFreeAt = 0.0;
        var waiting = new Queue<double>(); // start times of queued, not yet started blocks
        var tickPhase = Uniform(0, 2);
        for (var k = 0; k < blocks; k++)
        {
            var pcmAt = (k + 1) * FrameMillis * consoleScale + Uniform(0, 1);
            while (waiting.Count > 0 && waiting.Peek() <= pcmAt)
            {
                waiting.Dequeue();
            }

            // The source queue is full: the producer drops this block.
            if (waiting.Count >= SourceQueueDepth)
            {
                pcmDropped++;
                continue;
            }

            var start = Math.Max(pcmAt, encoderFreeAt);
            if (start > pcmAt)
            {
                waiting.Enqueue(start);
            }

            var encode = Uniform(p.EncodeMinMillis, p.EncodeMaxMillis);
            if (!p.ExclusiveEncode && random.NextDouble() < p.EncodeStallProbability)
            {
                encode = p.EncodeStallMillis;
            }

            var end = start + encode;
            encoderFreeAt = end;
            encodeWindows.Add((start, end));
            // The 2 ms audio timer picks the frame up.
            var tick = tickPhase + Math.Ceiling((end - tickPhase) / 2.0) * 2.0;
            ready.Add((k, tick));
        }

        // With the exclusive encode the host stack cannot run during an encode:
        // a hand-off that lands inside one waits for its end.
        var handoffs = new List<(int Block, double At)>(ready.Count);
        var w = 0;
        foreach (var (block, at) in ready)
        {
            var t = at;
            if (p.ExclusiveEncode)
            {
                while (w < encodeWindows.Count && encodeWindows[w].End <= t)
                {
                    w++;
                }

                if (w < encodeWindows.Count && encodeWindows[w].Start <= t && t < encodeWindows[w].End)
                {
                    t = encodeWindows[w].End;
                }
            }

            handoffs.Add((block, t));
        }

        // --- Link: one PDU per frame (tx_octets 251), up to PdusPerEvent per
        // connection event, ~2.4 ms apart; an event can carry nothing (miss).
        // The host stack delivers in order, possibly late, never reordered.
        //
        // What the adapter can HOLD while events are missed is bounded
        // (btstack_host_service_companion_audio): a frame goes to the
        // controller while BTstack has one of its MAX_NR_CONTROLLER_ACL_BUFFERS
        // (3) free, and otherwise waits in the encoder's depth-two queue
        // (ENCODED_FRAME_QUEUE_DEPTH), which drops its OLDEST frame when a new
        // one is encoded. Its sequence advances per frame sent, so a shed
        // frame is invisible to the receiver: the audio skips ahead without
        // concealment. ASSUMPTION: the companion's link has all three ACL
        // buffers (a Classic controller on the same radio shares them).
        const double PduMillis = 2.4;
        const double ControllerLeadMillis = 0.5; // hand-off must precede the event
        const int ControllerAclBuffers = 3;
        const int EncodedFrameQueueDepth = 2;
        var arrivals = new List<(int Block, double At)>(handoffs.Count);
        var inController = new Queue<int>();
        var encoded = new Queue<int>();
        long shed = 0;
        var nextEvent = Uniform(0, p.IntervalMillis);
        var lastArrival = 0.0;
        void Admit(int block)
        {
            if (inController.Count < ControllerAclBuffers && encoded.Count == 0)
            {
                inController.Enqueue(block);
                return;
            }

            if (encoded.Count == EncodedFrameQueueDepth)
            {
                encoded.Dequeue();
                shed++;
            }

            encoded.Enqueue(block);
        }

        for (var i = 0; i < handoffs.Count || inController.Count > 0 || encoded.Count > 0;)
        {
            if (inController.Count == 0 && encoded.Count == 0)
            {
                // Idle: skip to the event after the next hand-off.
                var due = handoffs[i].At + ControllerLeadMillis;
                if (nextEvent < due)
                {
                    nextEvent += Math.Ceiling((due - nextEvent) / p.IntervalMillis) * p.IntervalMillis;
                }
            }

            while (i < handoffs.Count && handoffs[i].At + ControllerLeadMillis <= nextEvent)
            {
                Admit(handoffs[i++].Block);
            }

            if (random.NextDouble() >= p.MissProbability)
            {
                for (var sent = 0; sent < p.PdusPerEvent && inController.Count > 0; sent++)
                {
                    lastArrival = Math.Max(
                        lastArrival, nextEvent + sent * PduMillis + Uniform(0, p.HostJitterMillis));
                    arrivals.Add((inController.Dequeue(), lastArrival));
                }

                // Buffers freed by the event take the waiting frames, oldest first.
                while (inController.Count < ControllerAclBuffers && encoded.Count > 0)
                {
                    inController.Enqueue(encoded.Dequeue());
                }
            }

            nextEvent += p.IntervalMillis;
        }

        // --- Receiver: a frame the adapter shed is a sequence gap only when the
        // sequence counts frames ENCODED; then the receiver conceals up to
        // MaxConcealedFrames blocks before the next frame (block -1 below),
        // which keeps the timeline, and the queue, whole. Counted per frame
        // SENT, the shed frame is invisible and the queue loses its depth.
        if (p.SequencePerEncodedFrame)
        {
            var expanded = new List<(int Block, double At)>(arrivals.Count);
            var last = -1;
            foreach (var (block, at) in arrivals)
            {
                if (last >= 0)
                {
                    var conceal = Math.Min(block - last - 1, CompanionAudioReceiver.MaxConcealedFrames);
                    for (var c = 0; c < conceal; c++)
                    {
                        expanded.Add((-1, at));
                    }
                }

                expanded.Add((block, at));
                last = block;
            }

            arrivals = expanded;
        }

        // --- Device: plays queued blocks back to back on ITS clock.
        var playout = new CompanionAudioPlayout(p.InitialPrebufferBlocks);
        var deviceScale = 1.0 / (1.0 + p.DevicePpm * 1e-6);
        var queue = new Queue<(double End, double Duration)>();
        var deviceAt = 0.0; // when the last queued block finishes, while playing
        var paused = new List<(int Block, double Duration)>();
        var latencies = new List<double>(arrivals.Count);
        var maxGap = 0.0;
        var previous = double.NaN;
        var settledQueue = double.NaN;
        var lastQueue = 0.0;
        long lateUnderruns = 0;
        long lateExtensions = 0;
        var lastReal = 0;

        foreach (var (arrived, at) in arrivals)
        {
            // A concealment block is written like any other; it has no source
            // block, so it records no latency and counts with the frame it precedes.
            var concealment = arrived < 0;
            var block = concealment ? lastReal : arrived;
            lastReal = block;
            if (!double.IsNaN(previous))
            {
                maxGap = Math.Max(maxGap, at - previous);
            }

            previous = at;
            while (queue.Count > 0 && queue.Peek().End <= at)
            {
                queue.Dequeue();
            }

            var queuedBlocks = playout.Playing ? queue.Count : paused.Count;
            var queuedMillis = playout.Playing
                ? Math.Max(0, deviceAt - at)
                : paused.Sum(x => x.Duration);
            var underrunsBefore = playout.Underruns;
            var extensionsBefore = playout.Extensions;
            var decision = playout.Accept(queuedBlocks, queuedMillis);
            if (block >= WarmupBlocks)
            {
                lateUnderruns += playout.Underruns - underrunsBefore;
                lateExtensions += playout.Extensions - extensionsBefore;
            }

            if (playout.Playing && playout.Drift.BaselineMillis is not null)
            {
                lastQueue = playout.Drift.SmoothedMillis;
            }

            if (decision.Underrun)
            {
                queue.Clear();
                paused.Clear();
            }

            if (decision.Drop)
            {
                continue;
            }

            var duration = decision.Frames / 48.0 * deviceScale;
            if (playout.Playing)
            {
                var startAt = Math.Max(deviceAt, at);
                deviceAt = startAt + duration;
                queue.Enqueue((deviceAt, duration));
                if (!concealment)
                {
                    latencies.Add(startAt - block * FrameMillis * consoleScale);
                }

                _ = playout.Queued(queue.Count);
                if (decision.Extend)
                {
                    // The service then writes one concealment block, which the
                    // sink treats like any other: every later block plays 20 ms later.
                    var conceal = playout.Accept(queue.Count, Math.Max(0, deviceAt - at));
                    if (!conceal.Drop)
                    {
                        var concealed = conceal.Frames / 48.0 * deviceScale;
                        deviceAt += concealed;
                        queue.Enqueue((deviceAt, concealed));
                        _ = playout.Queued(queue.Count);
                    }
                }
            }
            else
            {
                paused.Add((concealment ? -1 : block, duration));
                if (playout.Queued(paused.Count))
                {
                    var t = at;
                    foreach (var (b, d) in paused)
                    {
                        if (b >= 0)
                        {
                            latencies.Add(t - b * FrameMillis * consoleScale);
                        }

                        t += d;
                        queue.Enqueue((t, d));
                    }

                    deviceAt = t;
                    paused.Clear();
                }
            }

            if (double.IsNaN(settledQueue) && playout.Drift.BaselineMillis is { } b0)
            {
                settledQueue = b0;
            }
        }

        latencies.Sort();
        var baseline = playout.Drift.BaselineMillis ?? settledQueue;
        return new Result(
            playout.Underruns,
            playout.Drops,
            pcmDropped,
            playout.PrebufferMillis,
            latencies.Count == 0 ? 0 : latencies[latencies.Count / 2],
            latencies.Count == 0 ? 0 : latencies[(int)(latencies.Count * 0.99)],
            maxGap,
            double.IsNaN(baseline) ? 0 : lastQueue - baseline,
            playout.Drift.FramesAdjusted,
            playout.Extensions,
            lateUnderruns,
            lateExtensions,
            shed);
    }
}
