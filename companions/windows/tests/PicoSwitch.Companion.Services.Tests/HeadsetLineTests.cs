using PicoSwitch.Companion.Services.Presentation;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Play's Headset and Mic words, shared by the Windows and Linux pages so they
/// cannot drift (they were written twice before this projection existed).
/// </summary>
public sealed class HeadsetLineTests
{
    [Fact]
    public void TheHeadsetSaysWhatItNeedsAndWhatItIsDoing()
    {
        Assert.Equal("Headset · start Controller Link to use it",
            HeadsetLine.Headset(new CompanionAudioView(CompanionAudioPhase.Off, ControllerLink: false)));
        Assert.Equal("Headset off", HeadsetLine.Headset(new CompanionAudioView(CompanionAudioPhase.Off)));
        Assert.Equal("Headset starting...", HeadsetLine.Headset(new CompanionAudioView(CompanionAudioPhase.Starting)));
        Assert.Equal("Headset on · press any button so the console picks it up",
            HeadsetLine.Headset(new CompanionAudioView(CompanionAudioPhase.Playing, AwaitingConsole: true)));
        Assert.Equal("Headset on", HeadsetLine.Headset(new CompanionAudioView(CompanionAudioPhase.Playing)));
    }

    [Fact]
    public void TheMicSeparatesTheWishFromWhatTheAdapterTook()
    {
        var off = new CompanionAudioView(CompanionAudioPhase.Off);
        Assert.Equal("Mic off", HeadsetLine.Mic(off, wanted: false));
        Assert.Equal("Mic on with headset", HeadsetLine.Mic(off, wanted: true));
        Assert.Equal("Mic on", HeadsetLine.Mic(new CompanionAudioView(CompanionAudioPhase.Playing, Microphone: true), wanted: true));
        Assert.Equal("Mic unavailable", HeadsetLine.Mic(new CompanionAudioView(CompanionAudioPhase.Playing), wanted: true));
    }

    [Theory]
    [InlineData(CompanionAudioPhase.Unavailable, false)]
    [InlineData(CompanionAudioPhase.Starting, false)]
    [InlineData(CompanionAudioPhase.Off, true)]
    [InlineData(CompanionAudioPhase.Playing, true)]
    public void TheMicCanBeChosenBeforeAndDuringPlayback(CompanionAudioPhase phase, bool can) =>
        Assert.Equal(can, HeadsetLine.MicCanToggle(new CompanionAudioView(phase)));

    [Fact]
    public void ASessionNoticeOutranksTheMicrophoneNotice()
    {
        Assert.Null(HeadsetLine.Notice(new CompanionAudioView(CompanionAudioPhase.Playing)));
        Assert.Equal(new HeadsetNotice("speaker only", NoticeTone.Warning),
            HeadsetLine.Notice(new CompanionAudioView(CompanionAudioPhase.Playing, MicrophoneDetail: "speaker only")));
        Assert.Equal(new HeadsetNotice("ended", NoticeTone.Informational),
            HeadsetLine.Notice(new CompanionAudioView(CompanionAudioPhase.Ended, "ended", MicrophoneDetail: "speaker only")));
        Assert.Equal(new HeadsetNotice("busy", NoticeTone.Warning),
            HeadsetLine.Notice(new CompanionAudioView(CompanionAudioPhase.Refused, "busy")));
        Assert.Equal(new HeadsetNotice("broke", NoticeTone.Error),
            HeadsetLine.Notice(new CompanionAudioView(CompanionAudioPhase.Error, "broke")));

        // A microphone note is about a playing session only.
        Assert.Null(HeadsetLine.Notice(new CompanionAudioView(CompanionAudioPhase.Off, MicrophoneDetail: "speaker only")));
    }
}
