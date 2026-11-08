using PicoSwitch.Companion.Services.Presentation;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The status words every platform's top bar and Play cards show. One table,
/// so Windows, Linux and Android cannot drift into different wording again
/// (Linux had "Looking for adapter…" and "Repair pairing" where Windows said
/// "Looking…" and "Needs repair").
/// </summary>
public sealed class PlayStatusTests
{
    [Theory]
    [InlineData(AdapterRelationshipPhase.NoRelationship, StatusTone.Idle, "Not paired", "Pair adapter")]
    [InlineData(AdapterRelationshipPhase.Discovering, StatusTone.Busy, "Looking…", "Connecting…")]
    [InlineData(AdapterRelationshipPhase.Pairing, StatusTone.Busy, "Pairing…", "Connecting…")]
    [InlineData(AdapterRelationshipPhase.Connecting, StatusTone.Busy, "Connecting…", "Connecting…")]
    [InlineData(AdapterRelationshipPhase.Validating, StatusTone.Busy, "Verifying…", "Connecting…")]
    [InlineData(AdapterRelationshipPhase.Failed, StatusTone.Bad, "Could not connect", "Connect")]
    [InlineData(AdapterRelationshipPhase.RepairRequired, StatusTone.Bad, "Needs repair", "Repair")]
    public void EveryDisconnectedPhaseHasOneToneOneWordingAndOneNextStep(
        AdapterRelationshipPhase phase, StatusTone tone, string words, string next)
    {
        Assert.Equal((tone, words), AdapterStatus.Of(connected: false, phase));
        Assert.Equal(next, AdapterStatus.NextStep(connected: false, phase));
    }

    [Fact]
    public void ConnectedWinsOverAnyPhase()
    {
        foreach (var phase in Enum.GetValues<AdapterRelationshipPhase>())
        {
            Assert.Equal((StatusTone.Good, "Connected"), AdapterStatus.Of(connected: true, phase));
            Assert.Equal("Manage", AdapterStatus.NextStep(connected: true, phase));
            Assert.False(AdapterStatus.NextStepConnects(connected: true, phase, hasRemembered: true));
        }
    }

    [Fact]
    public void OnlyAPlainConnectConnectsInPlace()
    {
        // Pairing and repairing need the Adapter page's flow; nothing to
        // connect to means the button must go there too.
        Assert.True(AdapterStatus.NextStepConnects(false, AdapterRelationshipPhase.Failed, hasRemembered: true));
        Assert.False(AdapterStatus.NextStepConnects(false, AdapterRelationshipPhase.Failed, hasRemembered: false));
        Assert.False(AdapterStatus.NextStepConnects(false, AdapterRelationshipPhase.NoRelationship, hasRemembered: true));
        Assert.False(AdapterStatus.NextStepConnects(false, AdapterRelationshipPhase.RepairRequired, hasRemembered: true));
    }

    [Fact]
    public void TheControllerCardSaysWhatToDoNext()
    {
        Assert.Equal(("Pad", StatusTone.Good, "Ready"), ControllerCardStatus.Of("Pad", true, 1));
        Assert.Equal(("Pad", StatusTone.Busy, "Can't be read"), ControllerCardStatus.Of("Pad", false, 1));
        Assert.Equal(("No controller", StatusTone.Idle, "Plug one in"), ControllerCardStatus.Of(null, false, 0));
        Assert.Equal(("No controller chosen", StatusTone.Idle, "Choose one below"), ControllerCardStatus.Of(null, false, 2));
    }

    [Fact]
    public void ThePlayLineIsSilentWhenStartAlreadySaysItAll()
    {
        var ready = ControllerLinkView.Of(ControllerLinkPhase.Ready, managementReady: true);
        Assert.Equal(string.Empty, PlayLine.Of(ready));

        var streaming = ControllerLinkView.Of(ControllerLinkPhase.Streaming, managementReady: true);
        Assert.Equal(streaming.Headline, PlayLine.Of(streaming));

        var blocked = ControllerLinkView.Of(ControllerLinkPhase.Unavailable, managementReady: false);
        Assert.Equal(blocked.Explanation, PlayLine.Of(blocked));
    }
}
