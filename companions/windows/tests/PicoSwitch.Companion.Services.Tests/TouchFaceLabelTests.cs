using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;
using PicoSwitch.Companion.Services;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The on-screen controller must send the button it is DRAWN with.
///
/// Observed on hardware 2026-09-03: every face button transmitted the opposite
/// letter to its label while the face-layout preference was Nintendo, and the
/// correct one while it was Xbox. The player sees "A" and the console receives B.
///
/// The mistake is one of ownership. The face-layout preference describes the
/// printed legend on somebody's PHYSICAL controller, so that a positional pad
/// reporting "A" for its bottom button lands on the console's bottom button. The
/// on-screen controller has no printed legend to describe — this app draws its
/// letters, from a fixed presentation (<see cref="TouchControlNaming.FaceLayout"/>),
/// and with a freeform layout editor a control's LABEL is its binding. Running
/// that label through a second, unrelated mapping is what inverts it.
///
/// These tests assert the invariant directly — drawn letter equals transmitted
/// button — under every setting, so no future change to the physical-controller
/// preference can silently reach the on-screen pad again.
/// </summary>
public sealed class TouchFaceLabelTests
{
    /// <summary>
    /// Drive one on-screen face control through the REAL session and report the
    /// button that reaches the published state.
    /// </summary>
    private static ControllerButton Press(string controlId, ControllerFaceLayout layout)
    {
        var composed = TouchLayoutComposer.Compose(
            TouchProfileCatalog.Require(TouchProfileId.Pro2));
        var region = new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f);
        var resolved = TouchLayoutResolver.Resolve(composed.Layout, region);
        Assert.True(resolved.Fits, resolved.Problem);

        var session = new ControllerInputSession();
        session.SetTouchLayout(resolved);
        session.SetFaceLayout(layout);
        session.ActivateTouch();

        var control = resolved.Control(controlId)
            ?? throw new InvalidOperationException($"no control '{controlId}'");

        const long ms = 1_000_000L;
        session.DispatchTouchContacts(
        [
            new TouchContact(1, TouchPhase.Down, control.CenterX, control.CenterY, ms),
        ]);

        var pressed = session.Snapshot.Buttons.Values.ToList();
        Assert.Single(pressed);
        return pressed[0];
    }

    /// <summary>The letter the renderer draws on that control.</summary>
    private static ControllerButton DrawnLabel(FaceButtonPosition position) =>
        ControllerLayoutResolver.MapTouchFacePosition(
            position.Positional(), TouchControlNaming.FaceLayout);

    [Theory]
    [InlineData(TouchLayoutV1.FaceNorth, FaceButtonPosition.North)]
    [InlineData(TouchLayoutV1.FaceSouth, FaceButtonPosition.South)]
    [InlineData(TouchLayoutV1.FaceEast, FaceButtonPosition.East)]
    [InlineData(TouchLayoutV1.FaceWest, FaceButtonPosition.West)]
    public void SendsTheLetterItIsDrawnWith_UnderEveryPreference(
        string controlId, FaceButtonPosition position)
    {
        var expected = DrawnLabel(position);

        foreach (var layout in new[]
                 {
                     ControllerFaceLayout.Auto,
                     ControllerFaceLayout.Nintendo,
                     ControllerFaceLayout.Xbox,
                 })
        {
            Assert.Equal(expected, Press(controlId, layout));
        }
    }

    [Fact]
    public void ThePhysicalPreferenceDoesNotReachTheOnScreenPad()
    {
        // The whole defect in one assertion: the same on-screen control, under two
        // opposite preferences, must produce the same console button.
        Assert.Equal(
            Press(TouchLayoutV1.FaceEast, ControllerFaceLayout.Nintendo),
            Press(TouchLayoutV1.FaceEast, ControllerFaceLayout.Xbox));
    }

    [Fact]
    public void PhysicalControllerMappingIsUnchanged()
    {
        // The preference must keep doing its real job. A positional pad reports A
        // for its bottom button; on a Nintendo-labelled console that bottom button
        // is B, so the Xbox layout swaps and the Nintendo layout passes through.
        // Nothing in this fix may touch that.
        Assert.Equal(
            ControllerButton.B,
            ControllerLayoutResolver.MapPhysicalFaceKey(
                ControllerButton.A, ControllerFaceLayout.Xbox));

        Assert.Equal(
            ControllerButton.A,
            ControllerLayoutResolver.MapPhysicalFaceKey(
                ControllerButton.A, ControllerFaceLayout.Nintendo));
    }
}
