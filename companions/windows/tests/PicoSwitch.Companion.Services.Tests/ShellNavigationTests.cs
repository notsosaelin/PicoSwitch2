using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The desktop shell's rules (companion-shell-layout.md, "Navigation"), the
/// same as Android's ShellPresentationTest / BackKeyGestureTest /
/// GamepadNavigationTest.
/// </summary>
public sealed class ShellNavigationTests
{
    [Fact]
    public void AutomaticIsTheSideWhenWideAndTheBottomWhenTall()
    {
        Assert.Equal(NavPlacement.Left, NavPlacements.Resolve(NavPlacement.Auto, landscape: true));
        Assert.Equal(NavPlacement.Bottom, NavPlacements.Resolve(NavPlacement.Auto, landscape: false));
        Assert.Equal(NavPlacement.Top, NavPlacements.Resolve(NavPlacement.Top, landscape: true));
    }

    [Fact]
    public void PlacementKeysRoundTripAndAnythingElseIsAutomatic()
    {
        foreach (var option in NavPlacements.Options)
        {
            Assert.Equal(option, NavPlacements.FromKey(option.Key()));
        }

        Assert.Equal(NavPlacement.Auto, NavPlacements.FromKey(null));
        Assert.Equal(NavPlacement.Auto, NavPlacements.FromKey("diagonal"));
        Assert.Equal(NavPlacement.Right, NavPlacements.FromKey(" right\n"));
    }

    [Fact]
    public void PagesWrapAndSettingsStepsFromPlay()
    {
        Assert.Equal("kbm", ShellPages.Step("play", 1));
        Assert.Equal("adapter", ShellPages.Step("play", -1));
        Assert.Equal("play", ShellPages.Step("adapter", 1));
        Assert.Equal("kbm", ShellPages.Step("settings", 1));
    }

    [Fact]
    public void ATapIsATapAndAHoldFiresOnlyWhileStreaming()
    {
        var back = new BackKeyGesture();
        back.Down(0, repeat: false);
        Assert.Equal(BackKeyGesture.Result.Tap, back.Up());

        back.Down(0, repeat: false);
        Assert.Equal(BackKeyGesture.Result.None, back.Tick(799, holdAllowed: true));
        Assert.Equal(BackKeyGesture.Result.Hold, back.Tick(800, holdAllowed: true));
        // The release that follows a hold does nothing, and the hold fires once.
        Assert.Equal(BackKeyGesture.Result.None, back.Tick(1600, holdAllowed: true));
        Assert.Equal(BackKeyGesture.Result.None, back.Up());

        // Not streaming: a long press is still a tap.
        back.Down(0, repeat: false);
        Assert.Equal(BackKeyGesture.Result.None, back.Tick(5000, holdAllowed: false));
        Assert.Equal(BackKeyGesture.Result.Tap, back.Up());
    }

    [Fact]
    public void AutoRepeatDoesNotRestartTheHoldAndCancelIsSilent()
    {
        var back = new BackKeyGesture();
        back.Down(0, repeat: false);
        back.Down(500, repeat: true);
        Assert.Equal(BackKeyGesture.Result.Hold, back.Tick(800, holdAllowed: true));

        back.Down(0, repeat: false);
        back.Cancel();
        Assert.Equal(BackKeyGesture.Result.None, back.Up());
    }

    [Fact]
    public void TheConsoleOwnsThePadOnlyOnTheBareGameScreen()
    {
        Assert.True(PadOwnership.ConsoleOwnsPad(streaming: true, "play", overlayOpen: false, navigationShown: false));
        Assert.False(PadOwnership.ConsoleOwnsPad(streaming: false, "play", overlayOpen: false, navigationShown: false));
        // The regression this rule exists for: the library while a game waits for a figure.
        Assert.False(PadOwnership.ConsoleOwnsPad(streaming: true, "amiibo", overlayOpen: false, navigationShown: false));
        Assert.False(PadOwnership.ConsoleOwnsPad(streaming: true, "play", overlayOpen: false, navigationShown: true));
        Assert.False(PadOwnership.ConsoleOwnsPad(streaming: true, "play", overlayOpen: true, navigationShown: false));
    }

    [Fact]
    public void ConsoleCuesShowOnlyWhileConnected()
    {
        var console = new ConsoleView(Player: 3, AmiiboWanted: true);
        Assert.Equal("Player 3", ConsoleCues.PlayerLabel(connected: true, console));
        Assert.Null(ConsoleCues.PlayerLabel(connected: false, console));
        Assert.Null(ConsoleCues.PlayerLabel(connected: true, new ConsoleView()));
        Assert.True(ConsoleCues.AmiiboWanted(connected: true, console));
        Assert.False(ConsoleCues.AmiiboWanted(connected: false, console));
    }

    private static AnalogFrame Pad(bool up = false, bool right = false, bool down = false, bool left = false) =>
        AnalogFrame.Neutral with { Dpad = new DpadState(up, right, down, left) };

    private static AnalogFrame Stick(int x, int y) => AnalogFrame.Neutral with { LeftX = x, LeftY = y };

    [Fact]
    public void ADpadPressStepsOnceThenRepeatsAfterTheDelay()
    {
        var nav = new GamepadUiNavigator();
        var none = ControllerButtonSet.Empty;
        Assert.Equal([UiCommand.Right], nav.Feed(none, Pad(right: true), 0));
        Assert.Empty(nav.Feed(none, Pad(right: true), 100));
        Assert.Null(nav.Tick(GamepadUiNavigator.RepeatDelayMillis - 1));
        Assert.Equal(UiCommand.Right, nav.Tick(GamepadUiNavigator.RepeatDelayMillis));
        Assert.Null(nav.Tick(GamepadUiNavigator.RepeatDelayMillis + 50));
        Assert.Equal(UiCommand.Right, nav.Tick(GamepadUiNavigator.RepeatDelayMillis + GamepadUiNavigator.RepeatIntervalMillis));
        Assert.Empty(nav.Feed(none, Pad(), 900));
        Assert.Null(nav.Tick(2000));
    }

    [Fact]
    public void TheStickNeedsAFirmPushAndCannotChatterAtTheEdge()
    {
        var nav = new GamepadUiNavigator();
        var none = ControllerButtonSet.Empty;
        Assert.Empty(nav.Feed(none, Stick(128, 60), 0));          // a lean is not a step
        Assert.Equal([UiCommand.Up], nav.Feed(none, Stick(128, 10), 10));
        Assert.Empty(nav.Feed(none, Stick(128, 50), 20));         // easing back, still held
        Assert.Empty(nav.Feed(none, Stick(128, 128), 30));        // released
        Assert.Equal([UiCommand.Left], nav.Feed(none, Stick(5, 128), 40));
    }

    [Fact]
    public void AConfirmsOnPressAndBGoesBackOnRelease()
    {
        var nav = new GamepadUiNavigator();
        var a = ControllerButtonSet.Empty.With(ControllerButton.A);
        var b = ControllerButtonSet.Empty.With(ControllerButton.B);
        Assert.Equal([UiCommand.Confirm], nav.Feed(a, AnalogFrame.Neutral, 0));
        Assert.Empty(nav.Feed(a, AnalogFrame.Neutral, 10));
        Assert.Empty(nav.Feed(b, AnalogFrame.Neutral, 20));
        Assert.Equal([UiCommand.Back], nav.Feed(ControllerButtonSet.Empty, AnalogFrame.Neutral, 30));
    }

    [Fact]
    public void ShouldersStepOncePerPress()
    {
        var nav = new GamepadUiNavigator();
        var r1 = ControllerButtonSet.Empty.With(ControllerButton.R1);
        Assert.Equal([UiCommand.NextPage], nav.Feed(r1, AnalogFrame.Neutral, 0));
        Assert.Empty(nav.Feed(r1, AnalogFrame.Neutral, 1000));
        Assert.Equal(
            [UiCommand.PreviousPage],
            nav.Feed(ControllerButtonSet.Empty.With(ControllerButton.L1), AnalogFrame.Neutral, 1100));
    }

    [Fact]
    public void TakingThePadNeutralizesTheConsoleAndHoldsItThere()
    {
        var session = new ControllerInputSession();
        var source = new ControllerSourceIdentity("pad", "Pad", 0x045E, 0x028E);
        // Checked as "anything held": an Xbox identity reports positionally, so
        // A reaches the console as B.
        var a = ControllerButtonSet.Empty.With(ControllerButton.A);
        session.ApplyPhysicalFrame(source, a, AnalogFrame.Neutral);
        Assert.NotEqual(ControllerButtonSet.Empty, session.Snapshot.Buttons);

        var observed = 0;
        session.PhysicalFrameObserved += (_, _) => observed++;
        session.SetAppHoldsPad(true);
        Assert.Equal(ControllerButtonSet.Empty, session.Snapshot.Buttons);

        // Frames still arrive (the menus read them) but never reach the console.
        session.ApplyPhysicalFrame(source, a, AnalogFrame.Neutral);
        Assert.Equal(ControllerButtonSet.Empty, session.Snapshot.Buttons);
        Assert.Equal(1, observed);

        session.SetAppHoldsPad(false);
        session.ApplyPhysicalFrame(source, a, AnalogFrame.Neutral);
        Assert.NotEqual(ControllerButtonSet.Empty, session.Snapshot.Buttons);
    }

    [Fact]
    public void PlaysConsoleButtonsAreHeldForAsLongAsThePlayerHoldsThem()
    {
        var session = new ControllerInputSession();
        session.SetConsoleButton(ControllerButton.Capture, true);
        Assert.True(session.Snapshot.Buttons.Contains(ControllerButton.Capture));

        session.SetConsoleButton(ControllerButton.Capture, false);
        Assert.False(session.Snapshot.Buttons.Contains(ControllerButton.Capture));
    }
    [Fact]
    public void ViewAndMenuHeldTogetherFireBackOncePerHold()
    {
        var chord = new BackChordGesture();
        var both = ControllerButtonSet.Empty.With(ControllerButton.Select).With(ControllerButton.Start);

        Assert.False(chord.Feed(both, 0));
        Assert.False(chord.Feed(both, BackChordGesture.HoldMillis - 1));
        Assert.True(chord.Feed(both, BackChordGesture.HoldMillis));
        Assert.False(chord.Feed(both, BackChordGesture.HoldMillis * 4));

        // Letting go of only one is not a new hold.
        Assert.False(chord.Feed(ControllerButtonSet.Empty.With(ControllerButton.Start), 3000));
        Assert.False(chord.Feed(both, 3000));
        Assert.False(chord.Feed(both, 4000));

        // Both released: the next hold fires again.
        Assert.False(chord.Feed(ControllerButtonSet.Empty, 5000));
        Assert.False(chord.Feed(both, 5000));
        Assert.True(chord.Feed(both, 5000 + BackChordGesture.HoldMillis));
    }

    [Fact]
    public void EitherButtonAloneOrAShortChordIsNotBack()
    {
        var chord = new BackChordGesture();
        var view = ControllerButtonSet.Empty.With(ControllerButton.Select);
        var both = view.With(ControllerButton.Start);

        Assert.False(chord.Feed(view, 0));
        Assert.False(chord.Feed(view, 5000));
        Assert.False(chord.Feed(both, 5000));
        Assert.False(chord.Feed(view, 5000 + BackChordGesture.HoldMillis - 10));
        Assert.False(chord.Feed(both, 5000 + BackChordGesture.HoldMillis));
    }
}
