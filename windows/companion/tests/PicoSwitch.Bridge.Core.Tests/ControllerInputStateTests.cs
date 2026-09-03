using PicoSwitch.Bridge.Core;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The platform-neutral controller state machine.
///
/// The failures this guards against are the ones a console makes expensive: a
/// held input that survives a boundary reaches the game as a stuck button, and
/// two origins merged into one snapshot produce input nobody asked for.
/// </summary>
public sealed class ControllerInputStateTests
{
    [Fact]
    public void ButtonsAreHeldAsREPORTEDSoALayoutChangeResolvesThemAfresh()
    {
        var state = new ControllerInputState();
        state.SetRequestedLayout(ControllerFaceLayout.Xbox);
        state.PressButton(ControllerButton.A, true);
        Assert.Equal(ControllerButtonSet.Of(ControllerButton.B), state.State.Value.Buttons);

        // Switching layout neutralizes, so the held key does not silently change
        // meaning under the user's thumb. Pressing again resolves through the new
        // layout rather than through a stale translation.
        state.SetRequestedLayout(ControllerFaceLayout.Nintendo);
        Assert.Equal(ControllerButtonSet.Empty, state.State.Value.Buttons);
        state.PressButton(ControllerButton.A, true);
        Assert.Equal(ControllerButtonSet.Of(ControllerButton.A), state.State.Value.Buttons);
    }

    [Fact]
    public void DpadKeysAndHatAxesAreMergedNotOverwritten()
    {
        var state = new ControllerInputState();
        state.PressDpad(up: true);
        state.ApplyAnalog(new AnalogFrame(128, 128, 128, 128, 0, 0, DpadState.FromAxes(1f, 0f)));

        var published = state.State.Value;
        Assert.True(published.DpadUp);
        Assert.True(published.DpadRight);

        // Releasing the KEY must not clear the still-held axis contribution.
        state.PressDpad(up: false);
        Assert.False(state.State.Value.DpadUp);
        Assert.True(state.State.Value.DpadRight);
    }

    [Fact]
    public void AnAnalogFrameWithoutHatAxesLeavesTheHatContributionAlone()
    {
        var state = new ControllerInputState();
        state.ApplyAnalog(new AnalogFrame(128, 128, 128, 128, 0, 0, DpadState.FromAxes(0f, -1f)));
        Assert.True(state.State.Value.DpadUp);

        // Null means "this source has no hat axes", not "the hat is centered".
        state.ApplyAnalog(new AnalogFrame(200, 128, 128, 128, 0, 0));
        Assert.True(state.State.Value.DpadUp);
        Assert.Equal(200, state.State.Value.LeftX);
    }

    [Fact]
    public void OneAnalogEventPublishesExactlyOneSnapshot()
    {
        var state = new ControllerInputState();
        var notifications = 0;
        state.State.Changed += () => notifications++;

        state.ApplyAnalog(new AnalogFrame(10, 20, 30, 40, 50, 60, DpadState.FromAxes(-1f, 0f)));
        Assert.Equal(1, notifications);

        // An identical event is not a change: at the publish cadence (250 Hz) an
        // unchanged snapshot must not wake every observer.
        state.ApplyAnalog(new AnalogFrame(10, 20, 30, 40, 50, 60, DpadState.FromAxes(-1f, 0f)));
        Assert.Equal(1, notifications);
    }

    [Fact]
    public void OnePolledPhysicalFramePublishesOneCompleteSnapshot()
    {
        var state = new ControllerInputState();
        var notifications = 0;
        state.State.Changed += () => notifications++;

        state.ApplyPhysicalFrame(
            ControllerButtonSet.Of(ControllerButton.A, ControllerButton.R1),
            new AnalogFrame(10, 20, 30, 40, 50, 60, DpadState.FromAxes(1f, -1f)));

        Assert.Equal(1, notifications);
        var published = state.State.Value;
        Assert.True(published.Buttons.Contains(ControllerButton.B));
        Assert.True(published.Buttons.Contains(ControllerButton.R1));
        Assert.True(published.DpadUp);
        Assert.True(published.DpadRight);
        Assert.Equal(10, published.LeftX);
        Assert.Equal(60, published.RightTrigger);
    }

    [Fact]
    public void VirtualButtonsAreIndependentOfPhysicalOnesAndOfAuthority()
    {
        var state = new ControllerInputState();
        state.SetRequestedLayout(ControllerFaceLayout.Nintendo);
        state.PressButton(ControllerButton.A, true);
        state.SetVirtualButton(ControllerButton.A, true);
        Assert.Equal(ControllerButtonSet.Of(ControllerButton.A), state.State.Value.Buttons);

        // Releasing one origin must not cancel the other.
        state.PressButton(ControllerButton.A, false);
        Assert.Equal(ControllerButtonSet.Of(ControllerButton.A), state.State.Value.Buttons);

        // Home/Capture/C have no physical key on most hosts, so the virtual path is
        // the only path and is not gated on authority.
        state.SetAuthority(InputAuthority.Touch);
        state.SetVirtualButton(ControllerButton.Home, true);
        Assert.True(state.State.Value.Buttons.Contains(ControllerButton.Home));
    }

    [Fact]
    public void TheInactiveOriginsMutationsAreDISCARDEDNotRetained()
    {
        var state = new ControllerInputState();
        state.SetAuthority(InputAuthority.Touch);

        // Physical input while touch is authoritative is dropped, not buffered:
        // otherwise it would all arrive at once when authority came back.
        state.PressButton(ControllerButton.A, true);
        state.ApplyAnalog(new AnalogFrame(0, 0, 0, 0, 255, 255));
        Assert.Equal(ControllerState.Neutral, state.State.Value);

        state.SetAuthority(InputAuthority.Physical);
        Assert.Equal(ControllerState.Neutral, state.State.Value);
    }

    [Fact]
    public void SwitchingAuthorityAlwaysNeutralizesEvenMidHold()
    {
        var state = new ControllerInputState();
        state.PressButton(ControllerButton.L1, true);
        state.ApplyAnalog(new AnalogFrame(0, 255, 0, 255, 200, 200));
        Assert.NotEqual(ControllerState.Neutral, state.State.Value);

        // A control that was down at the moment of the switch belongs to the origin
        // being left. Carrying it across is how a console walks into a wall.
        state.SetAuthority(InputAuthority.Touch);
        Assert.Equal(ControllerState.Neutral, state.State.Value);
    }

    [Fact]
    public void TouchContributionIsVisibleEvenWhileTheAuthorityDiscardsIt()
    {
        var state = new ControllerInputState();
        state.SetAuthority(InputAuthority.Touch);
        var contribution = TouchContribution.Neutral with
        {
            LogicalButtons = ControllerButtonSet.Of(ControllerButton.R1),
        };
        state.ApplyTouch(contribution);
        Assert.Equal(contribution, state.TouchContribution);

        state.SetAuthority(InputAuthority.Physical);

        // Neutralized on the boundary, so a diagnostic can tell "touch is holding
        // nothing" from "touch is holding something that is being discarded".
        Assert.Equal(TouchContribution.Neutral, state.TouchContribution);
    }

    [Fact]
    public void TouchFacePositionsUseTheOppositeMapperFromPhysicalKeys()
    {
        var state = new ControllerInputState();
        state.SetRequestedLayout(ControllerFaceLayout.Nintendo);
        state.SetAuthority(InputAuthority.Touch);
        state.ApplyTouch(TouchContribution.Neutral with
        {
            PositionalButtons = ControllerButtonSet.Of(ControllerButton.A),
        });

        // Under a Nintendo presentation the south slot is drawn B and sends B --
        // the opposite of what the same layout does to a physical `A` key.
        Assert.Equal(ControllerButtonSet.Of(ControllerButton.B), state.State.Value.Buttons);
    }

    [Fact]
    public void TouchLogicalButtonsAreNeverFaceSwapped()
    {
        var state = new ControllerInputState();
        state.SetRequestedLayout(ControllerFaceLayout.Nintendo);
        state.SetAuthority(InputAuthority.Touch);
        state.ApplyTouch(TouchContribution.Neutral with
        {
            LogicalButtons = ControllerButtonSet.Of(ControllerButton.A, ControllerButton.L2),
        });

        Assert.True(state.State.Value.Buttons.Contains(ControllerButton.A));
    }

    [Fact]
    public void ChangingSourceReResolvesTheLayoutAndNeutralizes()
    {
        var state = new ControllerInputState();
        state.PressButton(ControllerButton.Start, true);

        state.SetSource(new ControllerSourceIdentity("odin", "Odin Controller", 0x2020, 0x0111));
        Assert.Equal(ControllerFaceLayout.Nintendo, state.ResolvedLayout.Layout);
        Assert.Equal(ControllerState.Neutral, state.State.Value);

        // Nothing held on the previous source has any meaning on this one.
        state.SetSource(null);
        Assert.Equal(ControllerFaceLayout.Xbox, state.ResolvedLayout.Layout);
        Assert.Equal(ControllerState.Neutral, state.State.Value);
    }

    [Fact]
    public void NeutralizeClearsEveryOriginAtOnce()
    {
        var state = new ControllerInputState();
        state.PressButton(ControllerButton.A, true);
        state.SetVirtualButton(ControllerButton.Home, true);
        state.PressDpad(down: true);
        state.ApplyAnalog(new AnalogFrame(1, 2, 3, 4, 5, 6));

        state.Neutralize();
        Assert.Equal(ControllerState.Neutral, state.State.Value);
        Assert.Equal(TouchContribution.Neutral, state.TouchContribution);
    }

    [Fact]
    public void AManualLayoutOverridesTheResolverAndSaysSo()
    {
        var state = new ControllerInputState();
        state.SetSource(new ControllerSourceIdentity("odin", "Odin Controller", 0x2020, 0x0111));
        Assert.Equal(ControllerFaceLayout.Nintendo, state.ResolvedLayout.Layout);

        state.SetRequestedLayout(ControllerFaceLayout.Xbox);
        Assert.Equal(ControllerFaceLayout.Xbox, state.ResolvedLayout.Layout);
        Assert.Equal("Selected manually", state.ResolvedLayout.Reason);
    }
}
