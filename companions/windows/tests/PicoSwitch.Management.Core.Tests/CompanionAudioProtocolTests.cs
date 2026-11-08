using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// `caudio status` decoding. The reply shape is the one
/// firmware/src/settings/config.c reply_caudio_status() emits.
/// </summary>
public sealed class CompanionAudioProtocolTests
{
    private const string Command = "caudio status";

    [Fact]
    public void DecodesTheFirmwareStatusReply()
    {
        const string reply = """
        {"caudio":{"supported":true,"active":true,"subscribed":true,"usb_speaker":true,
        "version":1,"codec":"opus","sample_rate":48000,"channels":2,"frame_ms":20,
        "kbps":64,"frame_bytes":164,"att_mtu":527,"min_att_mtu":167,"generation":3,"admit":"ok",
        "last_stop":"never_started","frames":{"sent":500,"busy":7,"failed":1,"rejected":0},
        "send_max_gap_us":41000}}
        """;

        var state = ManagementProtocol.CompanionAudio(Command, reply);

        Assert.True(state.Supported);
        Assert.True(state.Active);
        Assert.True(state.Subscribed);
        Assert.True(state.UsbSpeakerActive);
        Assert.Equal("opus", state.Codec);
        Assert.Equal(48_000, state.SampleRate);
        Assert.Equal(2, state.Channels);
        Assert.Equal(20, state.FrameMillis);
        Assert.Equal(64, state.Kbps);
        Assert.Equal(164, state.FrameBytes);
        Assert.Equal(527, state.AttMtu);
        Assert.Equal(167, state.MinimumAttMtu);
        Assert.Equal(3, state.Generation);
        Assert.Equal("ok", state.Admit);
        Assert.Equal(500, state.FramesSent);
        Assert.Equal(7, state.FramesBusy);
        Assert.Equal(1, state.FramesFailed);
        Assert.Equal(41_000, state.SendMaxGapMicros);
    }

    [Fact]
    public void ARefusalCarriesItsReason()
    {
        var state = ManagementProtocol.CompanionAudio(
            "caudio start",
            """{"caudio":{"supported":false,"active":false,"admit":"unsupported"}}""");
        Assert.False(state.Active);
        Assert.Equal("unsupported", state.Admit);
    }

    [Fact]
    public void AFirmwareWithoutTheRateFieldStillDecodes()
    {
        // The first prototype build (96 kbit/s fixed) did not report kbps.
        var state = ManagementProtocol.CompanionAudio(
            Command,
            """{"caudio":{"supported":true,"active":true,"frame_bytes":244}}""");
        Assert.Equal(0, state.Kbps);
        Assert.Equal(244, state.FrameBytes);
    }

    [Fact]
    public void DecodesTheMicrophoneFields()
    {
        var state = ManagementProtocol.CompanionAudio(
            "caudio start mic",
            """{"caudio":{"supported":true,"active":true,"mic":true,"mic_supported":true,"admit":"ok"}}""");
        Assert.True(state.Mic);
        Assert.True(state.MicSupported);

        var old = ManagementProtocol.CompanionAudio(Command, """{"caudio":{"supported":true,"active":true}}""");
        Assert.False(old.Mic);
        Assert.False(old.MicSupported);
    }

    [Fact]
    public void RejectsAReplyThatIsNotACompanionAudioStatus() =>
        Assert.ThrowsAny<Exception>(() =>
            ManagementProtocol.CompanionAudio(Command, """{"caudio":"status"}"""));

    [Fact]
    public void CommandsMatchTheFirmwareVocabulary()
    {
        Assert.Equal("caudio start", ManagementCommands.CompanionAudioStart);
        Assert.Equal("caudio start mic", ManagementCommands.CompanionAudioStartMic);
        Assert.Equal("caudio stop", ManagementCommands.CompanionAudioStop);
        Assert.Equal("caudio status", ManagementCommands.CompanionAudioStatus);
    }
}
