using PicoSwitch.Companion.Services.Presentation;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Play's controller headline and headset strip (companion-shell-layout.md,
/// "Play"): Android's HeroCard and HeadsetTile words, for the desktops.
/// </summary>
public sealed class PlayHomeWordsTests
{
    [Fact]
    public void TheHeadlineIsOnePhrasePerState()
    {
        Assert.Equal("On the console", PlayHeadline.Of(ControllerLinkView.Of(ControllerLinkPhase.Streaming)));
        Assert.Equal("Connecting…", PlayHeadline.Of(ControllerLinkView.Of(ControllerLinkPhase.Starting)));
        Assert.Equal("Stopping…", PlayHeadline.Of(ControllerLinkView.Of(ControllerLinkPhase.Stopping)));
        Assert.Equal("Ready to play", PlayHeadline.Of(ControllerLinkView.Of(ControllerLinkPhase.Ready, managementReady: true)));
        Assert.Equal("Not ready", PlayHeadline.Of(ControllerLinkView.Of(ControllerLinkPhase.Unavailable)));
    }

    [Fact]
    public void TheLineUnderItSpeaksOnlyWhenSomethingBlocksStarting()
    {
        Assert.Equal(string.Empty, PlayHeadline.Blocker(ControllerLinkView.Of(ControllerLinkPhase.Ready, managementReady: true)));
        Assert.Equal(string.Empty, PlayHeadline.Blocker(ControllerLinkView.Of(ControllerLinkPhase.Streaming)));
        Assert.Equal(string.Empty, PlayHeadline.Blocker(ControllerLinkView.Of(ControllerLinkPhase.Starting)));
        Assert.Equal(
            "Connect to a trusted PicoSwitch adapter first.",
            PlayHeadline.Blocker(ControllerLinkView.Of(ControllerLinkPhase.Unavailable)));
    }

    [Fact]
    public void TheHeadsetIsLiveOnlyWhilePlayingAndNeedsControllerLinkToPlugIn()
    {
        var playing = HeadsetStrip.Of(new CompanionAudioView(CompanionAudioPhase.Playing, Microphone: true));
        Assert.True(playing.Live);
        Assert.True(playing.Enabled);
        Assert.Equal(string.Empty, playing.Line);

        var speakerOnly = HeadsetStrip.Of(new CompanionAudioView(CompanionAudioPhase.Playing, MicrophoneDetail: "Microphone busy"));
        Assert.Equal("Headphones only · Microphone busy", speakerOnly.Line);

        var waiting = HeadsetStrip.Of(new CompanionAudioView(CompanionAudioPhase.Playing, AwaitingConsole: true));
        Assert.Equal("Press any button so the console picks it up", waiting.Line);

        var noLink = HeadsetStrip.Of(new CompanionAudioView(CompanionAudioPhase.Off, ControllerLink: false));
        Assert.False(noLink.Live);
        Assert.False(noLink.Enabled);
        Assert.Equal("Start playing to plug it in", noLink.Line);

        var starting = HeadsetStrip.Of(new CompanionAudioView(CompanionAudioPhase.Starting));
        Assert.True(starting.Busy);
        Assert.False(starting.Enabled);
    }
}
