using PicoSwitch.Bridge.Core;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// What the publisher decides to put on the wire.
///
/// These exist because the failure they guard against is invisible from inside
/// the app: every counter stays healthy, every frame is well formed, and the
/// only symptom is that a stick keeps playing out movement the player already
/// finished while their button presses wait behind it. The mechanism is that
/// nothing below this app can collapse a frame once it has been handed over, so
/// what matters is how much we hand over — and that is decided here.
/// </summary>
public sealed class ControllerLinkSendPolicyTests
{
    private static ControllerState Sticks(int lx = 128, int ly = 128, int rx = 128, int ry = 128) =>
        ControllerState.Neutral with { LeftX = lx, LeftY = ly, RightX = rx, RightY = ry };

    [Fact]
    public void StickNoiseIsNotMovement()
    {
        // An analog stick is never still: its low bits flicker from sensor noise
        // even untouched. Byte-exact comparison calls every one of those a change
        // and lets an idle stick set the send rate.
        var settled = Sticks(lx: 130, ly: 126);
        var jittering = Sticks(lx: 131, ly: 125);

        Assert.NotEqual(settled, jittering);
        Assert.False(ControllerLinkSendPolicy.AnalogMoved(jittering, settled));
    }

    [Fact]
    public void RealMovementIsMovement()
    {
        var basis = Sticks(lx: 128);
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(lx: 130), basis));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(lx: 126), basis));
    }

    [Fact]
    public void EveryAxisAndTriggerCounts()
    {
        var basis = ControllerState.Neutral;

        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(lx: 200), basis));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(ly: 200), basis));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(rx: 200), basis));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(ry: 200), basis));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(
            basis with { LeftTrigger = 40 }, basis));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(
            basis with { RightTrigger = 40 }, basis));
    }

    [Fact]
    public void DriftCannotAccumulatePastTheThreshold()
    {
        // The comparison is against the LAST SENT state, not the previous sample,
        // so a stick creeping one count at a time still trips as soon as it has
        // genuinely moved. Comparing consecutive samples instead would let a slow
        // drift travel the whole range while never reporting a change.
        var sent = Sticks(lx: 128);

        Assert.False(ControllerLinkSendPolicy.AnalogMoved(Sticks(lx: 129), sent));
        Assert.True(ControllerLinkSendPolicy.AnalogMoved(Sticks(lx: 130), sent));
    }

    [Fact]
    public void ADigitalEdgeIsNeverFilteredByTheAnalogThreshold()
    {
        // A button is not subject to the analog threshold at all: it is a
        // discrete event, and the whole point of separating them is that a press
        // must not wait on what a stick is doing.
        var released = ControllerState.Neutral;
        var pressed = released with
        {
            Buttons = ControllerButtonSet.Empty.With(ControllerButton.A, true),
        };

        Assert.True(ControllerLinkSendPolicy.DigitalChanged(pressed, released));
        Assert.False(ControllerLinkSendPolicy.AnalogMoved(pressed, released));
    }

    [Fact]
    public void DpadCountsAsDigital()
    {
        var none = ControllerState.Neutral;
        Assert.True(ControllerLinkSendPolicy.DigitalChanged(none with { DpadUp = true }, none));
        Assert.True(ControllerLinkSendPolicy.DigitalChanged(none with { DpadLeft = true }, none));
    }

    [Fact]
    public void AnIdenticalStateIsNotAChangeAtAll()
    {
        var state = Sticks(lx: 200, ly: 60);
        Assert.False(ControllerLinkSendPolicy.AnalogMoved(state, state));
        Assert.False(ControllerLinkSendPolicy.DigitalChanged(state, state));
    }
}
