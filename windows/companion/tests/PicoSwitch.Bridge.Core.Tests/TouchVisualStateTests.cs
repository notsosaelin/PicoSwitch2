using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The state an on-screen controller has to DRAW, as opposed to the state it sends.
///
/// The two are not the same question. What reaches the console is one vector per
/// stick and one level per trigger side; what a player has to see is which shape
/// under their thumb is doing something. A surface that answers the second question
/// with the first draws the wrong stick the moment a layout holds more than one.
///
/// These live at the engine because the alternative is a renderer deriving press
/// state from the pointer events it happened to receive — a second, quietly
/// divergent copy of ownership, the stick clamp and the latch state machine. The
/// picture and the input must come from one place, and this is the read that makes
/// that possible.
/// </summary>
public sealed class TouchVisualStateTests
{
    [Fact]
    public void AHeldControlIsNamed()
    {
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);

        var held = harness.Engine.Diagnostics().HeldControls;
        Assert.Contains(TouchLayoutV1.FaceSouth, held);
        Assert.Single(held);
    }

    [Fact]
    public void AReleasedControlStopsBeingNamed()
    {
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);
        harness.Up(1, TouchLayoutV1.FaceSouth);

        Assert.Empty(harness.Engine.Diagnostics().HeldControls);
    }

    [Fact]
    public void HeldControlsAgreesWithTheCount()
    {
        // OwnedControls was the only window onto ownership before this, and a count
        // that disagreed with the ids would send a surface hunting for a press that
        // was never there.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);
        harness.Down(2, TouchLayoutV1.FaceEast);

        var diagnostics = harness.Engine.Diagnostics();
        Assert.Equal(diagnostics.OwnedControls, diagnostics.HeldControls.Count);
    }

    [Fact]
    public void ALatchedControlIsHeldByNoContact()
    {
        // The distinction the surface draws differently, and the reason it needs
        // both sets. A latch keeps the control down with nothing touching it: a
        // renderer painting latches as ordinary presses would tell the player to
        // lift a finger they are not using, and one painting only HeldControls
        // would show a control it is still sending as released.
        var harness = Latched(TouchLayoutV1.FaceSouth);

        var diagnostics = harness.Engine.Diagnostics();
        Assert.Contains(TouchLayoutV1.FaceSouth, diagnostics.LatchedControls);
        Assert.DoesNotContain(TouchLayoutV1.FaceSouth, diagnostics.HeldControls);
    }

    /// <summary>The tap / press / dwell / slide that engages a hold, then lifts.</summary>
    private static TouchHarness Latched(string controlId)
    {
        var harness = new TouchHarness();
        var config = harness.Engine.Config;
        var (x, y) = harness.Centre(controlId);
        const long Ms = TouchHarness.Ms;

        harness.Send(new TouchContact(1, TouchPhase.Down, x, y, 100 * Ms));
        harness.Send(new TouchContact(1, TouchPhase.Up, x, y, 150 * Ms));

        var pressAt = 250 * Ms;
        harness.Send(new TouchContact(2, TouchPhase.Down, x, y, pressAt));

        var armAt = pressAt + config.Latch.LatchEngageThresholdNanos;
        harness.Engine.OnTick(armAt);

        var commit = config.Latch.LatchCommitDistanceUnits * harness.Region.UnitScale;
        harness.Send(new TouchContact(2, TouchPhase.Move, x + commit + 4f, y, armAt + Ms));
        harness.Send(new TouchContact(2, TouchPhase.Up, x + commit + 4f, y, armAt + (2 * Ms)));

        return harness;
    }

    [Fact]
    public void AStickAtRestReportsNoDeflection()
    {
        var harness = new TouchHarness();

        Assert.Empty(harness.Engine.Diagnostics().StickVectors);
    }

    [Fact]
    public void AStickReportsWhereItIsPushed()
    {
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);

        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.MoveTo(1, stick.CenterX + stick.TrackingRadius, stick.CenterY, 2 * TouchHarness.Ms);

        var vector = harness.Engine.Diagnostics().StickVectors[TouchLayoutV1.StickLeft];
        Assert.Equal(1f, vector.X, 3);
        Assert.Equal(0f, vector.Y, 3);
    }

    [Fact]
    public void DeflectionIsReportedInScreenAxes()
    {
        // Y grows DOWNWARD, matching the deltas the engine resolved. A surface adds
        // this straight onto a control centre; if the sign convention were the
        // gamepad's instead, every knob would move the opposite way from the thumb
        // and no test that only checked magnitude would notice.
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);

        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.MoveTo(1, stick.CenterX, stick.CenterY + stick.TrackingRadius, 2 * TouchHarness.Ms);

        var vector = harness.Engine.Diagnostics().StickVectors[TouchLayoutV1.StickLeft];
        Assert.True(vector.Y > 0.9f, $"pushing down should report +Y, got {vector.Y}");
    }

    [Fact]
    public void DeflectionIsClampedToFullScale()
    {
        // The thumb is allowed to leave the circle — the stick keeps the contact and
        // clamps — so the reported vector must clamp too. A surface scaling an
        // unclamped value by the tracking radius would draw the knob outside the
        // control it belongs to.
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);

        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.MoveTo(
            1, stick.CenterX + (stick.TrackingRadius * 4), stick.CenterY, 2 * TouchHarness.Ms);

        var vector = harness.Engine.Diagnostics().StickVectors[TouchLayoutV1.StickLeft];
        Assert.Equal(1f, MathF.Sqrt((vector.X * vector.X) + (vector.Y * vector.Y)), 3);
    }

    [Fact]
    public void ReleasingTheStickClearsItsDeflection()
    {
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);

        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.MoveTo(1, stick.CenterX + stick.TrackingRadius, stick.CenterY, 2 * TouchHarness.Ms);
        harness.UpAt(1, stick.CenterX + stick.TrackingRadius, stick.CenterY, 3 * TouchHarness.Ms);

        Assert.Empty(harness.Engine.Diagnostics().StickVectors);
    }

    [Fact]
    public void DeflectionIsKeyedByControlNotBySide()
    {
        // The reason this read exists at all. LeftStick reports what the CONSOLE is
        // sent — one vector for the side, from whichever instance won ownership — so
        // a surface drawing from it would move the wrong shape whenever a layout
        // carries two instances of the same stick.
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);

        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.MoveTo(1, stick.CenterX + stick.TrackingRadius, stick.CenterY, 2 * TouchHarness.Ms);

        var vectors = harness.Engine.Diagnostics().StickVectors;
        Assert.True(vectors.ContainsKey(TouchLayoutV1.StickLeft));
        Assert.DoesNotContain(TouchLayoutV1.StickRight, vectors.Keys);
    }

    [Fact]
    public void ASnapshotDoesNotChangeUnderTheCaller()
    {
        // One snapshot is one instant. A surface iterating the layout while the
        // engine mutated behind it could paint a press that had already lifted, and
        // the copy is what makes a repaint self-consistent.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.StickLeft);

        var before = harness.Engine.Diagnostics();
        var heldBefore = before.HeldControls.Count;
        var vectorsBefore = before.StickVectors.Count;

        harness.Up(1, TouchLayoutV1.StickLeft);

        Assert.Equal(heldBefore, before.HeldControls.Count);
        Assert.Equal(vectorsBefore, before.StickVectors.Count);
    }
}
