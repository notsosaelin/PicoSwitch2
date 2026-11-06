using PicoSwitch.Bridge.Touch;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The editor operations. Every one is a pure total function from one document to the
/// next, which is what lets undo be a stack of DOCUMENTS rather than of invertible
/// commands.
/// </summary>
public sealed class TouchLayoutEditorTests
{
    private static TouchControllerProfile Pro2 => TouchProfileCatalog.Require(TouchProfileId.Pro2);

    private static TouchLayoutDocument Authored => TouchLayoutDocument.AuthoredDefault(Pro2);

    private static ResolvedTouchLayout Resolve(TouchLayoutDocument document)
    {
        var composed = TouchLayoutComposer.Compose(Pro2, document);
        return TouchLayoutResolver.Resolve(
            composed.Layout, new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));
    }

    private static HashSet<string> Select(params string[] ids) =>
        new(ids, StringComparer.Ordinal);

    [Fact]
    public void AddingAControlWhoseAuthoredSpotIsFreePutsItExactlyThere()
    {
        // A personality's authored position for its grips is a considered piece of layout
        // design, not an arbitrary starting point.
        var result = TouchLayoutEditor.Add(Authored, Pro2, TouchLayoutV1.GripLeft, 0.5f, 0.5f);

        Assert.True(result.Changed);
        var added = result.Document.Instance(Assert.Single(result.Created));
        var entry = Pro2.CatalogEntry(TouchLayoutV1.GripLeft)!;

        Assert.Equal(entry.Geometry.AnchorX, added!.AnchorX, 5);
        Assert.Equal(entry.Geometry.AnchorY, added.AnchorY, 5);
    }

    [Fact]
    public void AddingAControlOntoAnOccupiedSpotFallsBackToWhereTheUserAsked()
    {
        // The authored spot is taken, so the second copy goes to the fallback anchor
        // rather than landing exactly underneath its twin, where it would look like
        // nothing had happened.
        var once = TouchLayoutEditor.Add(Authored, Pro2, TouchLayoutV1.GripLeft, 0.5f, 0.5f);
        var twice = TouchLayoutEditor.Add(
            once.Document, Pro2, TouchLayoutV1.GripLeft, 0.5f, 0.5f);

        var second = twice.Document.Instance(twice.Created[0])!;
        Assert.Equal(0.5f, second.AnchorX, 5);
        Assert.Equal(0.5f, second.AnchorY, 5);

        // Nothing was near the fallback anchor, so no step was needed. The step is
        // occupancy-driven, not unconditional — a control dropped into empty space should
        // land where it was asked for.
        Assert.Equal(0f, second.OffsetXUnits);
        Assert.Null(second.GroupId);
    }

    [Fact]
    public void EachFurtherCopyStepsFurtherOutInsteadOfPilingUp()
    {
        var document = Authored;
        for (var index = 0; index < 2; index++)
        {
            document = TouchLayoutEditor
                .Add(document, Pro2, TouchLayoutV1.GripLeft, 0.5f, 0.5f).Document;
        }

        // Two are placed; the authored spot and the fallback anchor are both taken now, so
        // the third has to clear the one sitting at the fallback.
        var third = TouchLayoutEditor.Add(document, Pro2, TouchLayoutV1.GripLeft, 0.5f, 0.5f);
        var placed = third.Document.Instance(third.Created[0])!;

        Assert.Equal(TouchLayoutEditor.PlacementStepUnits, placed.OffsetXUnits);
        Assert.Equal(TouchLayoutEditor.PlacementStepUnits, placed.OffsetYUnits);
    }

    [Fact]
    public void AddingAnUnknownCatalogEntryIsRefusedWithAReason()
    {
        var result = TouchLayoutEditor.Add(Authored, Pro2, "no-such-thing", 0.5f, 0.5f);

        Assert.False(result.Changed);
        Assert.Contains("no 'no-such-thing'", result.Refusal, StringComparison.Ordinal);
    }

    [Fact]
    public void InstanceIdsAreReadableAndDeterministic()
    {
        // Derived rather than randomly generated, so every operation stays a pure function
        // and a stored document can be understood by a person looking at it.
        var first = TouchLayoutEditor.Duplicate(
            Authored, Select(TouchLayoutV1.Dpad), editGroup: false);
        Assert.Equal($"{TouchLayoutV1.Dpad}#2", first.Created[0]);

        var second = TouchLayoutEditor.Duplicate(
            first.Document, Select(TouchLayoutV1.Dpad), editGroup: false);
        Assert.Equal($"{TouchLayoutV1.Dpad}#3", second.Created[0]);
    }

    [Fact]
    public void DuplicatingAGroupProducesAWholeNewGroup()
    {
        // Copying half a cluster into the original's group would silently change what the
        // original group means.
        var result = TouchLayoutEditor.Duplicate(
            Authored, Select(TouchLayoutV1.FaceNorth), editGroup: true);

        Assert.Equal(4, result.Created.Count);
        var groups = result.Created
            .Select(id => result.Document.Instance(id)!.GroupId)
            .Distinct()
            .ToList();

        Assert.Single(groups);
        Assert.NotNull(groups[0]);
        Assert.NotEqual(
            Authored.Instance(TouchLayoutV1.FaceNorth)!.GroupId, groups[0]);
    }

    [Fact]
    public void DeletingIsRealRemovalRatherThanHiding()
    {
        // An absent instance does not exist, and Add Control is how one comes back.
        var result = TouchLayoutEditor.Delete(
            Authored, Select(TouchLayoutV1.Chat), editGroup: false);

        Assert.True(result.Changed);
        Assert.Null(result.Document.Instance(TouchLayoutV1.Chat));

        // …and the catalog still offers it, which is the whole mechanism.
        Assert.NotNull(Pro2.CatalogEntry(TouchLayoutV1.Chat));
    }

    [Fact]
    public void GroupingAndUngroupingAreExactlyReversibleAndTouchNoGeometry()
    {
        var grouped = TouchLayoutEditor.Group(
            Authored, Select(TouchLayoutV1.Minus, TouchLayoutV1.Plus));
        Assert.True(grouped.Changed);

        var ungrouped = TouchLayoutEditor.Ungroup(
            grouped.Document, Select(TouchLayoutV1.Minus));

        // Geometry is untouched throughout, which is what makes this lossless on any
        // window shape.
        foreach (var id in new[] { TouchLayoutV1.Minus, TouchLayoutV1.Plus })
        {
            var before = Authored.Instance(id)!;
            var after = ungrouped.Document.Instance(id)!;
            Assert.Equal(before.AnchorX, after.AnchorX);
            Assert.Equal(before.AnchorY, after.AnchorY);
            Assert.Equal(before.OffsetXUnits, after.OffsetXUnits);
            Assert.Equal(before.GroupId, after.GroupId);
        }
    }

    [Fact]
    public void GroupingNeedsTwoControls()
    {
        var result = TouchLayoutEditor.Group(Authored, Select(TouchLayoutV1.Minus));
        Assert.False(result.Changed);
        Assert.Contains("two or more", result.Refusal, StringComparison.Ordinal);
    }

    [Fact]
    public void AnEditOnOneMemberOfAClusterCanBeScopedToThatMemberAlone()
    {
        // What a surface highlights and what an edit actually moves must be the same set.
        var alone = TouchLayoutEditor.Expand(
            Authored, Select(TouchLayoutV1.FaceNorth), editGroup: false);
        Assert.Single(alone);

        var whole = TouchLayoutEditor.Expand(
            Authored, Select(TouchLayoutV1.FaceNorth), editGroup: true);
        Assert.Equal(4, whole.Count);
    }

    [Fact]
    public void AClusterKeepsItsInternalSpacingWhenDraggedIntoAnEdge()
    {
        // Clamping each member after the move would compress the cluster against the edge
        // and silently destroy the relative spacing that makes it a cluster.
        var resolved = Resolve(Authored);
        var selection = Select(TouchLayoutV1.FaceNorth);

        var before = TouchLayoutEditor.Expand(Authored, selection, editGroup: true)
            .ToDictionary(id => id, id => Authored.Instance(id)!);

        var moved = TouchLayoutEditor.Move(
            Authored, resolved, selection, 100_000f, 0f, editGroup: true);

        var deltas = before.Keys
            .Select(id => moved.Instance(id)!.AnchorX - before[id].AnchorX)
            .Distinct()
            .ToList();

        Assert.Single(deltas);
    }

    [Fact]
    public void ScalingAClusterMovesItsMembersApartAndStaysRigid()
    {
        var resolved = Resolve(Authored);
        var selection = Select(TouchLayoutV1.FaceNorth);
        var scaled = TouchLayoutEditor.ScaleBy(
            Authored, resolved, selection, 1.3f, editGroup: true);

        var members = TouchLayoutEditor.Expand(Authored, selection, editGroup: true);
        foreach (var id in members)
        {
            var before = Authored.Instance(id)!;
            var after = scaled.Instance(id)!;
            Assert.True(after.Scale > before.Scale);

            // The displacement went into the aspect-independent offset, not the anchor.
            Assert.Equal(before.AnchorX, after.AnchorX);
            Assert.Equal(before.AnchorY, after.AnchorY);
        }
    }

    [Fact]
    public void AClusterAtItsSizeLimitDoesNotTearItselfApart()
    {
        // Move the member by what actually happened, never by what was asked for.
        var resolved = Resolve(Authored);
        var selection = Select(TouchLayoutV1.FaceNorth);

        var atLimit = TouchLayoutEditor.SetScale(
            Authored, selection, TouchLayoutLimits.MaxScale, editGroup: true);
        var pushed = TouchLayoutEditor.ScaleBy(
            atLimit, Resolve(atLimit), selection, 2f, editGroup: true);

        foreach (var id in TouchLayoutEditor.Expand(atLimit, selection, editGroup: true))
        {
            var before = atLimit.Instance(id)!;
            var after = pushed.Instance(id)!;
            Assert.Equal(TouchLayoutLimits.MaxScale, after.Scale);
            Assert.Equal(before.OffsetXUnits, after.OffsetXUnits, 3);
            Assert.Equal(before.OffsetYUnits, after.OffsetYUnits, 3);
        }
    }

    [Fact]
    public void RotationIsPurelyPresentationalAndNeverChangesABinding()
    {
        var resolved = Resolve(Authored);
        var rotated = TouchLayoutEditor.RotateBy(
            Authored, resolved, Select(TouchLayoutV1.Dpad), 30f, editGroup: false);

        var composedBefore = TouchLayoutComposer.Compose(Pro2, Authored);
        var composedAfter = TouchLayoutComposer.Compose(Pro2, rotated);

        var before = composedBefore.Layout.Controls.First(c => c.Id == TouchLayoutV1.Dpad);
        var after = composedAfter.Layout.Controls.First(c => c.Id == TouchLayoutV1.Dpad);

        Assert.Equal(before.Action, after.Action);
        Assert.Equal(before.Output, after.Output);
        Assert.NotEqual(before.VisualRotationDegrees, after.VisualRotationDegrees);
    }

    [Fact]
    public void RotationSnapsMagneticallyToTheAuthoredAngleAndItsQuarterTurns()
    {
        // The user keeps every angle in between; only the useful ones pull.
        Assert.Equal(0f, TouchLayoutEditor.SnapRotation(3f));
        Assert.Equal(90f, TouchLayoutEditor.SnapRotation(87f));
        Assert.Equal(-90f, TouchLayoutEditor.SnapRotation(-93f));
        Assert.Equal(45f, TouchLayoutEditor.SnapRotation(45f));
    }

    [Fact]
    public void ASnappedControlCanStillBeTurnedOutOfItsSnapZone()
    {
        // THE defect the intent angle exists to prevent: deriving the target from
        // stored + delta means a control inside a snap zone can never leave it — each
        // frame proposes a fraction of a degree, the magnet pulls it back to the same
        // target, the stored angle never moves, and the next frame asks the identical
        // question. Turning a control then needed a whole-hand flick instead of a wrist.
        var document = TouchLayoutEditor.SetRotation(
            Authored, Select(TouchLayoutV1.Dpad), 0f);

        var intent = 0f;
        var applied = 0f;
        for (var frame = 0; frame < 40; frame++)
        {
            intent += 0.5f;
            applied += TouchLayoutEditor.SnappedRotationDelta(
                document, TouchLayoutV1.Dpad, 0.5f, intent);
            document = TouchLayoutEditor.SetRotation(
                Authored, Select(TouchLayoutV1.Dpad), applied);
        }

        // Twenty degrees of intent is well clear of the snap radius, so the control has
        // to have followed rather than staying pinned at zero.
        Assert.True(applied > TouchLayoutEditor.RotationSnapDegrees,
            $"rotation stayed pinned at {applied:0.0} degrees");
    }

    [Fact]
    public void ZOrderOperationsProduceADenseDeterministicSequence()
    {
        var front = TouchLayoutEditor.BringToFront(
            Authored, Select(TouchLayoutV1.Dpad), editGroup: false);

        var top = front.Controls.MaxBy(control => control.ZIndex)!;
        Assert.Equal(TouchLayoutV1.Dpad, top.InstanceId);

        var back = TouchLayoutEditor.SendToBack(
            front, Select(TouchLayoutV1.Dpad), editGroup: false);
        var bottom = back.Controls.MinBy(control => control.ZIndex)!;
        Assert.Equal(TouchLayoutV1.Dpad, bottom.InstanceId);

        // Dense: every index is used exactly once.
        Assert.Equal(
            Enumerable.Range(0, back.Controls.Count),
            back.Controls.Select(control => control.ZIndex).OrderBy(index => index));
    }

    [Fact]
    public void ALatchCannotBeStoredOnAControlThatCannotHold()
    {
        var document = TouchLayoutEditor.SetLatch(
            Authored, Pro2, Select(TouchLayoutV1.StickLeft), true, editGroup: false);

        Assert.Null(document.Instance(TouchLayoutV1.StickLeft)!.Latch);

        // …but a button takes one.
        var button = TouchLayoutEditor.SetLatch(
            Authored, Pro2, Select(TouchLayoutV1.FaceSouth), true, editGroup: false);
        Assert.True(button.Instance(TouchLayoutV1.FaceSouth)!.Latch);
    }

    [Fact]
    public void ResettingADuplicateKeepsItsOwnPositionRatherThanStackingIt()
    {
        // A duplicate has no authored position; resetting it to the original's place
        // would silently stack the two.
        var duplicated = TouchLayoutEditor.Duplicate(
            Authored, Select(TouchLayoutV1.Dpad), editGroup: false);
        var copyId = duplicated.Created[0];
        var before = duplicated.Document.Instance(copyId)!;

        var reset = TouchLayoutEditor.Reset(
            duplicated.Document, Pro2, Select(copyId), editGroup: false);

        Assert.Equal(before, reset.Instance(copyId));
    }

    [Fact]
    public void ResetAllIsAFreshCopyOfTheShippedLayout()
    {
        var moved = TouchLayoutEditor.Place(Authored, Select(TouchLayoutV1.Dpad), 0.9f, 0.9f);
        Assert.NotEqual(Authored, moved);

        Assert.Equal(Authored, TouchLayoutEditor.ResetAll(Pro2));
    }
}

/// <summary>Undo/redo, which is a stack of documents rather than of commands.</summary>
public sealed class TouchEditorHistoryTests
{
    private static TouchLayoutDocument Authored =>
        TouchLayoutDocument.AuthoredDefault(TouchProfileCatalog.Require(TouchProfileId.Pro2));

    [Fact]
    public void UndoAndRedoWalkTheRevisionStack()
    {
        var history = new TouchEditorHistory(Authored);
        var moved = TouchLayoutEditor.Place(
            Authored, new HashSet<string>(StringComparer.Ordinal) { TouchLayoutV1.Dpad },
            0.9f, 0.9f);

        history.Push(moved, "Move");
        Assert.True(history.CanUndo);
        Assert.Equal("Move", history.UndoLabel);

        Assert.Equal(Authored, history.Undo());
        Assert.False(history.CanUndo);
        Assert.True(history.CanRedo);

        Assert.Equal(moved, history.Redo());
    }

    [Fact]
    public void AGestureThatEndedWhereItStartedLeavesNoUndoStep()
    {
        // An undo step that appears to do nothing is worse than no step at all.
        var history = new TouchEditorHistory(Authored);
        history.Push(Authored, "Move");

        Assert.False(history.CanUndo);
    }

    [Fact]
    public void ANewEditDiscardsTheRedoBranch()
    {
        var history = new TouchEditorHistory(Authored);
        var selection = new HashSet<string>(StringComparer.Ordinal) { TouchLayoutV1.Dpad };

        history.Push(TouchLayoutEditor.Place(Authored, selection, 0.9f, 0.9f), "A");
        history.Undo();
        Assert.True(history.CanRedo);

        history.Push(TouchLayoutEditor.Place(Authored, selection, 0.1f, 0.1f), "B");
        Assert.False(history.CanRedo);
    }

    [Fact]
    public void TheStackIsBounded()
    {
        var history = new TouchEditorHistory(Authored, limit: 4);
        var selection = new HashSet<string>(StringComparer.Ordinal) { TouchLayoutV1.Dpad };

        for (var index = 0; index < 20; index++)
        {
            history.Push(
                TouchLayoutEditor.Place(Authored, selection, 0.1f + (index * 0.01f), 0.5f),
                $"step {index}");
        }

        var undos = 0;
        while (history.Undo() is not null)
        {
            undos++;
        }

        Assert.Equal(4, undos);
    }
}

/// <summary>Alignment assistance: guides assist, they never restrict.</summary>
public sealed class TouchEditorAlignmentTests
{
    private static ResolvedTouchLayout Layout
    {
        get
        {
            var profile = TouchProfileCatalog.Require(TouchProfileId.Pro2);
            var composed = TouchLayoutComposer.Compose(profile);
            return TouchLayoutResolver.Resolve(
                composed.Layout, new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));
        }
    }

    private static HashSet<string> Select(params string[] ids) =>
        new(ids, StringComparer.Ordinal);

    [Fact]
    public void SnappingIsOffUnlessItIsAskedFor()
    {
        var delta = new TouchEditorDelta(3f, 3f);
        Assert.Equal(delta, TouchEditorAlignment.Snap(
            Layout, Select(TouchLayoutV1.Dpad), TouchLayoutV1.Dpad, delta,
            TouchAlignmentSettings.Off));
    }

    [Fact]
    public void ALargeMovementAlwaysWinsOverTheNearestGuide()
    {
        // Guides assist. No position may become unreachable.
        var layout = Layout;
        var settings = new TouchAlignmentSettings { Snap = true };
        var far = new TouchEditorDelta(400f, 0f);

        var snapped = TouchEditorAlignment.Snap(
            layout, Select(TouchLayoutV1.Dpad), TouchLayoutV1.Dpad, far, settings);

        var tolerance = TouchEditorAlignment.SnapToleranceUnits * layout.Region.UnitScale;
        Assert.True(MathF.Abs(snapped.X - far.X) <= tolerance);
    }

    [Fact]
    public void AMultiControlSelectionKeepsItsSpacingBecauseOneReferenceDecides()
    {
        // Snapping the members individually would align each to a different guide and pull
        // the cluster apart.
        var layout = Layout;
        var settings = new TouchAlignmentSettings { Snap = true };
        var selection = Select(TouchLayoutV1.FaceNorth, TouchLayoutV1.FaceSouth);

        var snapped = TouchEditorAlignment.Snap(
            layout, selection, TouchLayoutV1.FaceNorth, new TouchEditorDelta(2f, 2f), settings);

        // One correction, applied whole. The assertion is simply that a single delta comes
        // back — the per-member alternative could not be expressed by this signature.
        Assert.True(float.IsFinite(snapped.X) && float.IsFinite(snapped.Y));
    }

    [Fact]
    public void TheGridIsAnchoredToTheRegionCentreSoItStaysSymmetric()
    {
        var region = new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f);
        var lines = TouchEditorAlignment.GridLines(
            region, new TouchAlignmentSettings { Grid = true });

        var verticals = lines.Where(line => line.Vertical).Select(line => line.Position).ToList();
        Assert.Contains(region.Width / 2f, verticals);
    }

    [Fact]
    public void TheGridIsEmptyWhenItIsNotAskedFor()
    {
        Assert.Empty(TouchEditorAlignment.GridLines(
            new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f), TouchAlignmentSettings.Off));
    }

    [Fact]
    public void OnlyMatchedGuidesAreReturnedForDrawing()
    {
        // A guide that appears when nothing is aligned is noise; one that never appears
        // when something is aligned makes the user check alignment by eye.
        var layout = Layout;
        var settings = new TouchAlignmentSettings { Snap = true };

        var matched = TouchEditorAlignment.MatchedGuides(
            layout, Select(TouchLayoutV1.Dpad), TouchLayoutV1.Dpad, settings);

        var tolerance = TouchEditorAlignment.MatchToleranceUnits * layout.Region.UnitScale;
        var dpad = layout.Control(TouchLayoutV1.Dpad)!;

        foreach (var line in matched)
        {
            var reference = line.Vertical ? dpad.CenterX : dpad.CenterY;
            Assert.True(MathF.Abs(line.Position - reference) <= tolerance);
        }
    }

    [Fact]
    public void ANonFiniteDeltaResolvesToNoMovementRatherThanNaNGeometry()
    {
        var snapped = TouchEditorAlignment.Snap(
            Layout, Select(TouchLayoutV1.Dpad), TouchLayoutV1.Dpad,
            new TouchEditorDelta(float.NaN, 0f), new TouchAlignmentSettings { Snap = true });

        Assert.Equal(TouchEditorDelta.Zero, snapped);
    }
}

/// <summary>
/// The profile library. Its whole reason for existing is that the factory profile cannot
/// be damaged.
/// </summary>
public sealed class TouchProfileLibraryTests
{
    private const long Now = 1_700_000_000_000L;

    private static TouchProfileLibrary Empty =>
        TouchProfileLibrary.Empty(TouchProfileId.Pro2);

    private static TouchProfileLibrary Applied(TouchProfileEdit edit) =>
        Assert.IsType<TouchProfileEdit.Applied>(edit).Library;

    [Fact]
    public void TheFactoryProfileIsSynthesizedAndAlwaysPresent()
    {
        // Never persisted, so a corrupt stored document degrades to the shipped controller
        // rather than to a controller with no layout at all.
        var library = Empty;

        Assert.Equal(TouchProfileLibrary.FactoryProfileId, library.Selected.Id);
        Assert.True(library.Selected.IsFactory);
        Assert.True(library.Selected.IsPristine);
        Assert.Empty(library.UserProfiles);
    }

    [Fact]
    public void TheFactoryProfileCannotBeRenamedOrDeleted()
    {
        var library = Empty;

        Assert.IsType<TouchProfileEdit.Rejected>(
            TouchProfileLibraryEditor.Rename(library, TouchProfileLibrary.FactoryProfileId, "Mine"));
        Assert.IsType<TouchProfileEdit.Rejected>(
            TouchProfileLibraryEditor.Delete(library, TouchProfileLibrary.FactoryProfileId));
    }

    [Fact]
    public void SavingOntoTheFactoryProfileCreatesANewOneInstead()
    {
        // Refusing outright would discard work the user just did; overwriting would destroy
        // the one layout that is always supposed to be recoverable.
        var library = Empty;
        var edited = TouchLayoutEditor.Place(
            library.FactoryProfile.Document,
            new HashSet<string>(StringComparer.Ordinal) { TouchLayoutV1.Dpad }, 0.9f, 0.9f);

        var saved = Applied(TouchProfileLibraryEditor.Save(
            library, TouchProfileLibrary.FactoryProfileId, edited, Now));

        Assert.Single(saved.UserProfiles);
        Assert.NotEqual(TouchProfileLibrary.FactoryProfileId, saved.SelectedProfileId);
        Assert.True(saved.FactoryProfile.IsPristine);
    }

    [Fact]
    public void DeletingTheActiveProfileLandsOnTheFactoryOne()
    {
        // The only place that certainly exists.
        var library = Applied(TouchProfileLibraryEditor.Create(Empty, "Mine", Now));
        var id = library.SelectedProfileId;

        var after = Applied(TouchProfileLibraryEditor.Delete(library, id));

        Assert.Equal(TouchProfileLibrary.FactoryProfileId, after.SelectedProfileId);
        Assert.Empty(after.UserProfiles);
    }

    [Fact]
    public void NamesAreUniqueAndTheFactoryNameIsReserved()
    {
        // A second "Default" in the picker would make the protected profile
        // unidentifiable.
        var library = Applied(TouchProfileLibraryEditor.Create(
            Empty, TouchProfileLibrary.FactoryProfileName, Now));

        Assert.NotEqual(TouchProfileLibrary.FactoryProfileName, library.UserProfiles[0].Name);

        var second = Applied(TouchProfileLibraryEditor.Create(library, "Mine", Now + 1));
        var third = Applied(TouchProfileLibraryEditor.Create(second, "Mine", Now + 2));

        Assert.Equal(3, third.UserProfiles.Count);
        Assert.Equal(3, third.UserProfiles.Select(p => p.Name).Distinct().Count());
    }

    [Fact]
    public void NamesAreSanitizedAndBounded()
    {
        var library = Applied(TouchProfileLibraryEditor.Create(
            Empty, new string('x', 200) + "<>", Now));

        var name = library.UserProfiles[0].Name;
        Assert.True(name.Length <= TouchProfileLibrary.MaxNameLength);
        Assert.DoesNotContain('<', name);

        // A name that sanitizes to nothing still produces a usable profile.
        var blank = Applied(TouchProfileLibraryEditor.Create(library, "   ", Now + 1));
        Assert.False(string.IsNullOrWhiteSpace(blank.UserProfiles[1].Name));
    }

    [Fact]
    public void TheProfileCountIsBounded()
    {
        var library = Empty;
        for (var index = 0; index < TouchProfileLibrary.MaxUserProfiles; index++)
        {
            library = Applied(TouchProfileLibraryEditor.Create(library, $"P{index}", Now + index));
        }

        var refused = Assert.IsType<TouchProfileEdit.Rejected>(
            TouchProfileLibraryEditor.Create(library, "one too many", Now + 99));
        Assert.Contains("already has", refused.Reason, StringComparison.Ordinal);
    }

    [Fact]
    public void IdsAreDerivedAndCollisionsResolve()
    {
        // Two profiles created inside the same millisecond still get distinct identities.
        var first = Applied(TouchProfileLibraryEditor.Create(Empty, "A", Now));
        var second = Applied(TouchProfileLibraryEditor.Create(first, "B", Now));

        Assert.NotEqual(second.UserProfiles[0].Id, second.UserProfiles[1].Id);
    }

    [Fact]
    public void ImportingALayoutForAnotherControllerIsRefused()
    {
        var foreign = new TouchLayoutProfile(
            "x", "Theirs",
            TouchLayoutDocument.AuthoredDefault(
                TouchProfileCatalog.Require(TouchProfileId.GameCube)));

        var refused = Assert.IsType<TouchProfileEdit.Rejected>(
            TouchProfileLibraryEditor.Import(Empty, foreign, Now));
        Assert.Contains("another controller", refused.Reason, StringComparison.Ordinal);
    }

    [Fact]
    public void ALegacyOverrideBecomesANormalProfileAndIsSelected()
    {
        // Discarding it on upgrade would silently throw away every layout anybody had
        // already tuned.
        var @override = new TouchLayoutOverride
        {
            ProfileId = TouchProfileId.Pro2,
            TemplateId = TouchLayoutV1.Id,
            BasedOnRevision = TouchLayoutV1.TemplateRevision,
            Controls = new Dictionary<string, TouchControlOverride>(StringComparer.Ordinal)
            {
                [TouchLayoutV1.Dpad] = new() { AnchorX = 0.3f },
            },
        };

        var library = TouchProfileLibraryEditor.AdoptLegacyOverride(
            TouchProfileId.Pro2, @override, Now);

        Assert.Single(library.UserProfiles);
        Assert.Equal(library.UserProfiles[0].Id, library.SelectedProfileId);
    }

    [Fact]
    public void ALegacyOverrideThatSaidNothingIsNotWorthAProfile()
    {
        var library = TouchProfileLibraryEditor.AdoptLegacyOverride(
            TouchProfileId.Pro2,
            new TouchLayoutOverride
            {
                ProfileId = TouchProfileId.Pro2,
                TemplateId = TouchLayoutV1.Id,
                BasedOnRevision = TouchLayoutV1.TemplateRevision,
            },
            Now);

        Assert.Empty(library.UserProfiles);
    }
}

/// <summary>The persisted profile-library document, and the single exported profile.</summary>
public sealed class TouchProfileLibraryCodecTests
{
    private const long Now = 1_700_000_000_000L;

    private static TouchProfileLibrary Sample
    {
        get
        {
            var library = TouchProfileLibrary.Empty(TouchProfileId.Pro2);
            var created = (TouchProfileEdit.Applied)TouchProfileLibraryEditor.Create(
                library, "Racing", Now);

            var edited = TouchLayoutEditor.Place(
                created.Library.UserProfiles[0].Document,
                new HashSet<string>(StringComparer.Ordinal) { TouchLayoutV1.Dpad }, 0.35f, 0.7f);

            return ((TouchProfileEdit.Applied)TouchProfileLibraryEditor.Save(
                created.Library, created.ProfileId, edited, Now)).Library;
        }
    }

    [Fact]
    public void ARoundTripIsLossless()
    {
        var decoded = TouchProfileLibraryJsonCodec.Decode(
            TouchProfileLibraryJsonCodec.Encode(Sample), TouchProfileId.Pro2);

        var valid = Assert.IsType<TouchProfileLibraryDecodeResult.Valid>(decoded);
        Assert.False(valid.Migrated);
        Assert.Equal(Sample, valid.Value);
    }

    [Fact]
    public void TheFactoryProfileIsNeverWritten()
    {
        // Which is what makes it impossible for stored data to overwrite, rename or delete
        // it.
        var json = TouchProfileLibraryJsonCodec.Encode(Sample);
        Assert.DoesNotContain(TouchProfileLibrary.FactoryProfileId, json, StringComparison.Ordinal);
    }

    [Fact]
    public void AStoredProfileClaimingTheReservedIdIsRefused()
    {
        var json = TouchProfileLibraryJsonCodec.Encode(Sample)
            .Replace(Sample.UserProfiles[0].Id, TouchProfileLibrary.FactoryProfileId,
                     StringComparison.Ordinal);

        var invalid = Assert.IsType<TouchProfileLibraryDecodeResult.Invalid>(
            TouchProfileLibraryJsonCodec.Decode(json, TouchProfileId.Pro2));
        Assert.Contains("reserved default id", invalid.Problem, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("not json", "not valid JSON")]
    [InlineData("""{"personality":"pro2"}""", "no schema version")]
    [InlineData("""{"schemaVersion":99,"personality":"pro2"}""", "newer app")]
    [InlineData("""{"schemaVersion":2,"personality":"gc","profiles":[]}""", "another controller")]
    public void ADamagedDocumentIsReportedAndNeverGuessedAt(string raw, string expected)
    {
        var invalid = Assert.IsType<TouchProfileLibraryDecodeResult.Invalid>(
            TouchProfileLibraryJsonCodec.Decode(raw, TouchProfileId.Pro2));

        Assert.Contains(expected, invalid.Problem, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ASelectionNamingAMissingProfileFallsBackWithoutFailingTheDocument()
    {
        // The layouts themselves are still perfectly usable.
        var json = TouchProfileLibraryJsonCodec.Encode(Sample)
            .Replace($"\"selectedProfileId\":\"{Sample.SelectedProfileId}\"",
                     "\"selectedProfileId\":\"gone\"", StringComparison.Ordinal);

        var valid = Assert.IsType<TouchProfileLibraryDecodeResult.Valid>(
            TouchProfileLibraryJsonCodec.Decode(json, TouchProfileId.Pro2));

        Assert.Equal(TouchProfileLibrary.FactoryProfileId, valid.Value.SelectedProfileId);
        Assert.Single(valid.Value.UserProfiles);
    }

    [Fact]
    public void AnExportedProfileRoundTripsAndIsRecognisedByItsKind()
    {
        var exported = TouchProfileLibraryJsonCodec.EncodeExport(Sample.UserProfiles[0]);
        var valid = Assert.IsType<TouchProfileDecodeResult.Valid>(
            TouchProfileLibraryJsonCodec.DecodeExport(exported));

        Assert.Equal(Sample.UserProfiles[0].Document, valid.Value.Document);
        Assert.Equal(Sample.UserProfiles[0].Name, valid.Value.Name);
    }

    [Fact]
    public void AnUnrelatedJsonFileIsRefusedEarly()
    {
        var invalid = Assert.IsType<TouchProfileDecodeResult.Invalid>(
            TouchProfileLibraryJsonCodec.DecodeExport(
                """{"schemaVersion":2,"personality":"pro2","name":"x"}"""));

        Assert.Contains("not an exported touch layout", invalid.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void AnOutOfRangeStoredInstanceIsRefusedRatherThanClamped()
    {
        // The codec is the same gate an editor operation passes, so a hand-edited file
        // cannot construct geometry the editor refuses to make.
        var json = TouchProfileLibraryJsonCodec.Encode(Sample)
            .Replace("\"anchorX\":0.35", "\"anchorX\":7.5", StringComparison.Ordinal);

        var invalid = Assert.IsType<TouchProfileLibraryDecodeResult.Invalid>(
            TouchProfileLibraryJsonCodec.Decode(json, TouchProfileId.Pro2));

        Assert.Contains("out-of-range anchor", invalid.Problem, StringComparison.Ordinal);
    }

    [Fact]
    public void ASchemaOneDocumentIsMigratedOnTheWayIn()
    {
        // Version 1 wrote controls as an OBJECT keyed by template control id; version 2
        // writes an ARRAY of instances. The distinction is visible in the JSON itself,
        // which is what makes a half-migrated document impossible.
        var legacy =
            "{\"schemaVersion\":1,\"personality\":\"pro2\",\"selectedProfileId\":\"p1\"," +
            "\"profiles\":[{\"id\":\"p1\",\"name\":\"Old\"," +
            $"\"templateId\":\"{TouchLayoutV1.Id}\"," +
            $"\"templateRevision\":{TouchLayoutV1.TemplateRevision}," +
            "\"controls\":{" +
            $"\"{TouchLayoutV1.Dpad}\":{{\"anchorX\":0.3}}," +
            $"\"{TouchLayoutV1.Chat}\":{{\"visible\":false}}" +
            "}}]}";

        var valid = Assert.IsType<TouchProfileLibraryDecodeResult.Valid>(
            TouchProfileLibraryJsonCodec.Decode(legacy, TouchProfileId.Pro2));

        Assert.True(valid.Migrated);
        var document = valid.Value.UserProfiles[0].Document;
        Assert.Equal(TouchLayoutDocument.CurrentSchemaVersion, document.SchemaVersion);
        Assert.Equal(0.3f, document.Instance(TouchLayoutV1.Dpad)!.AnchorX, 4);
        Assert.Null(document.Instance(TouchLayoutV1.Chat));
    }
}

/// <summary>The editor toolbar: every rule here is a rule about reachability.</summary>
public sealed class TouchToolbarPlacementTests
{
    private static readonly TouchLayoutRegion Region = new(40f, 30f, 1240f, 730f, 2f);

    [Fact]
    public void ADockedToolbarFollowsTheINTERACTIONSafeEdgeAndNotTheWindow()
    {
        // A host aligning to the window would put a docked toolbar under the system
        // gesture strip — the same mistake the layout resolver exists to prevent.
        var (_, y) = TouchToolbarLayout.TopLeft(
            new TouchToolbarPlacement.Docked(TouchToolbarEdge.Top), 300f, 60f, Region);

        Assert.Equal(Region.Top, y);
    }

    [Fact]
    public void AFloatingPositionIsNormalizedSoItLandsInTheSamePlaceAtADifferentSize()
    {
        var placement = TouchToolbarLayout.PlacementFor(600f, 400f, 300f, 60f, Region);
        var floating = Assert.IsType<TouchToolbarPlacement.Floating>(placement);

        Assert.InRange(floating.X, 0f, 1f);
        Assert.InRange(floating.Y, 0f, 1f);
    }

    [Fact]
    public void ANearEdgeDragOffersADock()
    {
        var edge = TouchToolbarLayout.DockCandidate(
            Region.Left + 4f, 400f, 300f, 60f, Region);

        Assert.Equal(TouchToolbarEdge.Left, edge);
    }

    [Fact]
    public void ThePreviewAndTheResultCannotDisagree()
    {
        // One function, so the surface highlights whatever it commits.
        var x = Region.Left + 4f;
        var placement = TouchToolbarLayout.PlacementFor(x, 400f, 300f, 60f, Region);
        var candidate = TouchToolbarLayout.DockCandidate(x, 400f, 300f, 60f, Region);

        Assert.Equal(new TouchToolbarPlacement.Docked(candidate!.Value), placement);
    }

    [Fact]
    public void TheLeadingCornerIsAlwaysInsideTheRegion()
    {
        // THE invariant. A right-docked toolbar whose leading edge has gone off-screen has
        // taken the drag handle with it, and there is then no gesture that brings it back
        // and no Done button to leave by.
        foreach (var edge in Enum.GetValues<TouchToolbarEdge>())
        {
            var (x, y) = TouchToolbarLayout.TopLeft(
                new TouchToolbarPlacement.Docked(edge), 9_000f, 9_000f, Region);

            Assert.InRange(x, Region.Left, Region.Right);
            Assert.InRange(y, Region.Top, Region.Bottom);
        }
    }

    [Fact]
    public void ANonFinitePositionFallsBackToTheDefaultDock()
    {
        // A toolbar at NaN draws nowhere and cannot be dragged back.
        var clamped = TouchToolbarLayout.Clamp(
            new TouchToolbarPlacement.Floating(float.NaN, 0.5f), 300f, 60f, Region);

        Assert.Equal(TouchToolbarPlacement.Default, clamped);
    }

    [Fact]
    public void AnEdgeKeyRoundTrips()
    {
        foreach (var edge in Enum.GetValues<TouchToolbarEdge>())
        {
            Assert.Equal(edge, TouchToolbarEdges.FromKey(edge.Key()));
        }

        Assert.Null(TouchToolbarEdges.FromKey("nowhere"));
    }
}
