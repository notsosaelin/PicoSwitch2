using PicoSwitch.Companion.Services.Presentation;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The user-facing projection, tested without a radio.
///
/// The page paints this and decides nothing, so every sentence a user can see
/// is decided here — which makes this the right place to hold the line on what
/// the product is allowed to say.
/// </summary>
public sealed class ControllerLinkViewTests
{
    [Theory]
    [InlineData(ControllerLinkPhase.Unavailable, false, false, false)]
    [InlineData(ControllerLinkPhase.Ready, true, false, false)]
    [InlineData(ControllerLinkPhase.Stopped, true, false, false)]
    [InlineData(ControllerLinkPhase.Starting, false, true, true)]
    [InlineData(ControllerLinkPhase.Streaming, false, true, false)]
    [InlineData(ControllerLinkPhase.Stopping, false, false, true)]
    [InlineData(ControllerLinkPhase.Error, true, false, false)]
    public void EveryProductStateHasDeterministicActions(
        ControllerLinkPhase phase,
        bool canStart,
        bool canStop,
        bool busy)
    {
        var view = ControllerLinkView.Of(phase, managementReady: true);

        Assert.False(string.IsNullOrWhiteSpace(view.Headline));
        Assert.False(string.IsNullOrWhiteSpace(view.Explanation));
        Assert.Equal(canStart, view.CanStart);
        Assert.Equal(canStop, view.CanStop);
        Assert.Equal(busy, view.Busy);
    }

    [Fact]
    public void EveryPhaseIsCovered()
    {
        // A phase added without a case would fall through to Error and quietly
        // tell users something went wrong when nothing did.
        foreach (var phase in Enum.GetValues<ControllerLinkPhase>())
        {
            var view = ControllerLinkView.Of(phase, managementReady: true);
            if (phase != ControllerLinkPhase.Error)
            {
                Assert.Equal(phase, view.Phase);
            }
        }
    }

    [Fact]
    public void ProductCopyDoesNotExposeImplementationTerms()
    {
        // Diagnostics may say GATT, MTU and characteristic. The Gamepad page
        // may not: a user who opened it wants to know whether their PC is
        // driving the console.
        string[] forbidden =
        [
            "AppContainer", "AUMID", "HCI", "GATT", "MTU", "characteristic",
            "ATT", "data plane", "HOGP", "advertis",
        ];

        foreach (var phase in Enum.GetValues<ControllerLinkPhase>())
        {
            var view = ControllerLinkView.Of(phase, managementReady: true);
            var text = view.Headline + " " + view.Explanation;
            foreach (var term in forbidden)
            {
                Assert.DoesNotContain(term, text, StringComparison.OrdinalIgnoreCase);
            }
        }
    }

    [Fact]
    public void CannotStartWithoutTrustedManagement()
    {
        // Ready with management down would offer a button that cannot work.
        var view = ControllerLinkView.Of(ControllerLinkPhase.Ready, managementReady: false);
        Assert.False(view.CanStart);
    }

    [Fact]
    public void CarriesTheAdaptersOwnRefusalText()
    {
        // The service turns an adapter refusal into a sentence; the view must
        // pass it through rather than replacing it with something generic.
        var view = ControllerLinkView.Of(
            ControllerLinkPhase.Error,
            "This Bluetooth connection is too small to carry controller input.",
            managementReady: true);

        Assert.Contains("too small", view.Explanation);
        Assert.Equal(view.Explanation, view.Error);
    }

    [Fact]
    public void RetiredHogpStatesNoLongerExist()
    {
        // Path C never advertises, never pairs and is never dialled. Keeping
        // those states as aliases would let the UI say "waiting for the adapter
        // to pair" about something that cannot happen.
        var names = Enum.GetNames<ControllerLinkPhase>();
        Assert.DoesNotContain("Advertising", names);
        Assert.DoesNotContain("WaitingForConnection", names);
        Assert.DoesNotContain("Connecting", names);
        Assert.DoesNotContain("Reconnecting", names);
    }
}
