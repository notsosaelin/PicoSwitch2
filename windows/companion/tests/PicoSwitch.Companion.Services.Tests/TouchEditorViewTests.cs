using PicoSwitch.Bridge.Touch;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The mapping from confirmed personality to touch layout (WINDOWS_PASS.md §26.1).
/// </summary>
public sealed class TouchProfileSelectorTests
{
    [Theory]
    [InlineData(Personality.Pro2, TouchProfileId.Pro2)]
    [InlineData(Personality.GameCube, TouchProfileId.GameCube)]
    [InlineData(Personality.JoyConLeft, TouchProfileId.JoyConLeft)]
    [InlineData(Personality.JoyConRight, TouchProfileId.JoyConRight)]
    public void EveryGameplayPersonalityHasALayout(
        Personality personality, TouchProfileId expected)
    {
        Assert.Equal(expected, TouchProfileSelector.Select(personality));

        // And the layout it names is one the catalog actually ships, or the surface would
        // resolve a personality it cannot draw.
        Assert.NotNull(TouchProfileCatalog.Profile(expected));
    }

    [Fact]
    public void WithNoAdapterConnectedTheLastConfirmedControllerIsUsed()
    {
        // What makes the editor useful on a train. A remembered personality is still a
        // CONFIRMATION — it is what the adapter reported at the last verified connection —
        // rather than a default.
        Assert.Equal(
            TouchProfileId.GameCube,
            TouchProfileSelector.SelectOrRemembered(Personality.Unknown, "gc"));

        // A live answer always wins over the remembered one.
        Assert.Equal(
            TouchProfileId.Pro2,
            TouchProfileSelector.SelectOrRemembered(Personality.Pro2, "gc"));
    }

    [Fact]
    public void WithNothingEverConfirmedThereIsStillNothingToDraw()
    {
        Assert.Null(TouchProfileSelector.SelectOrRemembered(Personality.Unknown, null));
        Assert.Null(TouchProfileSelector.SelectOrRemembered(Personality.Unknown, "config"));
        Assert.Null(TouchProfileSelector.SelectOrRemembered(Personality.Config, "nonsense"));
    }

    [Theory]
    [InlineData(Personality.Config)]
    [InlineData(Personality.Unknown)]
    public void TheNonGameplayPersonalitiesMapToNothing(Personality personality)
    {
        // Not to Pro Controller 2. A surface that draws a controller the console is not
        // being shown is a lie, and the labels under the thumb are the lie.
        Assert.Null(TouchProfileSelector.Select(personality));
    }
}

public sealed class TouchRegionBuilderTests
{
    [Fact]
    public void TheRegionIsTheSurfaceMinusItsInsets()
    {
        var region = TouchRegionBuilder.Build(1000d, 600d, new TouchSafeInsets(10f, 32f, 10f, 8f));

        Assert.Equal(10f, region.Left);
        Assert.Equal(32f, region.Top);
        Assert.Equal(990f, region.Right);
        Assert.Equal(592f, region.Bottom);
        Assert.Equal(TouchRegionBuilder.EffectivePixelsPerUnit, region.UnitScale);
    }

    [Fact]
    public void OneUnitIsOneEffectivePixel()
    {
        // The layout's unit is the platform's comfort unit, so a region's size in units is
        // its size in epx. Folding RasterizationScale in on top would shrink the
        // controller by exactly the factor the user chose when they asked Windows for
        // larger UI.
        var region = TouchRegionBuilder.Build(1000d, 600d, TouchSafeInsets.None);

        Assert.Equal(1000f, region.WidthUnits);
        Assert.Equal(600f, region.HeightUnits);
    }

    [Fact]
    public void TheSmallestLayoutTheAuditWillPassClearsThePlatformTouchTarget()
    {
        // This is the reason the unit is the effective pixel and not a converted dp:
        // 44 units has to still be a legal Windows touch target at the reference size.
        var region = TouchRegionBuilder.Build(
            TouchLayoutResolver.ReferenceWidthUnits,
            TouchLayoutResolver.ReferenceHeightUnits,
            TouchSafeInsets.None);

        var epxPerUnit = region.UnitScale;
        Assert.True(TouchLayoutAudit.MinTargetUnits * epxPerUnit >= 40f);
    }

    [Fact]
    public void AFirstMeasurePassIsNotAnError()
    {
        // WinUI measures at zero before it measures for real, and a zero-size region has
        // exactly one truthful thing to say — which the resolver already says.
        foreach (var region in new[]
                 {
                     TouchRegionBuilder.Build(0d, 600d, TouchSafeInsets.None),
                     TouchRegionBuilder.Build(1000d, 0d, TouchSafeInsets.None),
                     TouchRegionBuilder.Build(double.NaN, 600d, TouchSafeInsets.None),
                 })
        {
            Assert.Equal(0f, region.Width);
            Assert.True(TouchLayoutResolver
                .Resolve(new TouchLayout("x", 2, []), region).RegionTooSmall);
        }
    }

    [Fact]
    public void InsetsThatMeetInTheMiddleReportNothingUsableRatherThanAMirroredRegion()
    {
        // A negative Width would make every anchor resolve left of the region's own left
        // edge, which draws a whole controller off-screen instead of saying so.
        var region = TouchRegionBuilder.Build(100d, 600d, new TouchSafeInsets(60f, 0f, 60f, 0f));

        Assert.Equal(0f, region.Width);
        Assert.False(region.Width < 0f);
    }

    [Fact]
    public void ABadInsetCanNeverEnlargeTheRegion()
    {
        var region = TouchRegionBuilder.Build(
            1000d, 600d, new TouchSafeInsets(-50f, float.NaN, -1f, 0f));

        Assert.Equal(0f, region.Left);
        Assert.Equal(0f, region.Top);
        Assert.Equal(1000f, region.Right);
    }

    [Fact]
    public void TheCaptionBarIsAvoidedOnlyWhenTheSurfaceIsNotFullWindow()
    {
        // In full-window mode there is no drag region to avoid, and reserving one would
        // leave a band of dead space across the top of a gameplay surface.
        var windowed = TouchRegionBuilder.Insets(fullWindow: false, 48f, touchCapable: true);
        var full = TouchRegionBuilder.Insets(fullWindow: true, 48f, touchCapable: true);

        Assert.Equal(48f, windowed.Top);
        Assert.Equal(TouchRegionBuilder.EdgeGestureInsetEpx, full.Top);
        Assert.Equal(TouchRegionBuilder.EdgeGestureInsetEpx, full.Left);
    }

    [Fact]
    public void APointerOnlyMachineGivesUpNoRoomToGestureStrips()
    {
        var insets = TouchRegionBuilder.Insets(fullWindow: true, 48f, touchCapable: false);

        Assert.Equal(TouchSafeInsets.None, insets);
    }
}

public sealed class TouchEditorKeysTests
{
    [Theory]
    [InlineData("Left", TouchEditorCommand.NudgeLeft)]
    [InlineData("Right", TouchEditorCommand.NudgeRight)]
    [InlineData("Up", TouchEditorCommand.NudgeUp)]
    [InlineData("Down", TouchEditorCommand.NudgeDown)]
    [InlineData("Delete", TouchEditorCommand.Delete)]
    [InlineData("Escape", TouchEditorCommand.Deselect)]
    [InlineData("Tab", TouchEditorCommand.NextControl)]
    public void TheEditorIsReachableWithNoPointerAtAll(string key, TouchEditorCommand expected)
    {
        // §26.5 runs the editor entirely by keyboard before it runs it by anything else.
        Assert.Equal(expected, TouchEditorKeys.Resolve(key, control: false, shift: false).Command);
    }

    [Fact]
    public void ShiftAsksForTheCoarseStepAndOnlyForNudges()
    {
        var coarse = TouchEditorKeys.Resolve("Left", control: false, shift: true);
        Assert.Equal(TouchEditorCommand.NudgeLeft, coarse.Command);
        Assert.True(coarse.Coarse);

        // Shift+Tab is a direction, not a bigger Tab.
        var back = TouchEditorKeys.Resolve("Tab", control: false, shift: true);
        Assert.Equal(TouchEditorCommand.PreviousControl, back.Command);
        Assert.False(back.Coarse);
    }

    [Theory]
    [InlineData("Z", false, TouchEditorCommand.Undo)]
    [InlineData("Z", true, TouchEditorCommand.Redo)]
    [InlineData("Y", false, TouchEditorCommand.Redo)]
    [InlineData("S", false, TouchEditorCommand.Save)]
    [InlineData("A", false, TouchEditorCommand.SelectAll)]
    [InlineData("G", false, TouchEditorCommand.Group)]
    [InlineData("G", true, TouchEditorCommand.Ungroup)]
    public void TheDesktopIdiomsAreTheOnesAUserAlreadyKnows(
        string key, bool shift, TouchEditorCommand expected) =>
        Assert.Equal(expected, TouchEditorKeys.Resolve(key, control: true, shift).Command);

    [Fact]
    public void BothPlusKeysGrowTheSelection()
    {
        // VirtualKey has no OemPlus member, so the main row arrives as its raw code. A
        // user should not have to know which keyboard row this was written against.
        Assert.Equal(
            TouchEditorCommand.Grow,
            TouchEditorKeys.Resolve("Add", control: false, shift: false).Command);
        Assert.Equal(
            TouchEditorCommand.Grow,
            TouchEditorKeys.Resolve("187", control: false, shift: false).Command);
        Assert.Equal(
            TouchEditorCommand.Shrink,
            TouchEditorKeys.Resolve("Subtract", control: false, shift: false).Command);
        Assert.Equal(
            TouchEditorCommand.Shrink,
            TouchEditorKeys.Resolve("189", control: false, shift: false).Command);
    }

    [Fact]
    public void AnUnboundKeyDoesNothingRatherThanSomethingApproximate()
    {
        Assert.Equal(
            TouchEditorCommand.None,
            TouchEditorKeys.Resolve("F7", control: false, shift: false).Command);
        Assert.Equal(
            TouchEditorCommand.None,
            TouchEditorKeys.Resolve(null, control: true, shift: false).Command);

        // Ctrl+Left is not a coarse nudge; it is unbound, and inventing a meaning would
        // move a control when the user expected a word jump.
        Assert.Equal(
            TouchEditorCommand.None,
            TouchEditorKeys.Resolve("Left", control: true, shift: false).Command);
    }

    [Fact]
    public void ANudgeIsTheSameDISTANCEWhateverTheWindowSize()
    {
        // Measured in layout units and converted with the layout's own scale, so precise
        // keyboard work does not change meaning when the window is resized.
        var layout = TouchLayoutComposer
            .Compose(
                TouchProfileCatalog.Require(TouchProfileId.Pro2),
                TouchLayoutDocument.AuthoredDefault(
                    TouchProfileCatalog.Require(TouchProfileId.Pro2)))
            .Layout;

        var small = TouchLayoutResolver.Resolve(
            layout, TouchRegionBuilder.Build(900d, 460d, TouchSafeInsets.None));
        var large = TouchLayoutResolver.Resolve(
            layout, TouchRegionBuilder.Build(1800d, 920d, TouchSafeInsets.None));

        Assert.Equal(
            TouchEditorKeys.NudgeUnits * small.Scale,
            TouchEditorKeys.NudgePixels(small, coarse: false), 3);
        Assert.True(
            TouchEditorKeys.NudgePixels(large, coarse: false) >=
            TouchEditorKeys.NudgePixels(small, coarse: false));
        Assert.True(
            TouchEditorKeys.NudgePixels(small, coarse: true) >
            TouchEditorKeys.NudgePixels(small, coarse: false));
    }

    [Fact]
    public void ANudgeOnAnUnmeasuredLayoutStillMovesSomething()
    {
        // Zero pixels would be a key that silently does nothing.
        Assert.True(TouchEditorKeys.NudgePixels(ResolvedTouchLayout.Empty, coarse: false) > 0f);
    }
}

public sealed class TouchEditorViewTests
{
    private static IReadOnlySet<string> Select(params string[] ids) =>
        new HashSet<string>(ids, StringComparer.Ordinal);

    [Fact]
    public void WithNoPersonalityConfirmedTheSurfaceSaysSoAndOffersNoEditing()
    {
        var view = TouchEditorView.Of(new TouchGamepadState(), controllerLinkAvailable: true);

        Assert.False(view.Editable);
        Assert.False(view.CanSave);
        Assert.Contains("adapter", view.Status, StringComparison.OrdinalIgnoreCase);
        Assert.Null(view.LinkNote);
    }

    [Fact]
    public void WithoutAControllerLinkTheSurfaceExplainsItselfRatherThanLookingBroken()
    {
        // §15.8: the surface still opens and stays fully editable, "and the UI must say
        // exactly that rather than appearing broken".
        using var fixture = new TouchGamepadFixture();
        var view = TouchEditorView.Of(
            fixture.NewService().State.Value, controllerLinkAvailable: false);

        Assert.NotNull(view.LinkNote);
        Assert.Contains("cannot drive a console", view.LinkNote!, StringComparison.Ordinal);
        Assert.True(view.Editable);
    }

    [Fact]
    public void AConfirmedPersonalityIsNamedTheWayAPersonWouldNameIt()
    {
        using var fixture = new TouchGamepadFixture();
        var view = TouchEditorView.Of(
            fixture.NewService().State.Value, controllerLinkAvailable: true);

        Assert.Equal(
            TouchProfileCatalog.Require(TouchProfileId.Pro2).DisplayName, view.Title);
        Assert.Equal(TouchProfileLibrary.FactoryProfileName, view.ProfileName);
        Assert.Equal(TouchEditorSeverity.Neutral, view.StatusSeverity);
        Assert.Empty(view.Findings);
    }

    [Fact]
    public void ARememberedControllerIsLabelledAsRememberedAndStillFullyEditable()
    {
        using var fixture = new TouchGamepadFixture();
        var service = new TouchGamepadService(
            fixture.Profiles, fixture.Legacy, fixture.Diagnostics, () => fixture.Clock);
        service.SetPersonality(Personality.Unknown, "pro2");
        service.SetRegion(new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));

        var view = TouchEditorView.Of(service.State.Value, true);

        Assert.True(view.Editable);
        Assert.Contains("last seen", view.Subtitle, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AnAdapterConnectingMidEditDoesNotThrowTheEditAway()
    {
        // Same controller, newly known live rather than remembered. Reloading there would
        // discard the user's unsaved work as a side effect of plugging something in.
        using var fixture = new TouchGamepadFixture();
        var service = new TouchGamepadService(
            fixture.Profiles, fixture.Legacy, fixture.Diagnostics, () => fixture.Clock);
        service.SetPersonality(Personality.Unknown, "pro2");
        service.SetRegion(new TouchLayoutRegion(0f, 0f, 1600f, 840f, 2f));

        var authored = service.State.Value.Document.Instance(TouchLayoutV1.Dpad)!;
        service.Edit("Move", document => TouchLayoutEditor.Place(
            document, Select(TouchLayoutV1.Dpad), authored.AnchorX + 0.01f, authored.AnchorY));

        service.SetPersonality(Personality.Pro2, "pro2");

        var view = TouchEditorView.Of(service.State.Value, true);
        Assert.True(service.State.Value.Dirty);
        Assert.True(service.State.Value.CanUndo);
        Assert.DoesNotContain("last seen", view.Subtitle, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void SaveLightsUpOnUnsavedWorkEvenOnTheFactoryProfile()
    {
        // Saving onto the factory profile is legal — the library turns it into a new
        // profile — so a disabled Save there would be refusing an action that works.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        Assert.False(TouchEditorView.Of(service.State.Value, true).CanSave);

        // A nudge from the authored position: this test is about Save lighting up, and an
        // absolute coordinate that happened to land on another control would change the
        // status line to an audit finding instead.
        var authored = service.State.Value.Document.Instance(TouchLayoutV1.Dpad)!;
        service.Edit("Move", document =>
            TouchLayoutEditor.Place(
                document, Select(TouchLayoutV1.Dpad), authored.AnchorX + 0.01f, authored.AnchorY));

        var view = TouchEditorView.Of(service.State.Value, true);
        Assert.True(view.CanSave);
        Assert.True(view.CanUndo);
        Assert.Contains("unsaved", view.Status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void GroupIsEnabledExactlyWhenTheEditorWouldAcceptIt()
    {
        // Asked through the editor itself. A button enabled by a different rule is a
        // button that refuses when pressed.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.SetSelection(Select(TouchLayoutV1.Dpad));
        Assert.False(TouchEditorView.Of(service.State.Value, true).CanGroup);

        service.SetSelection(Select(TouchLayoutV1.Minus, TouchLayoutV1.Plus));
        Assert.True(TouchEditorView.Of(service.State.Value, true).CanGroup);
    }

    [Fact]
    public void TheSelectionIsDescribedByTheLabelTheUserCanSee()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        Assert.Equal("Nothing selected", TouchEditorView.Of(service.State.Value, true).SelectionSummary);

        service.SetSelection(Select(TouchLayoutV1.Minus, TouchLayoutV1.Plus));
        Assert.Equal("2 controls selected",
                     TouchEditorView.Of(service.State.Value, true).SelectionSummary);

        // Never the raw catalog id: "dpad" is a wire identifier, and the audit above the
        // panel already calls the same control "Dpad".
        service.SetSelection(Select(TouchLayoutV1.Dpad));
        Assert.Equal("Dpad", TouchEditorView.Of(service.State.Value, true).SelectionSummary);
    }

    [Fact]
    public void ACopyIsNamedWithItsCopyNumberSoTheTwoCanBeToldApart()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.Edit("Add", document => TouchLayoutEditor.Add(
            document, TouchProfileCatalog.Require(TouchProfileId.Pro2),
            TouchLayoutV1.Dpad, 0.5f, 0.5f));

        Assert.Equal("Dpad (2)", TouchEditorView.Of(service.State.Value, true).SelectionSummary);
    }

    [Fact]
    public void AMixedSelectionShowsNoSizeRatherThanOneMembersSize()
    {
        // Picking one member's value and applying it to the rest on the next keystroke is
        // how a properties panel silently resizes controls nobody touched.
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        service.Edit("Resize", document =>
            TouchLayoutEditor.SetScale(document, Select(TouchLayoutV1.Minus), 1.4f, false));
        service.SetSelection(Select(TouchLayoutV1.Minus, TouchLayoutV1.Plus));

        Assert.Null(TouchEditorView.Of(service.State.Value, true).Scale);

        service.SetSelection(Select(TouchLayoutV1.Minus));
        Assert.Equal(1.4f, TouchEditorView.Of(service.State.Value, true).Scale!.Value, 3);
    }

    [Fact]
    public void ABlockingFindingIsListedAheadOfAnAdvisoryOneAndIsSaidLoudly()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();

        var stick = service.State.Value.Document.Instance(TouchLayoutV1.StickLeft)!;
        service.Edit("Move", document =>
            TouchLayoutEditor.Place(
                document, Select(TouchLayoutV1.Dpad), stick.AnchorX, stick.AnchorY));

        var view = TouchEditorView.Of(service.State.Value, true);

        Assert.Equal(TouchEditorSeverity.Blocking, view.StatusSeverity);
        Assert.NotEmpty(view.Findings);
        Assert.True(view.Findings[0].Blocking);
        Assert.Equal(TouchEditorSeverity.Blocking, view.Findings[0].Severity);

        // The editor stays open, because dragging it back off is how it gets fixed.
        Assert.True(view.Editable);
        Assert.True(view.CanUndo);
    }

    [Fact]
    public void AStorageComplaintIsAdvisoryBecauseTheLayoutInFrontOfTheUserIsFine()
    {
        using var fixture = new TouchGamepadFixture();
        fixture.Documents.Write(
            Companion.Windows.Storage.WindowsTouchProfileStore.DocumentName(TouchProfileId.Pro2),
            "not a layout");

        var view = TouchEditorView.Of(fixture.NewService().State.Value, true);

        Assert.Equal(TouchEditorSeverity.Advisory, view.StatusSeverity);
        Assert.True(view.Editable);
    }

    [Fact]
    public void AWindowTooSmallToPlayInOffersNoEditingButStillExplainsWhy()
    {
        using var fixture = new TouchGamepadFixture();
        var service = fixture.NewService();
        service.SetRegion(TouchRegionBuilder.Build(300d, 200d, TouchSafeInsets.None));

        var view = TouchEditorView.Of(service.State.Value, true);

        Assert.False(view.Editable);
        Assert.False(view.CanSave);
        Assert.Equal(TouchEditorSeverity.Blocking, view.StatusSeverity);
        Assert.Contains("too small", view.Status, StringComparison.OrdinalIgnoreCase);
    }
}
