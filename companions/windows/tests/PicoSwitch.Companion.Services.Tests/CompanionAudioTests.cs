using Concentus;
using Concentus.Enums;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

public sealed class CompanionAudioTests
{
    // ---------------------------------------------------------------- decoder

    /// <summary>
    /// The genuine console's fixed idle packet, captured from a real Switch 2
    /// (docs/switch2/pro2-headset-audio.md): FC FF FE then 237 zero bytes. A
    /// real-world vector, not one this test invented.
    /// </summary>
    private static byte[] ConsoleIdlePacket()
    {
        var packet = new byte[CompanionAudioDataPlane.MaxOpusBytes];
        packet[0] = 0xFC;
        packet[1] = 0xFF;
        packet[2] = 0xFE;
        return packet;
    }

    [Fact]
    public void StandardDecoderAcceptsTheConsolesIdlePacket()
    {
        var decoder = new ConcentusCompanionAudioDecoder();
        var pcm = new short[CompanionAudioDataPlane.SamplesPerChannel * CompanionAudioDataPlane.Channels];
        Assert.Equal(CompanionAudioDataPlane.SamplesPerChannel, decoder.Decode(ConsoleIdlePacket(), pcm));
    }

    [Theory]
    [InlineData(96_000, 240)]   // the validated Pro Controller 2 operating point
    [InlineData(64_000, 160)]   // receiver is size-agnostic; the adapter offers only 96
    [InlineData(48_000, 120)]
    public void FirmwareShapedPacketsDecodeToTheSignalThatWasEncoded(int bitrate, int packetBytes)
    {
        // Encode with the firmware's configuration: CELT-only, 48 kHz stereo,
        // 20 ms, CBR -- packets of bitrate/400 bytes with TOC 0xFC.
        var encoder = OpusCodecFactory.CreateEncoder(
            48_000, 2, OpusApplication.OPUS_APPLICATION_RESTRICTED_LOWDELAY);
        encoder.Bitrate = bitrate;
        encoder.UseVBR = false;
        var frames = new List<byte[]>();
        var input = new short[960 * 2];
        for (var f = 0; f < 10; f++)
        {
            for (var i = 0; i < 960; i++)
            {
                var t = (f * 960 + i) / 48_000.0;
                var s = (short)(Math.Sin(2 * Math.PI * 1000 * t) * 12_000);
                input[2 * i] = s;
                input[2 * i + 1] = s;
            }

            var packet = new byte[1275];
            var n = encoder.Encode(input, 960, packet, packet.Length);
            Assert.Equal(packetBytes, n);
            Assert.Equal(0xFC, packet[0]);
            frames.Add(packet[..n]);
        }

        long energy = 0;
        var blocks = 0;
        var receiver = new CompanionAudioReceiver(
            new ConcentusCompanionAudioDecoder(),
            pcm =>
            {
                blocks++;
                foreach (var v in pcm)
                {
                    energy += (long)v * v;
                }
            },
            CompanionAudioDataPlane.HeaderBytes + packetBytes);
        for (ushort seq = 0; seq < frames.Count; seq++)
        {
            receiver.Accept(Frame(seq, frames[seq]));
        }

        Assert.Equal(10, blocks);
        Assert.Equal(10, receiver.FramesDecoded);
        Assert.Equal(0, receiver.DecodeErrors);
        // A 1 kHz tone at ~-8.7 dBFS, not silence.
        Assert.True(energy / (blocks * 1920.0) > 1_000_000);
    }

    // --------------------------------------------------------------- receiver

    [Fact]
    public void ConcealsAGapButNeverMoreThanTheCap()
    {
        var decoder = new CountingDecoder();
        var blocks = 0;
        var receiver = new CompanionAudioReceiver(decoder, _ => blocks++, IdleFrameBytes);
        var packet = ConsoleIdlePacket();

        receiver.Accept(Frame(1, packet));
        receiver.Accept(Frame(3, packet));    // one lost
        receiver.Accept(Frame(100, packet));  // 96 lost: conceal only the cap

        Assert.Equal(3, receiver.FramesDecoded);
        Assert.Equal(97, receiver.FramesLost);
        Assert.Equal(1 + CompanionAudioReceiver.MaxConcealedFrames, receiver.FramesConcealed);
        Assert.Equal(3 + 1 + CompanionAudioReceiver.MaxConcealedFrames, blocks);
        Assert.Equal(1 + CompanionAudioReceiver.MaxConcealedFrames, decoder.Concealments);
    }

    [Fact]
    public void DropsDuplicatesAndRewindsAndMalformedNotifications()
    {
        var blocks = 0;
        var receiver = new CompanionAudioReceiver(new CountingDecoder(), _ => blocks++, IdleFrameBytes);
        var packet = ConsoleIdlePacket();

        receiver.Accept(Frame(5, packet));
        receiver.Accept(Frame(5, packet));
        receiver.Accept(Frame(4, packet));
        receiver.Accept(new byte[] { 1, 0x90, 0, 0 });

        Assert.Equal(1, blocks);
        Assert.Equal(2, receiver.FramesStale);
        Assert.Equal(1, receiver.FramesMalformed);
    }

    [Fact]
    public void WrapIsNotALoss()
    {
        var receiver = new CompanionAudioReceiver(new CountingDecoder(), _ => { }, IdleFrameBytes);
        receiver.Accept(Frame(0xFFFF, ConsoleIdlePacket()));
        receiver.Accept(Frame(0, ConsoleIdlePacket()));
        Assert.Equal(0, receiver.FramesLost);
        Assert.Equal(2, receiver.FramesDecoded);
    }

    // ---------------------------------------------------------------- service

    [Fact]
    public async Task PlaysWhenTheAdapterAcceptsAndTellsItToStop()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        var sink = new FakeSink();
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(sink), new DiagnosticLog(), () => new CountingDecoder());

        await service.StartAsync();

        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);
        Assert.True(sink.Started);
        management.Downlink!.Deliver(Frame(1, ConsoleIdlePacket()));
        Assert.Equal(1, sink.Blocks);

        await service.StopAsync();

        Assert.Equal(CompanionAudioPhase.Off, service.View.Value.Phase);
        Assert.Equal(1, management.Stops);
        Assert.True(management.Downlink.Disposed);
        Assert.True(sink.Stopped);
    }

    [Fact]
    public async Task ShowsTheAdaptersRefusalReasonAndOpensNothing()
    {
        var management = new FakeManagement
        {
            StartReply = new CompanionAudioState(Supported: false, Active: false, Admit: "unsupported"),
        };
        var factory = new FakeSinkFactory(new FakeSink());
        await using var service = new CompanionAudioService(
            management, factory, new DiagnosticLog(), () => new CountingDecoder());

        await service.StartAsync();

        Assert.Equal(CompanionAudioPhase.Refused, service.View.Value.Phase);
        Assert.Contains("Pico 2 W", service.View.Value.Detail);
        Assert.Null(management.Downlink);
        Assert.Equal(0, factory.Created);
    }

    [Fact]
    public void ARefusedOrSupersededSessionSaysTheControllersHeadsetHasTheAudio()
    {
        Assert.Contains(
            "plugged into your controller",
            CompanionAudioService.RefusalText(new CompanionAudioState(Supported: true, Admit: "controller_headset")));
        Assert.Contains(
            "plugged into your controller",
            CompanionAudioService.EndedText("preempted_controller_headset"));
        Assert.Contains("plugged into your controller", CompanionAudioService.EndedText("preempted_pro2"));
    }

    [Fact]
    public async Task PlaysTheSessionsReportedFrameSize()
    {
        // The receiver is sized from the report: in a 164-byte session a
        // 244-byte frame is malformed. (The adapter itself offers only 96 kbit/s.)
        var management = new FakeManagement { StartReply = Accepted() with { FrameBytes = 164, Kbps = 64 } };
        var sink = new FakeSink();
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(sink), new DiagnosticLog(), () => new CountingDecoder(),
            startWatchdogTimer: false);

        await service.StartAsync();
        management.Downlink!.Deliver(Frame(1, new byte[160]));
        management.Downlink.Deliver(Frame(2, ConsoleIdlePacket()));

        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);
        Assert.Equal(1, sink.Blocks);
        Assert.Equal(1, service.Metrics.FramesMalformed);
    }

    [Fact]
    public async Task SilenceAloneNeverEndsAnActiveSession()
    {
        long now = 0;
        var management = new FakeManagement
        {
            StartReply = Accepted(),
            StatusReply = Accepted(),
        };
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(new FakeSink()), new DiagnosticLog(), () => new CountingDecoder(),
            clockMillis: () => now, startWatchdogTimer: false);
        await service.StartAsync();

        // Under the threshold the adapter is not even asked.
        now = CompanionAudioService.SilenceBeforeCheckMillis - 1;
        await service.CheckAdapterSessionAsync();
        Assert.Equal(0, management.StatusQueries);

        // A quiet console: asked once, still active, still playing.
        now = CompanionAudioService.SilenceBeforeCheckMillis;
        await service.CheckAdapterSessionAsync();
        Assert.Equal(1, management.StatusQueries);
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);

        // And not asked again until another full silence window has passed.
        now += 500;
        await service.CheckAdapterSessionAsync();
        Assert.Equal(1, management.StatusQueries);
    }

    [Fact]
    public async Task AnAdapterEndedSessionIsEndedLocallyWithItsReason()
    {
        long now = 0;
        var management = new FakeManagement
        {
            StartReply = Accepted(),
            StatusReply = Accepted() with { Active = false, LastStop = "preempted_controller_headset" },
        };
        var sink = new FakeSink();
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(sink), new DiagnosticLog(), () => new CountingDecoder(),
            clockMillis: () => now, startWatchdogTimer: false);
        await service.StartAsync();

        now = CompanionAudioService.SilenceBeforeCheckMillis;
        await service.CheckAdapterSessionAsync();

        Assert.Equal(CompanionAudioPhase.Ended, service.View.Value.Phase);
        Assert.Contains("plugged into your controller", service.View.Value.Detail);
        Assert.False(service.View.Value.On);
        Assert.True(service.View.Value.CanToggle);
        Assert.True(sink.Stopped);
        Assert.True(management.Downlink!.Disposed);
        // The adapter already stopped; nothing is sent back.
        Assert.Equal(0, management.Stops);
    }

    [Fact]
    public async Task RefusesAnUnknownFrameGeometryAndReleasesTheAdapter()
    {
        var management = new FakeManagement { StartReply = Accepted() with { FrameBytes = 300 } };
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(new FakeSink()), new DiagnosticLog(), () => new CountingDecoder());

        await service.StartAsync();

        Assert.Equal(CompanionAudioPhase.Error, service.View.Value.Phase);
        // The adapter is still advertising a headphone jack: it must be told.
        Assert.Equal(1, management.Stops);
    }

    [Fact]
    public async Task ManagementLossTearsDownLocallyWithoutCallingTheAdapter()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        var sink = new FakeSink();
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(sink), new DiagnosticLog(), () => new CountingDecoder());
        await service.StartAsync();

        management.SetReady(false);
        await WaitFor(() => service.View.Value.Phase == CompanionAudioPhase.Unavailable);

        Assert.True(management.Downlink!.Disposed);
        Assert.True(sink.Stopped);
        Assert.Equal(0, management.Stops);
    }

    // ------------------------------------------------- Controller Link gate

    [Fact]
    public async Task TheHeadsetCannotBeTurnedOnWithoutControllerLink()
    {
        var link = new StateValue<bool>(false);
        var management = new FakeManagement { StartReply = Accepted() };
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(new FakeSink()), new DiagnosticLog(), () => new CountingDecoder(),
            startWatchdogTimer: false, controllerLink: link);

        Assert.Equal(CompanionAudioPhase.Off, service.View.Value.Phase);
        Assert.False(service.View.Value.ControllerLink);
        Assert.False(service.View.Value.CanToggle);

        // Even if asked, nothing is sent to the adapter.
        await service.StartAsync();
        Assert.Equal(CompanionAudioPhase.Off, service.View.Value.Phase);
        Assert.Null(management.Downlink);

        link.Set(true);
        Assert.True(service.View.Value.CanToggle);
        await service.StartAsync();
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);
    }

    [Fact]
    public async Task StoppingControllerLinkEndsTheHeadsetAndReturnsAudioToTheConsole()
    {
        var link = new StateValue<bool>(true);
        var management = new FakeManagement { StartReply = Accepted() };
        var sink = new FakeSink();
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(sink), new DiagnosticLog(), () => new CountingDecoder(),
            startWatchdogTimer: false, controllerLink: link);
        await service.StartAsync();

        link.Set(false);
        await WaitFor(() => service.View.Value.Phase == CompanionAudioPhase.Ended);

        Assert.Contains("Controller Link stopped", service.View.Value.Detail);
        Assert.False(service.View.Value.CanToggle);
        Assert.True(sink.Stopped);
        Assert.True(management.Downlink!.Disposed);
        // The jack is withdrawn now, not on the adapter's next tick.
        Assert.Equal(1, management.Stops);

        // Turning off is never blocked by the gate.
        link.Set(true);
        Assert.True(service.View.Value.CanToggle);
    }

    [Fact]
    public async Task UntilTheConsoleOpensItsStreamThePlayerIsToldToPressAButton()
    {
        var management = new FakeManagement { StartReply = Accepted() with { UsbSpeakerActive = false } };
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(new FakeSink()), new DiagnosticLog(), () => new CountingDecoder(),
            startWatchdogTimer: false);

        await service.StartAsync();
        Assert.True(service.View.Value.AwaitingConsole);

        management.Downlink!.Deliver(Frame(1, ConsoleIdlePacket()));
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);
        Assert.False(service.View.Value.AwaitingConsole);
    }

    [Fact]
    public async Task AStreamThatIsAlreadyOpenIsNotAwaited()
    {
        var management = new FakeManagement { StartReply = Accepted() with { UsbSpeakerActive = true } };
        await using var service = new CompanionAudioService(
            management, new FakeSinkFactory(new FakeSink()), new DiagnosticLog(), () => new CountingDecoder(),
            startWatchdogTimer: false);

        await service.StartAsync();
        Assert.False(service.View.Value.AwaitingConsole);
    }

    [Theory]
    [InlineData("no_controller_link", "Start Controller Link")]
    [InlineData("other_controller", "Another controller")]
    public void ControllerLinkRefusalsNameTheFix(string admit, string expected)
    {
        Assert.Contains(expected, CompanionAudioService.RefusalText(new CompanionAudioState(Supported: true, Admit: admit)));
    }

    [Theory]
    [InlineData("controller_link_ended", "Controller Link stopped")]
    [InlineData("source_changed", "Another controller took over")]
    public void ControllerLinkEndsSayWhereTheAudioWent(string lastStop, string expected)
    {
        Assert.Contains(expected, CompanionAudioService.EndedText(lastStop));
    }

    // ------------------------------------------------------------- microphone

    private static CompanionAudioService MicService(
        FakeManagement management, FakeMicCapture? capture) =>
        new(management, new FakeSinkFactory(new FakeSink()), new DiagnosticLog(), () => new CountingDecoder(),
            startWatchdogTimer: false,
            micCaptures: new FakeMicCaptureFactory(capture),
            micEncoders: () => new FixedMicEncoder());

    [Fact]
    public async Task WithTheMicrophoneOnTheSessionStartsAsAHeadsetAndStreamsUp()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        var capture = new FakeMicCapture();
        await using var service = MicService(management, capture);

        await service.SetMicrophoneAsync(true);
        await service.StartAsync();

        Assert.Equal([true], management.StartRequests);
        Assert.True(service.View.Value.Microphone);
        Assert.Null(service.View.Value.MicrophoneDetail);
        capture.Deliver();
        capture.Deliver();
        var sent = management.MicUplink!.Sent;
        Assert.Equal(2, sent.Count);
        Assert.Equal(CompanionMicDataPlane.FrameBytes, sent[0].Length);
        Assert.Equal(CompanionMicDataPlane.OpcodeOpus, sent[0][1]);
        Assert.Equal(1, sent[1][2]);

        await service.StopAsync();
        Assert.True(capture.Stopped);
        Assert.True(management.MicUplink.Disposed);
        Assert.False(service.View.Value.Microphone);
    }

    [Fact]
    public async Task WithoutTheMicrophoneNothingIsCapturedOrRequested()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        var capture = new FakeMicCapture();
        await using var service = MicService(management, capture);

        await service.StartAsync();

        Assert.Equal([false], management.StartRequests);
        Assert.Null(management.MicUplink);
        Assert.Null(capture.OnBlock);
        Assert.False(service.View.Value.Microphone);
    }

    [Fact]
    public async Task AnAdapterThatCannotDecodeTheMicrophoneStillPlaysTheSpeaker()
    {
        var management = new FakeManagement
        {
            StartReply = Accepted(),
            StartMicReply = new CompanionAudioState(Supported: true, Active: false, Admit: "mic_unsupported"),
        };
        await using var service = MicService(management, new FakeMicCapture());

        await service.SetMicrophoneAsync(true);
        await service.StartAsync();

        Assert.Equal([true, false], management.StartRequests);
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);
        Assert.False(service.View.Value.Microphone);
        Assert.Contains("cannot take a microphone", service.View.Value.MicrophoneDetail);
    }

    [Fact]
    public async Task NoMicrophoneDeviceReplugsAsHeadphonesAndKeepsPlaying()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        await using var service = MicService(management, capture: null);

        await service.SetMicrophoneAsync(true);
        await service.StartAsync();

        // Started as a headset, then re-plugged as headphones: the console must
        // not wait on a microphone that will never speak.
        Assert.Equal([true, false], management.StartRequests);
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);
        Assert.False(service.View.Value.Microphone);
        Assert.Contains("No microphone", service.View.Value.MicrophoneDetail);
        Assert.True(management.MicUplink!.Disposed);
    }

    [Fact]
    public async Task FirmwareWithoutTheMicCharacteristicReplugsAsHeadphones()
    {
        var management = new FakeManagement { StartReply = Accepted(), MicUplinkOpens = false };
        var capture = new FakeMicCapture();
        await using var service = MicService(management, capture);

        await service.SetMicrophoneAsync(true);
        await service.StartAsync();

        Assert.Equal([true, false], management.StartRequests);
        Assert.Null(capture.OnBlock);
        Assert.Contains("does not accept a microphone", service.View.Value.MicrophoneDetail);
    }

    [Fact]
    public async Task TogglingTheMicrophoneWhilePlayingReplugsWithoutEndingTheSession()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        var capture = new FakeMicCapture();
        await using var service = MicService(management, capture);
        await service.StartAsync();
        var downlink = management.Downlink!;

        await service.SetMicrophoneAsync(true);
        Assert.True(service.View.Value.Microphone);
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);

        await service.SetMicrophoneAsync(false);
        Assert.False(service.View.Value.Microphone);
        Assert.True(capture.Stopped);
        Assert.Equal(CompanionAudioPhase.Playing, service.View.Value.Phase);

        Assert.Equal([false, true, false], management.StartRequests);
        Assert.Same(downlink, management.Downlink);
        Assert.False(downlink.Disposed);
        Assert.Equal(0, management.Stops);
    }

    [Fact]
    public async Task ARunningSessionThatRefusesTheMicrophoneStaysHeadphones()
    {
        // The firmware answers a refused re-plug with the live session's status:
        // still active, mic false, admit mic_unsupported.
        var management = new FakeManagement
        {
            StartReply = Accepted(),
            StartMicReply = Accepted() with { Admit = "mic_unsupported" },
        };
        var capture = new FakeMicCapture();
        await using var service = MicService(management, capture);
        await service.StartAsync();

        await service.SetMicrophoneAsync(true);

        Assert.False(service.View.Value.Microphone);
        Assert.Null(capture.OnBlock);
        Assert.Null(management.MicUplink);
        Assert.Contains("cannot take a microphone", service.View.Value.MicrophoneDetail);
    }

    [Fact]
    public async Task TheMicrophoneSettingWhileOffAppliesToTheNextStartOnly()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        await using var service = MicService(management, new FakeMicCapture());

        await service.SetMicrophoneAsync(true);

        Assert.Empty(management.StartRequests);
        Assert.True(service.MicrophoneEnabled);
        Assert.Equal(CompanionAudioPhase.Off, service.View.Value.Phase);
    }

    [Fact]
    public async Task ManagementLossClosesTheMicrophone()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        var capture = new FakeMicCapture();
        await using var service = MicService(management, capture);
        await service.SetMicrophoneAsync(true);
        await service.StartAsync();

        management.SetReady(false);
        await WaitFor(() => service.View.Value.Phase is CompanionAudioPhase.Unavailable);

        Assert.True(capture.Stopped);
        Assert.True(management.MicUplink!.Disposed);
        Assert.False(service.View.Value.Microphone);
    }

    [Fact]
    public async Task TheFirstConsoleFrameKeepsTheMicrophoneState()
    {
        var management = new FakeManagement { StartReply = Accepted() };
        await using var service = MicService(management, new FakeMicCapture());
        await service.SetMicrophoneAsync(true);
        await service.StartAsync();
        Assert.True(service.View.Value.AwaitingConsole);

        management.Downlink!.Deliver(Frame(1, ConsoleIdlePacket()));

        Assert.False(service.View.Value.AwaitingConsole);
        Assert.True(service.View.Value.Microphone);
    }

    // ---------------------------------------------------------------- helpers

    /// <summary>The console idle packet is 240 bytes: a 96 kbit/s session.</summary>
    private const int IdleFrameBytes = CompanionAudioDataPlane.MaxFrameBytes;

    private static CompanionAudioState Accepted() => new(
        Supported: true, Active: true, Version: 1, Codec: "opus", SampleRate: 48_000,
        Channels: 2, FrameMillis: 20, FrameBytes: 244, AttMtu: 527, MinimumAttMtu: 247, Admit: "ok",
        Kbps: 96);

    private static byte[] Frame(ushort sequence, byte[] opus)
    {
        var frame = new byte[CompanionAudioDataPlane.HeaderBytes + opus.Length];
        frame[0] = CompanionAudioDataPlane.Version;
        frame[1] = CompanionAudioDataPlane.OpcodeOpus;
        frame[2] = (byte)(sequence & 0xFF);
        frame[3] = (byte)(sequence >> 8);
        opus.CopyTo(frame, CompanionAudioDataPlane.HeaderBytes);
        return frame;
    }

    private static async Task WaitFor(Func<bool> condition)
    {
        for (var i = 0; i < 200 && !condition(); i++)
        {
            await Task.Delay(10);
        }

        Assert.True(condition());
    }

    private sealed class CountingDecoder : ICompanionAudioDecoder
    {
        public int Concealments { get; private set; }

        public int Decode(byte[]? opus, short[] pcm)
        {
            if (opus is null)
            {
                Concealments++;
            }

            return CompanionAudioDataPlane.SamplesPerChannel;
        }

        public void Reset()
        {
        }
    }

    private sealed class FakeManagement : ICompanionAudioManagement
    {
        private bool ready = true;

        public CompanionAudioState StartReply { get; init; } = new();

        public CompanionAudioState StatusReply { get; init; } = new();

        public int Stops { get; private set; }

        public int StatusQueries { get; private set; }

        public FakeDownlink? Downlink { get; private set; }

        public bool Ready => ready;

        public string? UnavailableReason => ready ? null : "not connected";

        public event Action? Changed;

        public void SetReady(bool value)
        {
            ready = value;
            Changed?.Invoke();
        }

        /// <summary>The reply to <c>caudio start mic</c>; defaults to StartReply with Mic set.</summary>
        public CompanionAudioState? StartMicReply { get; init; }

        /// <summary>Every start request, true for <c>caudio start mic</c>.</summary>
        public List<bool> StartRequests { get; } = [];

        public FakeMicUplink? MicUplink { get; private set; }

        public bool MicUplinkOpens { get; init; } = true;

        public Task<CompanionAudioState> StartAsync(bool microphone, CancellationToken cancellationToken = default)
        {
            StartRequests.Add(microphone);
            return Task.FromResult(microphone ? StartMicReply ?? StartReply with { Mic = true } : StartReply);
        }

        public ICompanionMicUplink? TryCreateMicUplink() => MicUplink = new FakeMicUplink(MicUplinkOpens);

        public Task<CompanionAudioState> StopAsync(CancellationToken cancellationToken = default)
        {
            Stops++;
            return Task.FromResult(new CompanionAudioState(Supported: true));
        }

        public Task<CompanionAudioState> StatusAsync(CancellationToken cancellationToken = default)
        {
            StatusQueries++;
            return Task.FromResult(StatusReply);
        }

        public ICompanionAudioDownlink? TryCreateDownlink() => Downlink = new FakeDownlink();
    }

    private sealed class FakeMicUplink(bool opens) : ICompanionMicUplink
    {
        public List<byte[]> Sent { get; } = [];

        public bool Disposed { get; private set; }

        public CompanionMicWriter.Counters Stats => new(Sent.Count, Sent.Count, 0, 0);

        public Task<bool> OpenAsync(CancellationToken cancellationToken = default) => Task.FromResult(opens);

        public void Send(byte[] frame) => Sent.Add(frame);

        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FakeMicCapture : ICompanionMicCapture
    {
        public Action<short[]>? OnBlock { get; private set; }

        public bool Stopped { get; private set; }

        public long BlocksCaptured { get; private set; }

        public void Start(Action<short[]> onBlock) => OnBlock = onBlock;

        public void Deliver()
        {
            BlocksCaptured++;
            OnBlock?.Invoke(new short[CompanionMicDataPlane.SamplesPerFrame]);
        }

        public void Stop() => Stopped = true;

        public void Dispose()
        {
        }
    }

    private sealed class FakeMicCaptureFactory(FakeMicCapture? capture) : ICompanionMicCaptureFactory
    {
        public ICompanionMicCapture? CreateCapture() => capture;
    }

    private sealed class FixedMicEncoder : ICompanionMicEncoder
    {
        public byte[]? Encode(short[] pcm)
        {
            var packet = new byte[CompanionMicDataPlane.OpusBytes];
            packet[0] = CompanionMicDataPlane.Toc;
            return packet;
        }
    }

    private sealed class FakeDownlink : ICompanionAudioDownlink
    {
        public bool Disposed { get; private set; }

        public event Action<byte[]>? FrameReceived;

        public int AttMtu => 527;

        public long NotificationsReceived { get; private set; }

        public void Deliver(byte[] frame)
        {
            NotificationsReceived++;
            FrameReceived?.Invoke(frame);
        }

        public Task<bool> OpenAsync(CancellationToken cancellationToken = default) => Task.FromResult(true);

        public ValueTask DisposeAsync()
        {
            Disposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FakeSink : ICompanionAudioSink
    {
        public bool Started { get; private set; }

        public bool Stopped { get; private set; }

        public int Blocks { get; private set; }

        public int QueuedMillis => 0;

        public long Underruns => 0;

        public int PrebufferMillis => 60;

        public void Start(int sampleRate, int channels) => Started = true;

        public bool Write(ReadOnlySpan<short> interleaved)
        {
            Blocks++;
            return true;
        }

        public void Stop() => Stopped = true;

        public void Dispose()
        {
        }
    }

    private sealed class FakeSinkFactory(FakeSink sink) : ICompanionAudioSinkFactory
    {
        public int Created { get; private set; }

        public ICompanionAudioSink? CreateSink()
        {
            Created++;
            return sink;
        }
    }
}
