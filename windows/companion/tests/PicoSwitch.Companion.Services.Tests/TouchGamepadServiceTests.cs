using PicoSwitch.Bridge.Touch;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The Touch Gamepad service over REAL document stores in a temporary directory.
///
/// Real files rather than an in-memory fake, for the same reason the adapter registry
/// fixture uses them: Phase 6a's exit criterion is that a profile survives a restart,
/// and a fake filesystem proves nothing about that.
/// </summary>
public sealed class TouchGamepadFixture : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(), "picoswitch-touch-" + Guid.NewGuid().ToString("N"));

    public TouchGamepadFixture()
    {
        Documents = new WindowsDocumentStore(root);
        Diagnostics = new DiagnosticLog();
        Profiles = new WindowsTouchProfileStore(Documents);
        Legacy = new WindowsTouchOverrideStore(Documents);
    }

    public WindowsDocumentStore Documents { get; }

    public DiagnosticLog Diagnostics { get; }

    public WindowsTouchProfileStore Profiles { get; }

    public WindowsTouchOverrideStore Legacy { get; }

    public long Clock { get; set; } = 1_700_000_000_000L;

    /// <summary>
    /// A service over this fixture's stores.
    ///
    /// Built on demand so a test can construct a SECOND one against the same directory,
    /// which is what "restart the app" means here.
    /// </summary>
    public TouchGamepadService NewService()
    {
        var service = new TouchGamepadService(Profiles, Legacy, Diagnostics, () => Clock);
        service.SetPersonality(Personality.Pro2);

        // 800 x 420 logical units — deliberately outside the documented GameCube 2:1
        // collision band, so a layout that refuses to fit is a real finding.
        service.SetRegion(new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));
        return service;
    }

    public void Dispose()
    {
        try
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
        catch (IOException)
        {
            // A leaked temp directory is not worth failing a test run over.
        }
    }
}

public sealed class TouchGamepadServiceTests
{
    private static IReadOnlySet<string> Select(params string[] ids) =>
        new HashSet<string>(ids, StringComparer.Ordinal);

    [Fact]
    public void NothingIsDrawnUntilAPersonalityIsConfirmed()
    {
        // Guessing would show the user a layout for hardware they are not emulating.
        using var fixture = new TouchGamepadFixture();
        var service = new TouchGamepadService(
            fixture.Profiles, fixture.Legacy, fixture.Diagnostics, () => fixture.Clock);

        Assert.Null(service.State.Value.Personality);
        Assert.False(service.State.Value.Editable);

        service.SetPersonality(Personality.Unknown);
        Assert.Null(service.State.Value.Personality);
    }

    [Fact]
    public void AConfirmedPersonalityGivesTheShippedLayoutWithNoAdapterAttached()
    {
        // The whole point of Phase 6a being ungated: this works with nothing paired.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();
        var state = service.State.Value;

        Assert.Equal(TouchProfileId.Pro2, state.Personality);
        Assert.True(state.Editable);
        Assert.True(state.Resolved.Fits);
        Assert.NotEmpty(state.Resolved.Controls);
        Assert.Equal(TouchProfileLibrary.FactoryProfileId, state.Library.SelectedProfileId);
        Assert.Null(state.Warning);
    }

    [Fact]
    public void ChangingPersonalityReloadsAndDoesNotCarryTheOtherLayoutOver()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();
        service.SetRegion(new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));

        service.SetPersonality(Personality.GameCube);
        service.SetRegion(new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));

        var state = service.State.Value;
        Assert.Equal(TouchProfileId.GameCube, state.Personality);
        Assert.Equal(TouchProfileId.GameCube, state.Document.ProfileId);
        Assert.False(state.CanUndo);
    }

    [Fact]
    public void AWindowTooSmallRefusesRatherThanOfferingARepairThatCannotWork()
    {
        // No EDIT makes the window bigger.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.SetRegion(new TouchLayoutRegion(0f, 0f, 200f, 100f, 1f));

        var state = service.State.Value;
        Assert.True(state.Resolved.RegionTooSmall);
        Assert.False(state.Editable);
        Assert.False(string.IsNullOrWhiteSpace(state.Warning));
    }

    [Fact]
    public void AGeometryChangeReResolvesRatherThanScalingWhatWasAlreadyResolved()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();
        var before = service.State.Value.Resolved.Control(TouchLayoutV1.Dpad)!.CenterX;

        service.SetRegion(new TouchLayoutRegion(0f, 0f, 2400f, 1000f, 2f));
        var after = service.State.Value.Resolved.Control(TouchLayoutV1.Dpad)!.CenterX;

        Assert.NotEqual(before, after);
        Assert.True(service.State.Value.Resolved.Fits);
    }

    [Fact]
    public void AnEditIsOneUndoStepAndTheCanvasFollowsIt()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.Edit("Move", document =>
            TouchLayoutEditor.Place(document, Select(TouchLayoutV1.Dpad), 0.30f, 0.72f));

        Assert.True(service.State.Value.Dirty);
        Assert.True(service.State.Value.CanUndo);
        Assert.Equal(0.30f, service.State.Value.Document.Instance(TouchLayoutV1.Dpad)!.AnchorX, 4);

        service.Undo();
        Assert.False(service.State.Value.Dirty);
        Assert.True(service.State.Value.CanRedo);
    }

    [Fact]
    public void APreviewFollowsTheFingerWithoutFillingTheUndoStack()
    {
        // The working document is authoritative during a drag; only the endpoints are
        // worth remembering.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        for (var frame = 0; frame < 30; frame++)
        {
            service.Preview(TouchLayoutEditor.Place(
                service.State.Value.Document, Select(TouchLayoutV1.Dpad),
                0.3f + (frame * 0.001f), 0.7f));
        }

        Assert.False(service.State.Value.CanUndo);
        Assert.True(service.State.Value.Dirty);
    }

    [Fact]
    public void ARefusedOperationChangesNothingIncludingTheUndoStack()
    {
        // An undo step for an operation that did not happen is a step that appears to do
        // nothing.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        var refusal = service.Edit("Group", document =>
            TouchLayoutEditor.Group(document, Select(TouchLayoutV1.Dpad)));

        Assert.NotNull(refusal);
        Assert.False(service.State.Value.CanUndo);
        Assert.False(service.State.Value.Dirty);
        Assert.Equal(refusal, service.State.Value.Warning);
    }

    [Fact]
    public void AnAddedControlIsSelectedSoTheSurfaceCanActOnItImmediately()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.Edit("Add", document =>
            TouchLayoutEditor.Add(document, TouchProfileCatalog.Require(TouchProfileId.Pro2),
                                  TouchLayoutV1.GripLeft, 0.5f, 0.5f));

        Assert.Equal(TouchLayoutV1.GripLeft, Assert.Single(service.State.Value.Selection));
    }

    [Fact]
    public void AnAuditFindingIsReportedWithoutBlockingTheEditor()
    {
        // A layout can be perfectly well formed and still have two controls on top of each
        // other; the user has to be able to see that and fix it.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.Edit("Move", document =>
            TouchLayoutEditor.Place(document, Select(TouchLayoutV1.Dpad),
                service.State.Value.Document.Instance(TouchLayoutV1.StickLeft)!.AnchorX,
                service.State.Value.Document.Instance(TouchLayoutV1.StickLeft)!.AnchorY));

        var state = service.State.Value;
        Assert.False(state.Resolved.Fits);
        Assert.NotEmpty(state.Resolved.InvalidControlIds);
        Assert.False(string.IsNullOrWhiteSpace(state.Warning));

        // Still editable, so the user can drag it back off.
        Assert.True(state.Editable);
        Assert.True(state.CanUndo);
    }

    // ------------------------------------------------------------------ persistence

    [Fact]
    public void AProfileSurvivesARestart()
    {
        // Phase 6a's exit criterion, end to end: create, edit, save, "restart", find it.
        using var fixture = new TouchGamepadFixture();

        var first = fixture.NewService();
        first.CreateProfile("Racing");

        // A nudge from wherever the template authored it, rather than an absolute
        // coordinate: the saved layout is asserted to still FIT after the restart, and a
        // guessed anchor that happened to land on the stick would fail that for a reason
        // having nothing to do with persistence.
        var authored = first.State.Value.Document.Instance(TouchLayoutV1.Dpad)!;
        var movedX = authored.AnchorX + 0.02f;
        first.Edit("Move", document =>
            TouchLayoutEditor.Place(
                document, Select(TouchLayoutV1.Dpad), movedX, authored.AnchorY));
        first.Save();

        Assert.False(first.State.Value.Dirty);

        var second = fixture.NewService();
        var state = second.State.Value;

        Assert.Single(state.Library.UserProfiles);
        Assert.Equal("Racing", state.Library.Selected.Name);
        Assert.Equal(movedX, state.Document.Instance(TouchLayoutV1.Dpad)!.AnchorX, 4);
        Assert.True(state.Resolved.Fits);
    }

    [Fact]
    public void TheFactoryProfileCannotBeOverwrittenByAnyEditorPath()
    {
        // Saving onto it creates a new profile instead, so the one layout that is always
        // recoverable stays recoverable.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        Assert.Equal(TouchProfileLibrary.FactoryProfileId,
                     service.State.Value.Library.SelectedProfileId);

        service.Edit("Move", document =>
            TouchLayoutEditor.Place(document, Select(TouchLayoutV1.Dpad), 0.9f, 0.9f));
        service.Save();

        var state = service.State.Value;
        Assert.Single(state.Library.UserProfiles);
        Assert.NotEqual(TouchProfileLibrary.FactoryProfileId, state.Library.SelectedProfileId);
        Assert.True(state.Library.FactoryProfile.IsPristine);

        // And it is still pristine after a restart.
        Assert.True(fixture.NewService().State.Value.Library.FactoryProfile.IsPristine);
    }

    [Fact]
    public void DiscardingGoesBackToWhatIsStored()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();
        service.CreateProfile("Mine");
        service.Save();

        service.Edit("Move", document =>
            TouchLayoutEditor.Place(document, Select(TouchLayoutV1.Dpad), 0.9f, 0.9f));
        Assert.True(service.State.Value.Dirty);

        service.Discard();

        Assert.False(service.State.Value.Dirty);
        Assert.False(service.State.Value.CanUndo);
    }

    [Fact]
    public void EachPersonalityKeepsItsOwnDocument()
    {
        // One file per personality, so a damaged GameCube document costs nothing on Pro
        // Controller 2.
        using var fixture = new TouchGamepadFixture();

        var pro2 = fixture.NewService();
        pro2.CreateProfile("Pro2 layout");
        pro2.Save();

        var gc = new TouchGamepadService(
            fixture.Profiles, fixture.Legacy, fixture.Diagnostics, () => fixture.Clock);
        gc.SetPersonality(Personality.GameCube);
        gc.SetRegion(new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));

        Assert.Empty(gc.State.Value.Library.UserProfiles);

        Assert.NotNull(fixture.Documents.Read(
            WindowsTouchProfileStore.DocumentName(TouchProfileId.Pro2)));
        Assert.Null(fixture.Documents.Read(
            WindowsTouchProfileStore.DocumentName(TouchProfileId.GameCube)));
    }

    [Fact]
    public void ADamagedDocumentIsReportedAndNeverDeleted()
    {
        // A later build may understand it, and the user may have hand-edited it. The
        // runtime is safe regardless because the factory profile needs nothing from
        // storage.
        using var fixture = new TouchGamepadFixture();
        var name = WindowsTouchProfileStore.DocumentName(TouchProfileId.Pro2);
        fixture.Documents.Write(name, "{ this is not a layout }");

        var service = fixture.NewService();
        var state = service.State.Value;

        Assert.False(string.IsNullOrWhiteSpace(state.Warning));
        Assert.Contains("left untouched", state.Warning, StringComparison.Ordinal);

        // The controller still works…
        Assert.True(state.Resolved.Fits);
        Assert.True(state.Library.FactoryProfile.IsPristine);

        // …and the file is exactly as it was.
        Assert.Equal("{ this is not a layout }", fixture.Documents.Read(name));
    }

    [Fact]
    public void APreTwoPointZeroOverrideIsAdoptedOnceAndOnlyOnce()
    {
        // Discarding it on upgrade would silently throw away every layout anybody had
        // already tuned; re-adopting it would add a copy on every launch.
        using var fixture = new TouchGamepadFixture();

        fixture.Documents.Write(
            WindowsTouchOverrideStore.DocumentName(TouchProfileId.Pro2),
            TouchLayoutOverrideJsonCodec.Encode(new TouchLayoutOverride
            {
                ProfileId = TouchProfileId.Pro2,
                TemplateId = TouchLayoutV1.Id,
                BasedOnRevision = TouchLayoutV1.TemplateRevision,
                Controls = new Dictionary<string, TouchControlOverride>(StringComparer.Ordinal)
                {
                    [TouchLayoutV1.Dpad] = new() { AnchorX = 0.28f },
                },
            }));

        var first = fixture.NewService();
        Assert.Single(first.State.Value.Library.UserProfiles);
        Assert.Equal(0.28f, first.State.Value.Document.Instance(TouchLayoutV1.Dpad)!.AnchorX, 4);

        // A second launch finds a library document and must not adopt again.
        var second = fixture.NewService();
        Assert.Single(second.State.Value.Library.UserProfiles);
    }

    [Fact]
    public void AnExportedLayoutCanBeImportedBack()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();
        service.CreateProfile("Shared");
        service.Edit("Move", document =>
            TouchLayoutEditor.Place(document, Select(TouchLayoutV1.Dpad), 0.33f, 0.66f));
        service.Save();

        var exported = service.Export(service.State.Value.Library.SelectedProfileId);
        Assert.NotNull(exported);

        Assert.Null(service.Import(exported!));
        Assert.Equal(2, service.State.Value.Library.UserProfiles.Count);
        Assert.Equal(0.33f, service.State.Value.Document.Instance(TouchLayoutV1.Dpad)!.AnchorX, 4);
    }

    [Fact]
    public void ImportingSomethingThatIsNotALayoutIsRefusedWithAReason()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        var refusal = service.Import("""{"hello":"world"}""");

        Assert.False(string.IsNullOrWhiteSpace(refusal));
        Assert.Empty(service.State.Value.Library.UserProfiles);
    }

    [Fact]
    public void ImportingALayoutForAnotherControllerIsRefused()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        var foreign = TouchProfileLibraryJsonCodec.EncodeExport(new TouchLayoutProfile(
            "x", "Theirs",
            TouchLayoutDocument.AuthoredDefault(
                TouchProfileCatalog.Require(TouchProfileId.GameCube))));

        Assert.NotNull(service.Import(foreign));
        Assert.Empty(service.State.Value.Library.UserProfiles);
    }
}
