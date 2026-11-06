using PicoSwitch.Bridge.Core;
using PicoSwitch.Bridge.Touch;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// A resolved Pro Controller 2 layout in a fixed region, plus the contact vocabulary
/// the tests are written in.
///
/// Real geometry from the shipped catalog rather than a hand-built fixture: the whole
/// point of these tests is that ownership and the gestures behave on the layout the
/// product actually draws.
/// </summary>
internal sealed class TouchHarness
{
    /// <summary>One millisecond, in the contact clock's units.</summary>
    public const long Ms = 1_000_000L;

    private readonly List<TouchContribution> published = [];

    public TouchHarness(TouchProfileId profileId = TouchProfileId.Pro2)
    {
        Profile = TouchProfileCatalog.Require(profileId);
        var composed = TouchLayoutComposer.Compose(Profile);
        // 800 x 420 logical units, deliberately NOT 800 x 400.
        //
        // The shipped GameCube layout has a documented pre-existing collision in a band
        // around 2:1 (see TouchShippedLayoutTests), and a layout that does not fit
        // claims no contacts at all — so a harness sitting in that band would test
        // nothing while looking like it tested everything.
        Region = new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f);
        Layout = TouchLayoutResolver.Resolve(composed.Layout, Region);
        if (!Layout.Fits)
        {
            throw new InvalidOperationException(
                $"harness layout for {profileId} does not fit: {Layout.Problem}");
        }

        Engine = new TouchControlEngine(contribution => published.Add(contribution));
        Engine.InstallLayout(Layout);
        Contacts = new TouchContactTracker(Engine);
    }

    public TouchControllerProfile Profile { get; }

    public TouchLayoutRegion Region { get; }

    public ResolvedTouchLayout Layout { get; }

    public TouchControlEngine Engine { get; }

    public TouchContactTracker Contacts { get; }

    public TouchContribution Contribution => Engine.Contribution;

    public IReadOnlyList<TouchContribution> Published => published;

    public ResolvedTouchControl Control(string id) =>
        Layout.Control(id) ?? throw new InvalidOperationException($"no control '{id}'");

    /// <summary>The centre of a control, which is always inside its own hit region.</summary>
    public (float X, float Y) Centre(string id)
    {
        var control = Control(id);
        return (control.CenterX, control.CenterY);
    }

    public void Down(long contactId, string controlId, long timeNanos = Ms)
    {
        var (x, y) = Centre(controlId);
        Send(new TouchContact(contactId, TouchPhase.Down, x, y, timeNanos));
    }

    public void DownAt(long contactId, float x, float y, long timeNanos = Ms) =>
        Send(new TouchContact(contactId, TouchPhase.Down, x, y, timeNanos));

    public void MoveTo(long contactId, float x, float y, long timeNanos = Ms) =>
        Send(new TouchContact(contactId, TouchPhase.Move, x, y, timeNanos));

    public void Up(long contactId, string controlId, long timeNanos = Ms)
    {
        var (x, y) = Centre(controlId);
        Send(new TouchContact(contactId, TouchPhase.Up, x, y, timeNanos));
    }

    public void UpAt(long contactId, float x, float y, long timeNanos = Ms) =>
        Send(new TouchContact(contactId, TouchPhase.Up, x, y, timeNanos));

    /// <summary>
    /// One contact through the tracker.
    ///
    /// The batch deliberately carries every contact still down, which is the tracker's
    /// stated contract — a batch that mentioned only the changed contact would have
    /// every other one cancelled, exactly as a real adapter that forgot to accumulate
    /// would discover.
    /// </summary>
    public void Send(TouchContact contact)
    {
        var batch = new List<TouchContact> { contact };
        foreach (var id in live.Where(id => id != contact.Id))
        {
            batch.Add(new TouchContact(id, TouchPhase.Move, positions[id].X, positions[id].Y,
                                       contact.TimeNanos));
        }

        if (contact.Phase is TouchPhase.Down or TouchPhase.Move)
        {
            live.Add(contact.Id);
            positions[contact.Id] = (contact.X, contact.Y);
        }
        else
        {
            live.Remove(contact.Id);
            positions.Remove(contact.Id);
        }

        Contacts.Dispatch(batch);
    }

    private readonly HashSet<long> live = [];
    private readonly Dictionary<long, (float X, float Y)> positions = [];
}

/// <summary>
/// Contact ownership: the rules that make a five-finger controller work, and the one
/// keyed-by-index defect that breaks every on-screen controller that gets it wrong.
/// </summary>
public sealed class TouchOwnershipTests
{
    [Fact]
    public void OneContactOwnsOneControlAndKeepsItUntilItEnds()
    {
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);

        Assert.Equal(TouchLayoutV1.FaceSouth, harness.Engine.OwnerOf(1));
        Assert.Equal(1, harness.Engine.ContactOn(TouchLayoutV1.FaceSouth));

        harness.Up(1, TouchLayoutV1.FaceSouth);
        Assert.Null(harness.Engine.OwnerOf(1));
        Assert.Null(harness.Engine.ContactOn(TouchLayoutV1.FaceSouth));
    }

    [Fact]
    public void ASecondContactOnAnOwnedControlIsIgnoredRatherThanStealingIt()
    {
        // Two contradictory positions for one stick have no correct answer, and
        // silently taking the newest would make a resting palm beat a deliberate thumb.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.Down(2, TouchLayoutV1.StickLeft);

        Assert.Equal(1, harness.Engine.ContactOn(TouchLayoutV1.StickLeft));
        Assert.Null(harness.Engine.OwnerOf(2));
        Assert.Equal(1, harness.Engine.Diagnostics().ContactsContested);
    }

    [Fact]
    public void FiveSimultaneousContactsAreFiveIndependentControls()
    {
        var harness = new TouchHarness();
        var ids = new[]
        {
            TouchLayoutV1.StickLeft, TouchLayoutV1.FaceSouth, TouchLayoutV1.FaceEast,
            TouchLayoutV1.ShoulderLeft, TouchLayoutV1.ShoulderRight,
        };

        for (var index = 0; index < ids.Length; index++)
        {
            harness.Down(index + 1, ids[index]);
        }

        Assert.Equal(5, harness.Engine.Diagnostics().OwnedControls);
        for (var index = 0; index < ids.Length; index++)
        {
            Assert.Equal(ids[index], harness.Engine.OwnerOf(index + 1));
        }
    }

    [Fact]
    public void OwnershipIsKeyedOnTheStableIdAndSurvivesAnEarlierContactLifting()
    {
        // THE defect this keying exists to prevent. A platform is free to reorder
        // contacts between events, so an implementation keyed on the array index
        // silently swaps which control a thumb is holding the moment the first one
        // lifts — invisible with two fingers, broken with three.
        var harness = new TouchHarness();
        harness.Down(10, TouchLayoutV1.FaceSouth);
        harness.Down(20, TouchLayoutV1.FaceEast);
        harness.Down(30, TouchLayoutV1.FaceNorth);

        harness.Up(10, TouchLayoutV1.FaceSouth);

        Assert.Equal(TouchLayoutV1.FaceEast, harness.Engine.OwnerOf(20));
        Assert.Equal(TouchLayoutV1.FaceNorth, harness.Engine.OwnerOf(30));
    }

    [Fact]
    public void AStickKeepsItsContactWhenTheThumbLeavesTheCircle()
    {
        // The alternative is the thumb wandering into a face button mid-turn.
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);
        harness.Down(1, TouchLayoutV1.StickLeft);

        harness.MoveTo(1, stick.CenterX + (stick.HalfWidth * 4f), stick.CenterY);

        Assert.Equal(TouchLayoutV1.StickLeft, harness.Engine.OwnerOf(1));

        // …and it clamps to full deflection rather than following the finger.
        Assert.Equal(255, harness.Contribution.LeftX);
    }

    [Fact]
    public void AContactTheHostStopsMentioningIsCancelled()
    {
        // A dropped contact is what a window losing its gesture looks like from in
        // here, and the consequence would be a control held down forever with no
        // contact left to release it.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);
        Assert.True(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));

        // A batch that no longer mentions contact 1 at all.
        harness.Contacts.Dispatch([]);

        Assert.Null(harness.Engine.OwnerOf(1));
        Assert.Equal(ControllerButtonSet.Empty, harness.Contribution.PositionalButtons);
    }

    [Fact]
    public void ReleasedContactsAreQuarantinedUntilTheyLift()
    {
        // Otherwise a finger still on the glass at a boundary would immediately claim
        // the new geometry it was never pressed against.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);
        harness.Contacts.ReleaseAll(TouchReleaseReason.EditorEntered);

        Assert.Equal(1, harness.Contacts.QuarantinedCount);

        // A move from the same still-down finger claims nothing.
        harness.MoveTo(1, harness.Centre(TouchLayoutV1.FaceEast).X,
                       harness.Centre(TouchLayoutV1.FaceEast).Y);
        Assert.Null(harness.Engine.OwnerOf(1));
    }

    [Fact]
    public void ATouchOutsideEveryControlClaimsNothing()
    {
        var harness = new TouchHarness();

        // The quiet centre of the layout, which is deliberately empty.
        harness.DownAt(1, harness.Region.Width / 2f, harness.Region.Height * 0.65f);

        Assert.Null(harness.Engine.OwnerOf(1));
        Assert.Equal(1, harness.Engine.Diagnostics().ContactsUnclaimed);
        Assert.Equal(TouchContribution.Neutral, harness.Contribution);
    }
}

/// <summary>What the engine publishes, and how duplicates aggregate.</summary>
public sealed class TouchContributionTests
{
    [Fact]
    public void AFacePressPublishesItsPositionAndNotItsLetter()
    {
        // The face-layout resolver decides what a POSITION means; the engine must never
        // pre-resolve it, or the two opposite mappers would both run.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);

        Assert.True(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));
        Assert.Equal(ControllerButtonSet.Empty, harness.Contribution.LogicalButtons);
    }

    [Fact]
    public void AShoulderPublishesALogicalButtonAndIsNeverFaceSwapped()
    {
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.ShoulderLeft);

        Assert.True(harness.Contribution.LogicalButtons.Contains(ControllerButton.L1));
        Assert.Equal(ControllerButtonSet.Empty, harness.Contribution.PositionalButtons);
    }

    [Fact]
    public void ADigitalTriggerPublishesBothItsBitAndItsAnalogValue()
    {
        // A physical trigger publishes both and the adapter's seam reads either; a touch
        // trigger that published only one would be a second contract for the control.
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.TriggerLeft);

        Assert.True(harness.Contribution.LogicalButtons.Contains(ControllerButton.L2));
        Assert.Equal(255, harness.Contribution.LeftTrigger);
    }

    [Fact]
    public void AStickPublishesCircularlyClampedAxes()
    {
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);
        harness.Down(1, TouchLayoutV1.StickLeft);

        // A long diagonal drag. Clamping the axes independently would let this reach
        // full scale on BOTH, which is the square-gate defect.
        harness.MoveTo(1,
            stick.CenterX + (stick.TrackingRadius * 4f),
            stick.CenterY + (stick.TrackingRadius * 4f));

        var contribution = harness.Contribution;
        var dx = contribution.LeftX - TouchAxis.Neutral;
        var dy = contribution.LeftY - TouchAxis.Neutral;
        var magnitude = MathF.Sqrt((dx * dx) + (dy * dy)) / 127f;

        Assert.True(magnitude <= 1.02f, $"diagonal reached {magnitude:0.000} of full scale");
        Assert.True(magnitude >= 0.98f, $"diagonal only reached {magnitude:0.000}");
    }

    [Fact]
    public void TheStickReturnsToExactCentreTheInstantTheThumbLeaves()
    {
        var harness = new TouchHarness();
        var stick = harness.Control(TouchLayoutV1.StickLeft);
        harness.Down(1, TouchLayoutV1.StickLeft);
        harness.MoveTo(1, stick.CenterX + stick.TrackingRadius, stick.CenterY);
        Assert.NotEqual(TouchAxis.Neutral, harness.Contribution.LeftX);

        harness.UpAt(1, stick.CenterX + stick.TrackingRadius, stick.CenterY);

        Assert.Equal(TouchAxis.Neutral, harness.Contribution.LeftX);
        Assert.Equal(TouchAxis.Neutral, harness.Contribution.LeftY);
    }

    [Fact]
    public void TheDpadCannotProduceOpposingDirectionsFromOneContact()
    {
        // Structurally impossible: one angle selects one sector, and only diagonals set
        // two flags. Nothing here needs a cancellation rule.
        var harness = new TouchHarness();
        var dpad = harness.Control(TouchLayoutV1.Dpad);
        harness.Down(1, TouchLayoutV1.Dpad);

        foreach (var angle in Enumerable.Range(0, 36).Select(step => step * 10f))
        {
            var radians = angle * MathF.PI / 180f;
            harness.MoveTo(1,
                dpad.CenterX + (MathF.Cos(radians) * dpad.TrackingRadius * 0.9f),
                dpad.CenterY + (MathF.Sin(radians) * dpad.TrackingRadius * 0.9f));

            var pad = harness.Contribution.Dpad;
            Assert.False(pad.Up && pad.Down, $"{angle} degrees produced Up+Down");
            Assert.False(pad.Left && pad.Right, $"{angle} degrees produced Left+Right");
        }
    }

    [Fact]
    public void OnlyDistinctContributionsAreEmitted()
    {
        // The session coalesces onto the publish cadence (250 Hz); republishing
        // an unchanged controller would be pure noise on that path.
        var harness = new TouchHarness();
        var before = harness.Published.Count;

        harness.Down(1, TouchLayoutV1.FaceSouth);
        var afterPress = harness.Published.Count;
        Assert.Equal(before + 1, afterPress);

        // A move on a BUTTON changes nothing that is published.
        var centre = harness.Centre(TouchLayoutV1.FaceSouth);
        harness.MoveTo(1, centre.X + 1f, centre.Y + 1f);

        Assert.Equal(afterPress, harness.Published.Count);
    }

    [Fact]
    public void ReleaseAllIsIdempotentAndNeverInventsAPress()
    {
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);

        harness.Engine.ReleaseAll(TouchReleaseReason.LinkEnded);
        var afterFirst = harness.Contribution;
        harness.Engine.ReleaseAll(TouchReleaseReason.LinkEnded);

        Assert.Equal(TouchContribution.Neutral, afterFirst);
        Assert.Equal(TouchContribution.Neutral, harness.Contribution);
        Assert.Equal(TouchReleaseReason.LinkEnded,
                     harness.Engine.Diagnostics().LastReleaseReason);
    }

    [Fact]
    public void EveryBoundaryReasonIsRecordedSoAStuckButtonCanBeExplained()
    {
        // "The console kept walking" and "the app cleared input but the link was already
        // gone" are different defects with the same symptom.
        foreach (var reason in Enum.GetValues<TouchReleaseReason>())
        {
            var harness = new TouchHarness();
            harness.Down(1, TouchLayoutV1.FaceSouth);
            harness.Engine.ReleaseAll(reason);

            Assert.Equal(reason, harness.Engine.Diagnostics().LastReleaseReason);
            Assert.Equal(TouchContribution.Neutral, harness.Contribution);
        }
    }

    [Fact]
    public void ChangingGeometryReleasesFirstBecauseEveryRetainedPositionIsStale()
    {
        var harness = new TouchHarness();
        harness.Down(1, TouchLayoutV1.FaceSouth);

        harness.Engine.SetLayout(harness.Layout);

        Assert.Null(harness.Engine.OwnerOf(1));
        Assert.Equal(TouchReleaseReason.GeometryInvalidated,
                     harness.Engine.Diagnostics().LastReleaseReason);
    }

    [Fact]
    public void NothingIsClaimedWhileTheLayoutDoesNotFit()
    {
        // Drawing overlapping controls in a window too small would send the console
        // input the user did not choose.
        var composed = TouchLayoutComposer.Compose(
            TouchProfileCatalog.Require(TouchProfileId.Pro2));
        var tiny = new TouchLayoutRegion(0f, 0f, 200f, 100f, 1f);
        var resolved = TouchLayoutResolver.Resolve(composed.Layout, tiny);
        Assert.False(resolved.Fits);

        var engine = new TouchControlEngine(_ => { });
        engine.InstallLayout(resolved);
        engine.OnContact(new TouchContact(1, TouchPhase.Down, 100f, 50f, 1_000_000L));

        Assert.Null(engine.OwnerOf(1));
        Assert.Equal(TouchContribution.Neutral, engine.Contribution);
    }
}

/// <summary>
/// Duplicate instances: two controls that send the same thing are still two separate
/// objects, and that is the whole of Editor 2.0's duplicate safety.
/// </summary>
public sealed class TouchDuplicateInstanceTests
{
    private static ResolvedTouchLayout WithTwoSouthButtons(out string first, out string second)
    {
        var profile = TouchProfileCatalog.Require(TouchProfileId.Pro2);
        var authored = TouchLayoutDocument.AuthoredDefault(profile);
        var original = authored.Controls.First(
            instance => instance.CatalogId == TouchLayoutV1.FaceSouth);

        first = original.InstanceId;
        second = original.InstanceId + "#2";

        // Placed in the empty lower-left corner so the audit stays quiet: this test is
        // about aggregation, not about overlap. The centre-bottom looks free and is not
        // — the two stick clicks live there.
        var duplicate = original with
        {
            InstanceId = second,
            AnchorX = 0.06f,
            AnchorY = 0.85f,
            OffsetXUnits = 0f,
            OffsetYUnits = 0f,
            GroupId = null,
            ZIndex = 999,
        };

        var document = authored with { Controls = [.. authored.Controls, duplicate] };
        var composed = TouchLayoutComposer.Compose(profile, document);
        var resolved = TouchLayoutResolver.Resolve(
            composed.Layout, new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));
        if (!resolved.Fits)
        {
            throw new InvalidOperationException($"duplicate fixture does not fit: {resolved.Problem}");
        }

        return resolved;
    }

    [Fact]
    public void ReleasingOneOfTwoInstancesLeavesTheBindingPressed()
    {
        // Keyed by BINDING, the second release would have taken the first one's press
        // with it, and the console would have seen a release edge the user never made.
        var layout = WithTwoSouthButtons(out var first, out var second);
        var engine = new TouchControlEngine(_ => { });
        engine.InstallLayout(layout);

        var a = layout.Control(first)!;
        var b = layout.Control(second)!;

        engine.OnContact(new TouchContact(1, TouchPhase.Down, a.CenterX, a.CenterY, 1_000_000L));
        engine.OnContact(new TouchContact(2, TouchPhase.Down, b.CenterX, b.CenterY, 2_000_000L));
        Assert.True(engine.Contribution.PositionalButtons.Contains(ControllerButton.A));

        engine.OnContact(new TouchContact(1, TouchPhase.Up, a.CenterX, a.CenterY, 3_000_000L));

        Assert.True(engine.Contribution.PositionalButtons.Contains(ControllerButton.A));

        engine.OnContact(new TouchContact(2, TouchPhase.Up, b.CenterX, b.CenterY, 4_000_000L));
        Assert.False(engine.Contribution.PositionalButtons.Contains(ControllerButton.A));
    }

    [Fact]
    public void TwoInstancesOfOneOutputMayOverlapWithoutFailingTheAudit()
    {
        // Whichever one a contact lands on produces the same thing, so there is no
        // ambiguity to report — and stacking duplicates deliberately is a reasonable way
        // to build a larger target out of two controls.
        var profile = TouchProfileCatalog.Require(TouchProfileId.Pro2);
        var authored = TouchLayoutDocument.AuthoredDefault(profile);
        var original = authored.Controls.First(
            instance => instance.CatalogId == TouchLayoutV1.FaceSouth);

        var document = authored with
        {
            Controls =
            [
                .. authored.Controls,
                original with { InstanceId = original.InstanceId + "#2", ZIndex = 999 },
            ],
        };

        var composed = TouchLayoutComposer.Compose(profile, document);
        var resolved = TouchLayoutResolver.Resolve(
            composed.Layout, new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));

        Assert.DoesNotContain(resolved.Findings, finding =>
            finding.Blocking && finding.Message.Contains("overlap", StringComparison.Ordinal));
    }
}

/// <summary>
/// The hold gesture. Timing alone cannot create a hold; a deliberate slide commits it.
/// </summary>
public sealed class TouchLatchTests
{
    private const long Ms = TouchHarness.Ms;

    /// <summary>The tap / press / dwell / slide that engages a hold.</summary>
    private static TouchHarness Engaged(out long committedAt)
    {
        var harness = new TouchHarness();
        var config = harness.Engine.Config;
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);

        // Tap one.
        harness.Send(new TouchContact(1, TouchPhase.Down, x, y, 100 * Ms));
        harness.Send(new TouchContact(1, TouchPhase.Up, x, y, 150 * Ms));

        // Press two, inside the double-tap window.
        var pressAt = 250 * Ms;
        harness.Send(new TouchContact(2, TouchPhase.Down, x, y, pressAt));

        // Dwell to arming.
        var armAt = pressAt + config.Latch.LatchEngageThresholdNanos;
        harness.Engine.OnTick(armAt);

        // And the slide that commits.
        var commit = config.Latch.LatchCommitDistanceUnits * harness.Region.UnitScale;
        committedAt = armAt + Ms;
        harness.Send(new TouchContact(2, TouchPhase.Move, x + commit + 4f, y, committedAt));

        return harness;
    }

    [Fact]
    public void APlainTapIsAnOrdinaryPressAndNeverAHold()
    {
        // Mashing IS a stream of double taps, so timing alone must not latch anything.
        var harness = new TouchHarness();
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);

        for (var index = 0; index < 6; index++)
        {
            var at = (100 + (index * 80)) * Ms;
            harness.Send(new TouchContact(index, TouchPhase.Down, x, y, at));
            harness.Send(new TouchContact(index, TouchPhase.Up, x, y, at + (30 * Ms)));
        }

        Assert.Empty(harness.Engine.LatchedControlIds());
        Assert.Equal(ControllerButtonSet.Empty, harness.Contribution.PositionalButtons);
    }

    [Fact]
    public void TheDwellOnlyArmsAndTheControlStaysAnOrdinaryHeldButton()
    {
        // The collision this avoids: "double tap, then keep holding" is something games
        // ask for directly, and no dwell separates it from an engage attempt.
        var harness = new TouchHarness();
        var config = harness.Engine.Config;
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);

        harness.Send(new TouchContact(1, TouchPhase.Down, x, y, 100 * Ms));
        harness.Send(new TouchContact(1, TouchPhase.Up, x, y, 150 * Ms));
        harness.Send(new TouchContact(2, TouchPhase.Down, x, y, 250 * Ms));
        harness.Engine.OnTick((250 * Ms) + config.Latch.LatchEngageThresholdNanos);

        Assert.Contains(TouchLayoutV1.FaceSouth, harness.Engine.ArmedControlIds());
        Assert.Empty(harness.Engine.LatchedControlIds());

        // Letting go now simply ends the press.
        harness.Send(new TouchContact(2, TouchPhase.Up, x, y, 500 * Ms));
        Assert.Empty(harness.Engine.LatchedControlIds());
        Assert.Equal(ControllerButtonSet.Empty, harness.Contribution.PositionalButtons);
    }

    [Fact]
    public void TheSlideCommitsAndTheControlStaysHeldAfterTheFingerLifts()
    {
        var harness = Engaged(out var committedAt);

        Assert.Contains(TouchLayoutV1.FaceSouth, harness.Engine.LatchedControlIds());

        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);
        harness.Send(new TouchContact(2, TouchPhase.Up, x, y, committedAt + (50 * Ms)));

        Assert.True(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));
        Assert.Contains(TouchLayoutV1.FaceSouth, harness.Engine.LatchedControlIds());
    }

    [Fact]
    public void SlidingBackToTheOriginTakesTheHoldOffWithoutAnyEdge()
    {
        // The finger is still down, so the control is still physically pressed and the
        // console sees no edge at all. Only the hold that would have outlived the finger
        // is gone.
        var harness = Engaged(out var committedAt);
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);
        var before = harness.Contribution;

        harness.Send(new TouchContact(2, TouchPhase.Move, x, y, committedAt + (10 * Ms)));

        Assert.Empty(harness.Engine.LatchedControlIds());
        Assert.Equal(before, harness.Contribution);

        // Straight back to ARMED, so an overshoot can simply slide out again.
        Assert.Contains(TouchLayoutV1.FaceSouth, harness.Engine.ArmedControlIds());
    }

    [Fact]
    public void APressAndHoldRemovesTheHold()
    {
        // Undoing something the user can see is wrong is deliberately the easier half:
        // no leading tap, no slide, and half the dwell.
        var harness = Engaged(out var committedAt);
        var config = harness.Engine.Config;
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);
        harness.Send(new TouchContact(2, TouchPhase.Up, x, y, committedAt + (50 * Ms)));

        var pressAt = committedAt + (500 * Ms);
        harness.Send(new TouchContact(3, TouchPhase.Down, x, y, pressAt));
        harness.Engine.OnTick(pressAt + config.Latch.LatchReleaseThresholdNanos);

        Assert.Empty(harness.Engine.LatchedControlIds());

        // The finger that performed the release gesture is still down and stays
        // authoritative: dropping the button at that instant would be a release edge
        // the user never made.
        Assert.True(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));

        harness.Send(new TouchContact(3, TouchPhase.Up, x, y, pressAt + (900 * Ms)));
        Assert.False(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));
    }

    [Fact]
    public void AQuickTapOnAHeldControlProducesAnObservableReleaseEdge()
    {
        // A latched control is already published as pressed, so a tap can only produce a
        // new press edge if a release is genuinely observable first.
        var harness = Engaged(out var committedAt);
        var config = harness.Engine.Config;
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);
        harness.Send(new TouchContact(2, TouchPhase.Up, x, y, committedAt + (50 * Ms)));

        var tapAt = committedAt + (400 * Ms);
        harness.Send(new TouchContact(4, TouchPhase.Down, x, y, tapAt));
        harness.Send(new TouchContact(4, TouchPhase.Up, x, y, tapAt + (30 * Ms)));

        Assert.False(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));
        Assert.Equal(1, harness.Engine.Diagnostics().RetriggerPulses);

        harness.Engine.OnTick(tapAt + (30 * Ms) + config.Latch.RetriggerReleaseNanos);
        Assert.True(harness.Contribution.PositionalButtons.Contains(ControllerButton.A));
    }

    [Fact]
    public void AHoldIsDroppedAtEveryBoundaryThatClearsInput()
    {
        // A hold nothing is touching is exactly the state that must not survive a
        // boundary.
        var harness = Engaged(out var committedAt);
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);
        harness.Send(new TouchContact(2, TouchPhase.Up, x, y, committedAt + (50 * Ms)));
        Assert.NotEmpty(harness.Engine.LatchedControlIds());

        harness.Engine.ReleaseAll(TouchReleaseReason.LinkEnded);

        Assert.Empty(harness.Engine.LatchedControlIds());
        Assert.Equal(TouchContribution.Neutral, harness.Contribution);
    }

    [Fact]
    public void TurningLatchingOffDropsWhateverIsCurrentlyHeld()
    {
        // A window where the setting says off while a control is still held would be the
        // exact confusion the setting exists to end.
        var harness = Engaged(out var committedAt);
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);
        harness.Send(new TouchContact(2, TouchPhase.Up, x, y, committedAt + (50 * Ms)));

        harness.Engine.SetConfig(harness.Engine.Config with
        {
            Latch = harness.Engine.Config.Latch with { EnabledByDefault = false },
        });

        Assert.Empty(harness.Engine.LatchedControlIds());
        Assert.Equal(TouchContribution.Neutral, harness.Contribution);
    }

    [Fact]
    public void AStickAndTheDpadCanNeverBeLatched()
    {
        // A latched direction the user cannot see themselves holding walks the character
        // into a wall. Excluded structurally so no stored document can ask for it.
        Assert.False(TouchControlKind.Stick.SupportsLatch());
        Assert.False(TouchControlKind.Dpad.SupportsLatch());
        Assert.True(TouchControlKind.Button.SupportsLatch());
        Assert.True(TouchControlKind.FaceButton.SupportsLatch());
        Assert.True(TouchControlKind.Trigger.SupportsLatch());
    }

    [Fact]
    public void ATickAfterATeardownDoesNothing()
    {
        // The whole of the retrigger's session-safety: there is no queued closure that
        // could carry stale state across a teardown.
        var harness = Engaged(out var committedAt);
        harness.Engine.ReleaseAll(TouchReleaseReason.Disposed);

        harness.Engine.OnTick(committedAt + (10_000 * Ms));

        Assert.Equal(TouchContribution.Neutral, harness.Contribution);
        Assert.Empty(harness.Engine.LatchedControlIds());
    }

    [Fact]
    public void AHostWithNoClockGetsNoLatchGestureRatherThanARandomOne()
    {
        // A recognizer running on a clock stuck at zero would toggle a persistent hold at
        // random, which is the worst failure this feature has.
        var harness = new TouchHarness();
        var (x, y) = harness.Centre(TouchLayoutV1.FaceSouth);

        for (var index = 0; index < 4; index++)
        {
            harness.Send(new TouchContact(index, TouchPhase.Down, x, y, 0L));
            harness.Send(new TouchContact(index, TouchPhase.Up, x, y, 0L));
        }

        Assert.Empty(harness.Engine.LatchedControlIds());
        Assert.Null(harness.Engine.NextDeadlineNanos());
    }
}

/// <summary>
/// The analog trigger. Only the GameCube personality has one, and full travel IS the
/// terminal click there.
/// </summary>
public sealed class TouchAnalogTriggerTests
{
    private const long Ms = TouchHarness.Ms;
    private const string TriggerL = "trigger-l";

    [Fact]
    public void OnlyTheGameCubeTriggersHaveRealTravel()
    {
        // Pro Controller 2 and Joy-Con triggers are digital on the far side however hard
        // they are pulled, so giving them a travel gesture would let a stray drag
        // silently send nothing at all.
        var gc = TouchProfileCatalog.Require(TouchProfileId.GameCube);
        Assert.True(((TouchControlAction.Trigger)gc.Bindings[TouchOutputControl.L]).Analog);
        Assert.True(((TouchControlAction.Trigger)gc.Bindings[TouchOutputControl.R]).Analog);

        foreach (var id in new[]
                 {
                     TouchProfileId.Pro2, TouchProfileId.JoyConLeft, TouchProfileId.JoyConRight,
                 })
        {
            var profile = TouchProfileCatalog.Require(id);
            foreach (var trigger in profile.Bindings.Values.OfType<TouchControlAction.Trigger>())
            {
                Assert.False(trigger.Analog, $"{id} should have no analog trigger");
            }
        }
    }

    [Fact]
    public void NothingIsPublishedOnTheWayDown()
    {
        // The single most damaging thing this control could do is assert a full pull the
        // instant it is touched, because full travel IS the terminal click here.
        var harness = new TouchHarness(TouchProfileId.GameCube);
        harness.Down(1, TriggerL, 100 * Ms);

        Assert.Equal(0, harness.Contribution.LeftTrigger);
        Assert.False(harness.Contribution.LogicalButtons.Contains(ControllerButton.L2));
    }

    [Fact]
    public void APullPublishesTheProjectionAndTheDetentOnlyAtTheEnd()
    {
        var harness = new TouchHarness(TouchProfileId.GameCube);
        var control = harness.Control(TriggerL);
        var config = harness.Engine.Config;
        harness.Down(1, TriggerL, 100 * Ms);

        var axis = TouchTriggerTravel.InwardAxis(
            control.CenterX, control.CenterY, harness.Region, config.Trigger.CenterEpsilonUnits);
        var full = TouchTriggerTravel.FullTravelPx(
            harness.Region, axis, config.Trigger.TravelFraction, config.Trigger.VerticalTravelRatio);

        // Half the travel: a real value, and NOT the click.
        harness.MoveTo(1,
            control.CenterX + (axis.X * full * 0.5f),
            control.CenterY + (axis.Y * full * 0.5f),
            110 * Ms);

        Assert.InRange(harness.Contribution.LeftTrigger, 100, 155);
        Assert.False(harness.Contribution.LogicalButtons.Contains(ControllerButton.L2));

        // All the way: the click, and the byte the firmware seam reads as clicked.
        harness.MoveTo(1,
            control.CenterX + (axis.X * full * 1.2f),
            control.CenterY + (axis.Y * full * 1.2f),
            120 * Ms);

        Assert.Equal(255, harness.Contribution.LeftTrigger);
        Assert.True(harness.Contribution.LogicalButtons.Contains(ControllerButton.L2));
    }

    [Fact]
    public void SubDetentTravelIsCappedBelowTheByteTheFirmwareReadsAsClicked()
    {
        // A hysteresis band on a local Boolean would be decorative: whatever value is
        // published IS the detent, because the seam derives the click from the byte.
        var harness = new TouchHarness(TouchProfileId.GameCube);
        var control = harness.Control(TriggerL);
        var config = harness.Engine.Config;
        harness.Down(1, TriggerL, 100 * Ms);

        var axis = TouchTriggerTravel.InwardAxis(
            control.CenterX, control.CenterY, harness.Region, config.Trigger.CenterEpsilonUnits);
        var full = TouchTriggerTravel.FullTravelPx(
            harness.Region, axis, config.Trigger.TravelFraction, config.Trigger.VerticalTravelRatio);

        // Just under the engage fraction: high, but it must not reach the click byte.
        harness.MoveTo(1,
            control.CenterX + (axis.X * full * 0.90f),
            control.CenterY + (axis.Y * full * 0.90f),
            110 * Ms);

        Assert.False(harness.Contribution.LogicalButtons.Contains(ControllerButton.L2));
        Assert.True(harness.Contribution.LeftTrigger <= (int)TouchTriggerConfig.SubDetentByte,
            $"published {harness.Contribution.LeftTrigger}, which the seam reads as clicked");
    }

    [Fact]
    public void ATapPublishesAFullPulseOnRelease()
    {
        // A late tap is worse input; a speculative click is WRONG input. And a press and
        // a release published in the same instant would collapse to no change at all.
        var harness = new TouchHarness(TouchProfileId.GameCube);
        var config = harness.Engine.Config;

        harness.Down(1, TriggerL, 100 * Ms);
        harness.Up(1, TriggerL, 120 * Ms);

        Assert.Equal(255, harness.Contribution.LeftTrigger);
        Assert.Equal(1, harness.Engine.Diagnostics().TriggerPulses);

        harness.Engine.OnTick((120 * Ms) + config.Latch.RetriggerReleaseNanos);
        Assert.Equal(0, harness.Contribution.LeftTrigger);
    }

    [Fact]
    public void AStillPressResolvesIntoADeliberateFullPull()
    {
        var harness = new TouchHarness(TouchProfileId.GameCube);
        var config = harness.Engine.Config;

        harness.Down(1, TriggerL, 100 * Ms);
        harness.Engine.OnTick((100 * Ms) + config.Latch.HoldThresholdNanos);

        Assert.Equal(255, harness.Contribution.LeftTrigger);
        Assert.True(harness.Contribution.LogicalButtons.Contains(ControllerButton.L2));
    }

    [Fact]
    public void TheTravelAxisPointsInwardFromWhereverTheControlIs()
    {
        // Encoding "L means drag down" would be correct exactly once — for the shipped
        // layout — and wrong for every user who moves the control.
        var region = new TouchLayoutRegion(0f, 0f, 800f, 400f, 1f);

        var topLeft = TouchTriggerTravel.InwardAxis(50f, 30f, region, 16f);
        Assert.True(topLeft.X > 0f && topLeft.Y > 0f);

        var topRight = TouchTriggerTravel.InwardAxis(750f, 30f, region, 16f);
        Assert.True(topRight.X < 0f && topRight.Y > 0f);

        var bottom = TouchTriggerTravel.InwardAxis(400f, 380f, region, 16f);
        Assert.True(bottom.Y < 0f);
    }

    [Fact]
    public void AControlParkedOnTheCentreGetsTheNearestEdgeNormalRatherThanNoise()
    {
        var region = new TouchLayoutRegion(0f, 0f, 800f, 400f, 1f);
        var axis = TouchTriggerTravel.InwardAxis(400f, 200f, region, 16f);

        Assert.Equal(1f, MathF.Sqrt((axis.X * axis.X) + (axis.Y * axis.Y)), 3);

        // Deterministic across repeated reads, which is what the fixed tie order buys.
        Assert.Equal(axis, TouchTriggerTravel.InwardAxis(400f, 200f, region, 16f));
    }

    [Fact]
    public void AFullPullNeverCostsMoreThanTheVerticalBudget()
    {
        // The measured defect the MIN replaced: a weighted blend charged the horizontal
        // budget for motion that never happened, and a straight-down stroke needed 96%
        // of the usable height.
        var region = new TouchLayoutRegion(0f, 0f, 1920f, 1025f, 1f);
        var config = new TouchTriggerConfig().Validated();
        var horizontal = MathF.Min(region.Width, region.Height) * config.TravelFraction;
        var vertical = horizontal * config.VerticalTravelRatio;

        foreach (var degrees in Enumerable.Range(0, 72).Select(step => step * 5f))
        {
            var radians = degrees * MathF.PI / 180f;
            var axis = new TouchVector(MathF.Cos(radians), MathF.Sin(radians));
            var travel = TouchTriggerTravel.FullTravelPx(
                region, axis, config.TravelFraction, config.VerticalTravelRatio);

            var dx = MathF.Abs(axis.X) * travel;
            var dy = MathF.Abs(axis.Y) * travel;

            Assert.True(dx <= horizontal + 0.01f, $"{degrees} deg cost {dx:0.0} px of width");
            Assert.True(dy <= vertical + 0.01f, $"{degrees} deg cost {dy:0.0} px of height");
        }
    }

    [Fact]
    public void TheFillFollowsTheSwipeRatherThanTheAxis()
    {
        // The shipped GameCube R has a diagonal axis: a straight DOWNWARD swipe projects
        // positively onto it and correctly increases travel, but filling from the axis
        // showed a bar growing leftward while the thumb moved down.
        Assert.Equal(TouchFillDirection.Down,
                     TouchTriggerTravel.FillDirection(new TouchVector(-0.2f, 1f)));
        Assert.Equal(TouchFillDirection.Left,
                     TouchTriggerTravel.FillDirection(new TouchVector(-1f, 0.2f)));

        // The vertical wins an exact tie, in a stated order, so a perfectly diagonal
        // input cannot pick a different answer twice running.
        Assert.Equal(TouchFillDirection.Down,
                     TouchTriggerTravel.FillDirection(new TouchVector(1f, 1f)));
    }

    [Fact]
    public void PerpendicularDriftCostsNothing()
    {
        // A thumb sweeps an arc rather than a line, which is what makes the invisible
        // axis usable at all.
        var axis = new TouchVector(0f, 1f);
        Assert.Equal(100f, TouchTriggerTravel.ProjectedTravelPx(60f, 100f, axis), 3);

        // And backing out returns toward rest rather than going negative.
        Assert.Equal(0f, TouchTriggerTravel.ProjectedTravelPx(0f, -100f, axis), 3);
    }
}
