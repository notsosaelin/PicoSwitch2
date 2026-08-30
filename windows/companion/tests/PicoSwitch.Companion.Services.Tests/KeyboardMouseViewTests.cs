using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The Keyboard and Mouse projection.
///
/// The rules worth guarding are the ones that go wrong quietly: a slider bounded
/// by client constants instead of the adapter's reply, a requested mode shown as
/// the active one, and a binding the app cannot draw silently disappearing.
/// </summary>
public sealed class KeyboardMouseViewTests
{
    private static KeyboardMouseState State(
        KbmStatus? status = null,
        KbmMouseConfig? mouse = null,
        IEnumerable<KbmBinding>? bindings = null,
        CapabilityState capability = CapabilityState.Available,
        KbmProfile profile = KbmProfile.Keyboard) => new()
    {
        Status = status ?? new KbmStatus(),
        Mouse = mouse ?? Ranged(),
        Mappings = new ValueList<KbmMapping>(
        [
            new KbmMapping(
                profile,
                new ValueList<KbmBinding>(bindings?.ToList() ?? []),
                Loaded: true),
        ]),
        Loaded = true,
        Capability = capability,
    };

    private static KbmMouseConfig Ranged() => new(
        SensitivityX: 256, SensitivityY: 256, VelocityWindowMs: 8,
        AntiDeadzone: 10,
        SensitivityMin: 64, SensitivityMax: 1024,
        VelocityWindowMinMs: 2, VelocityWindowMaxMs: 32,
        AntiDeadzoneMax: 100);

    private static KeyboardMouseView Project(
        KeyboardMouseState? state = null,
        KbmProfile profile = KbmProfile.Keyboard,
        bool connected = true) =>
        KeyboardMouse.Project(state ?? State(), profile, connected);

    /* --------------------------------------------------- capability and gating */

    [Fact]
    public void AnUnsupportedAdapterHidesTheFeatureAndSaysTheRestStillWorks()
    {
        var view = Project(State(capability: CapabilityState.Unsupported));

        Assert.False(view.Visible);
        Assert.Contains("Everything else in this app still works", view.HiddenReason);
    }

    [Fact]
    public void AnUnknownCapabilityKeepsThePage()
    {
        // The rule that stops a probe timeout from removing a working feature.
        Assert.True(Project(State(capability: CapabilityState.Unknown)).Visible);
    }

    [Fact]
    public void BeingDisconnectedDisablesEditingWithoutClaimingTheFirmwareLacksIt()
    {
        var view = Project(connected: false);

        Assert.True(view.Visible);
        Assert.False(view.Availability.Enabled);
        Assert.Equal(AdapterDashboard.NotConnected, view.Availability.DisabledReason);
    }

    /* ------------------------------------------------------------ mouse ranges */

    [Fact]
    public void SliderBoundsComeFromTheAdapterNotFromClientConstants()
    {
        // §16.3. Client-side constants drift from firmware the moment either side
        // changes, and the failure is silent.
        var sensitivity = Project().MouseSliders.Single(s => s.Field == KbmMouseField.Sensitivity);

        Assert.Equal(64, sensitivity.Minimum);
        Assert.Equal(1024, sensitivity.Maximum);
        Assert.True(sensitivity.Available);
    }

    [Fact]
    public void ARangeThatHasNotBeenReadYetDisablesItsSliderAndExplainsWhy()
    {
        // The default config is all zeroes, so every range is degenerate until
        // `kbm mouse` answers. Dragging against invented bounds would let the user
        // pick a value the adapter clamps, with the slider springing back and
        // nothing saying why.
        //
        // For SENSITIVITY this is the only way it can be unavailable: the decoder
        // requires sensitivityMax > sensitivityMin, so any reply that parses has a
        // real range.
        var view = Project(State(mouse: new KbmMouseConfig(SensitivityX: 256)));

        var sensitivity = view.MouseSliders.Single(s => s.Field == KbmMouseField.Sensitivity);
        Assert.False(sensitivity.Available);
        Assert.Contains("has not reported an adjustable range", sensitivity.Detail);
    }

    [Fact]
    public void AGenuinelyDegenerateRangeAlsoDisablesItsSlider()
    {
        // Unlike sensitivity, the decoder permits recenterMax == recenterMin and
        // antiDeadzoneMax == 0, so an adapter really can report these two as not
        // adjustable.
        var view = Project(State(mouse: Ranged() with
        {
            VelocityWindowMinMs = 8,
            VelocityWindowMaxMs = 8,
            AntiDeadzoneMax = 0,
        }));

        Assert.False(view.MouseSliders.Single(s => s.Field == KbmMouseField.VelocityWindow).Available);
        Assert.False(view.MouseSliders.Single(s => s.Field == KbmMouseField.AntiDeadzone).Available);
        Assert.True(view.MouseSliders.Single(s => s.Field == KbmMouseField.Sensitivity).Available);
    }

    [Fact]
    public void EachSliderIsRangedIndependently()
    {
        // One missing range must not disable the other two.
        var view = Project(State(mouse: Ranged() with { AntiDeadzoneMax = 0 }));

        Assert.True(view.MouseSliders.Single(s => s.Field == KbmMouseField.Sensitivity).Available);
        Assert.True(view.MouseSliders.Single(s => s.Field == KbmMouseField.VelocityWindow).Available);
        Assert.False(view.MouseSliders.Single(s => s.Field == KbmMouseField.AntiDeadzone).Available);
    }

    [Fact]
    public void SensitivityIsShownAsItsMultiplierBecauseTheRawValueMeansNothing() =>
        Assert.Equal(
            "×1.00",
            Project().MouseSliders.Single(s => s.Field == KbmMouseField.Sensitivity).Detail);

    /* ------------------------------------------------------------------- mode */

    [Fact]
    public void AnOverrideWaitingForItsDeviceIsNotShownAsTheActiveMode()
    {
        // Mode and ModeOverride are separate facts. Conflating them has the user
        // tuning a profile the adapter is not using.
        var view = Project(State(new KbmStatus(
            Mode: KbmMode.Controller,
            ModeOverride: KbmMode.KeyboardMouse)));

        Assert.Contains("Using controller only", view.ModeText);
        Assert.Contains("waiting for the device", view.ModeText);
    }

    [Fact]
    public void AnOverrideThatHasTakenEffectIsNotNaggedAbout()
    {
        var view = Project(State(new KbmStatus(
            Mode: KbmMode.Keyboard,
            ModeOverride: KbmMode.Keyboard)));

        Assert.Equal("Using keyboard", view.ModeText);
        Assert.DoesNotContain("waiting", view.ModeText);
    }

    [Fact]
    public void NoDevicesConnectedIsStatedWithoutBeingAnError()
    {
        // KB/M can be configured with nothing plugged in; the mapping is what is
        // being edited, not the live devices.
        Assert.Equal("No keyboard or mouse connected to the adapter", Project().DevicesText);

        Assert.Equal(
            "Keyboard and mouse connected",
            Project(State(new KbmStatus(KeyboardConnected: true, MouseConnected: true))).DevicesText);
    }

    /* --------------------------------------------------------------- bindings */

    [Fact]
    public void EveryDrawnKeyGetsACellWhetherBoundOrNot()
    {
        var view = Project();

        Assert.NotEmpty(view.Keys);
        Assert.All(view.Keys, cell => Assert.False(cell.Bound));
        Assert.Equal(5, view.MouseButtons.Count);
    }

    [Fact]
    public void ABoundKeyCarriesItsDestinationAndWhetherItWasChanged()
    {
        var view = Project(State(bindings:
        [
            new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x04), KbmDestination.A, Custom: true),
        ]));

        var a = view.Keys.Single(cell => cell.Cap.Usage == 0x04);
        Assert.True(a.Bound);
        Assert.Equal("A", a.DestinationLabel);
        Assert.True(a.Overridden);
        Assert.Equal(1, view.BoundCount);
    }

    [Fact]
    public void ABindingForAKeyThisBuildCannotDrawIsListedRatherThanDropped()
    {
        // A binding the user cannot see is one they cannot remove, and a newer
        // adapter is allowed to know keys this build does not.
        var view = Project(State(bindings:
        [
            new KbmBinding(new KbmSource(KbmSourceKind.Key, 0xA5), KbmDestination.Plus, Custom: true),
        ]));

        var undrawn = Assert.Single(view.Undrawn);
        Assert.Equal("Key 0xA5", undrawn.Cap.Label);
        Assert.Equal("Plus", undrawn.DestinationLabel);
        Assert.DoesNotContain(view.Keys, cell => cell.Cap.Usage == 0xA5);
    }

    [Fact]
    public void AnUndrawnKeyWithNoDestinationIsNotListedAsClutter() =>
        Assert.Empty(Project(State(bindings:
        [
            new KbmBinding(new KbmSource(KbmSourceKind.Key, 0xA5), KbmDestination.None, Custom: false),
        ])).Undrawn);

    [Fact]
    public void MouseButtonsAreBoundThroughTheSameGridButAreNotKeys()
    {
        var view = Project(State(bindings:
        [
            new KbmBinding(
                new KbmSource(KbmSourceKind.MouseButton, 1),
                KbmDestination.Zr,
                Custom: false),
        ]));

        var left = view.MouseButtons.Single(cell => cell.Cap.Usage == 1);
        Assert.Equal("ZR", left.DestinationLabel);

        // The key grid is untouched: a mouse button has no physical position on a
        // keyboard, and usage 1 is not a key usage at all.
        Assert.All(view.Keys, cell => Assert.False(cell.Bound));
    }

    [Fact]
    public void EditingAProfileTheAdapterIsNotUsingIsCalledOut()
    {
        // The profile is NOT user-settable -- there is no `kbm profile` command.
        // The adapter derives it from which roles are filled. So a user can bind
        // a key in the other profile, watch every operation report success, and
        // get nothing at the console. Silence there is indistinguishable from a
        // broken keyboard, which is why this is stated rather than left implicit.
        var state = State(new KbmStatus(Profile: KbmProfile.KeyboardMouse));

        var editingOther = KeyboardMouse.Project(state, KbmProfile.Keyboard, true);
        Assert.True(editingOther.EditingInactiveProfile);
        Assert.Contains("keyboard and mouse profile", editingOther.InactiveProfileWarning);
        Assert.Contains("will not affect the console", editingOther.InactiveProfileWarning);

        var editingLive = KeyboardMouse.Project(state, KbmProfile.KeyboardMouse, true);
        Assert.False(editingLive.EditingInactiveProfile);
        Assert.Null(editingLive.InactiveProfileWarning);
    }

    [Fact]
    public void TheActiveProfileComesFromTheAdapterNotFromTheEditor() =>
        Assert.Equal(
            KbmProfile.KeyboardMouse,
            KeyboardMouse.Project(
                State(new KbmStatus(Profile: KbmProfile.KeyboardMouse)),
                KbmProfile.Keyboard,
                true).ActiveProfile);

    [Fact]
    public void TheKeyboardOnlyProfileDoesNotOfferMouseButtons()
    {
        // That profile is what the adapter uses when only a keyboard is attached,
        // so a mouse binding in it can never fire. Drawing five dead controls
        // invites the user to bind something that silently does nothing.
        var keyboardOnly = KeyboardMouse.Project(State(), KbmProfile.Keyboard, true);
        Assert.False(keyboardOnly.ShowMouseButtons);

        var both = KeyboardMouse.Project(State(profile: KbmProfile.KeyboardMouse), KbmProfile.KeyboardMouse, true);
        Assert.True(both.ShowMouseButtons);
    }

    [Fact]
    public void TheMappedCountMatchesWhatTheProfileActuallyShows()
    {
        // "27 of 88" must not count five inputs the user cannot see or press.
        var keyboardOnly = KeyboardMouse.Project(State(), KbmProfile.Keyboard, true);
        var both = KeyboardMouse.Project(
            State(profile: KbmProfile.KeyboardMouse),
            KbmProfile.KeyboardMouse,
            true);

        Assert.Equal(keyboardOnly.Keys.Count, keyboardOnly.MappableCount);
        Assert.Equal(both.Keys.Count + both.MouseButtons.Count, both.MappableCount);
    }

    [Fact]
    public void AMouseBindingIsNotCountedInTheKeyboardOnlyProfile()
    {
        // Even if the adapter reports one, it cannot fire there, so it must not
        // inflate the profile's mapped count.
        var view = KeyboardMouse.Project(
            State(bindings:
            [
                new KbmBinding(new KbmSource(KbmSourceKind.MouseButton, 1), KbmDestination.Zr, false),
            ]),
            KbmProfile.Keyboard,
            true);

        Assert.Equal(0, view.BoundCount);
    }

    [Fact]
    public void TheTwoProfilesAreProjectedSeparately()
    {
        // Showing one profile's bindings under the other's name would have the
        // user rebind the wrong thing.
        var state = State(
            bindings: [new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x04), KbmDestination.A, true)],
            profile: KbmProfile.Keyboard);

        Assert.Equal(1, KeyboardMouse.Project(state, KbmProfile.Keyboard, true).BoundCount);
        Assert.Equal(0, KeyboardMouse.Project(state, KbmProfile.KeyboardMouse, true).BoundCount);
    }

    [Fact]
    public void NothingReadYetIsNotTheSameAsNoBindings()
    {
        // An empty map before the first read is indistinguishable from a genuinely
        // empty profile, and only one of those should be presented as configured.
        Assert.False(Project(new KeyboardMouseState()).Loaded);
        Assert.True(Project().Loaded);
    }

    [Fact]
    public void EveryCellCarriesAScreenReaderNameWithTheWholeFact()
    {
        // The visual form is a key label, a smaller word and a colour, none of
        // which survives being read aloud.
        var view = Project(State(bindings:
        [
            new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x04), KbmDestination.A, Custom: true),
        ]));

        Assert.Equal("A, mapped to A, changed", view.Keys.Single(c => c.Cap.Usage == 0x04).AccessibleName);
        Assert.Equal("B, not mapped", view.Keys.Single(c => c.Cap.Usage == 0x05).AccessibleName);
    }

    [Fact]
    public void RuntimeCountersReportTheWholeIngressPath()
    {
        // Every number a keypress test needs, on three lines, in the order a
        // report travels: who holds the role, what got through, what did not.
        var view = KeyboardMouse.Project(
            State(status: new KbmStatus(
                Mode: KbmMode.Keyboard,
                Profile: KbmProfile.Keyboard,
                KeyboardConnected: true,
                KeyboardConn: 5,
                GroupId: 2,
                SourceId: 7,
                KeyboardReports: 17,
                Publishes: 42,
                RejectedNotOwner: 3,
                RejectedUnclassified: 5,
                UndecodedReports: 9)),
            KbmProfile.Keyboard,
            connected: true);

        var counters = view.Counters;
        Assert.NotNull(counters);
        Assert.Equal(
            "mode=keyboard profile=kb keyboard=yes (conn 5) mouse=no group=2 source=7",
            counters!.Roles);
        Assert.Equal("accepted(keyboard=17 mouse=0) published=42 rollover=0", counters.Accepted);
        Assert.Equal(
            "rejected(mode=0 duplicate=0 notOwner=3 noPeerKey=0 unclassified=5 noRole=0) " +
            "undecoded=9 roleLosses=0",
            counters.Rejected);
    }

    [Fact]
    public void CountersAreAbsentUntilSomethingHasBeenRead()
    {
        // Zeroes that were never read look exactly like zeroes that were, and
        // "no reports arrived" is the conclusion this display exists to support.
        var view = KeyboardMouse.Project(
            State(profile: KbmProfile.Keyboard) with { Loaded = false },
            KbmProfile.Keyboard,
            connected: true);

        Assert.Null(view.Counters);
    }
}


/// <summary>
/// The drawn keyboard's geometry.
///
/// The layout is a data table, so it is exactly the kind of thing a typo ruins
/// silently: a wrong column overlaps its neighbour, a wrong usage binds the wrong
/// key, and neither shows up until someone looks closely at a screenshot. These
/// check the properties a person cannot eyeball reliably.
/// </summary>
public sealed class KeyboardLayoutTests
{
    public static TheoryData<string> ClusterNames() =>
        new(KeyboardLayout.Clusters.Select(cluster => cluster.Name));

    private static KeyboardCluster Cluster(string name) =>
        KeyboardLayout.Clusters.Single(cluster => cluster.Name == name);

    [Fact]
    public void EveryDrawnKeyIsAUsageTheProtocolAccepts()
    {
        // KbmSource enforces 0x04..0xE7. A layout entry outside that range would
        // throw the moment the user clicked it.
        foreach (var cap in KeyboardLayout.AllKeys)
        {
            var source = cap.Source;
            Assert.Equal(KbmSourceKind.Key, source.Kind);
            Assert.InRange(source.Code, KbmSource.KeyUsageMin, KbmSource.KeyUsageMax);
        }
    }

    [Fact]
    public void NoKeyIsDrawnTwice()
    {
        // Two cells for one usage would show conflicting bindings for the same key
        // and make one of them unreachable.
        var usages = KeyboardLayout.AllKeys.Select(cap => cap.Usage).ToList();
        Assert.Equal(usages.Count, usages.Distinct().Count());
    }

    [Theory]
    [MemberData(nameof(ClusterNames))]
    public void NoTwoKeysInAClusterOverlap(string name)
    {
        // The failure a rows-of-lists model could not have: a mistyped column, or a
        // two-unit-tall key in the wrong place, silently covers its neighbour.
        var keys = Cluster(name).Keys;
        for (var i = 0; i < keys.Count; i++)
        {
            for (var j = i + 1; j < keys.Count; j++)
            {
                Assert.False(
                    keys[i].Overlaps(keys[j]),
                    $"{name}: {keys[i].Label} overlaps {keys[j].Label}");
            }
        }
    }

    [Theory]
    [MemberData(nameof(ClusterNames))]
    public void EveryKeyFitsInsideItsClustersDeclaredBounds(string name)
    {
        // The canvas is sized from Columns and Rows, so a key past either edge is
        // simply clipped away and becomes unclickable.
        var cluster = Cluster(name);
        foreach (var cap in cluster.Keys)
        {
            Assert.True(cap.Right <= cluster.Columns, $"{name}: {cap.Label} past the right edge");
            Assert.True(cap.Bottom <= cluster.Rows, $"{name}: {cap.Label} past the bottom edge");
            Assert.True(cap.Width > 0 && cap.Height > 0, $"{name}: {cap.Label} has no size");
        }
    }

    [Fact]
    public void TheMainBlockRowsEachSpanTheFullFifteenUnits()
    {
        // A short row is a missing key; a long one overhangs the block. Both are
        // hard to see in a screenshot and obvious here.
        foreach (var row in new[] { 1.25, 2.25, 3.25, 4.25, 5.25 })
        {
            var width = KeyboardLayout.Main.Keys
                .Where(cap => cap.Row == row)
                .Sum(cap => cap.Width);
            Assert.Equal(15.0, width);
        }
    }

    [Fact]
    public void TheThreeClustersShareRowOriginsSoTheyLineUp()
    {
        // What makes the picture read as one keyboard rather than three widgets.
        Assert.Equal(KeyboardLayout.Main.Rows, KeyboardLayout.Navigation.Rows);
        Assert.Equal(KeyboardLayout.Main.Rows, KeyboardLayout.Numpad.Rows);

        var bottomRow = KeyboardLayout.Main.Keys.Single(cap => cap.Label == "Space").Row;
        Assert.Contains(KeyboardLayout.Navigation.Keys, cap => cap.Row == bottomRow);
        Assert.Contains(KeyboardLayout.Numpad.Keys, cap => cap.Row == bottomRow);
    }

    [Fact]
    public void TheArrowsFormAnInvertedT()
    {
        // Drawn inline they are unrecognisable; the T is most of what makes the
        // navigation block readable at a glance.
        var up = KeyboardLayout.Navigation.Keys.Single(cap => cap.Label == "Up");
        var left = KeyboardLayout.Navigation.Keys.Single(cap => cap.Label == "Left");
        var down = KeyboardLayout.Navigation.Keys.Single(cap => cap.Label == "Down");
        var right = KeyboardLayout.Navigation.Keys.Single(cap => cap.Label == "Right");

        Assert.Equal(down.Column, up.Column);
        Assert.Equal(up.Row + 1, down.Row);
        Assert.Equal(down.Row, left.Row);
        Assert.Equal(down.Row, right.Row);
        Assert.True(left.Column < down.Column && down.Column < right.Column);
    }

    [Fact]
    public void TheNumpadPlusAndEnterAreTwoUnitsTall()
    {
        // As on the hardware. A one-unit Plus leaves a hole the user reads as a
        // missing key.
        Assert.Equal(2, KeyboardLayout.Numpad.Keys.Single(cap => cap.Usage == 0x57).Height);
        Assert.Equal(2, KeyboardLayout.Numpad.Keys.Single(cap => cap.Usage == 0x58).Height);
        Assert.Equal(2, KeyboardLayout.Numpad.Keys.Single(cap => cap.Usage == 0x62).Width);
    }

    [Fact]
    public void EveryLetterDigitAndFunctionKeyIsPresent()
    {
        var labels = KeyboardLayout.AllKeys.Select(cap => cap.Label).ToHashSet();
        foreach (var letter in "QWERTYUIOPASDFGHJKLZXCVBNM")
        {
            Assert.Contains(letter.ToString(), labels);
        }

        for (var f = 1; f <= 12; f++)
        {
            Assert.Contains($"F{f}", labels);
        }
    }

    [Fact]
    public void TheWholeNumpadIsPresent()
    {
        // The complaint that produced this layout: a full-size keyboard's numpad
        // was missing entirely, so a third of the board could not be bound.
        var usages = KeyboardLayout.Numpad.Keys.Select(cap => cap.Usage).ToHashSet();
        foreach (var usage in Enumerable.Range(0x53, 0x63 - 0x53 + 1))
        {
            Assert.Contains(usage, usages);
        }

        Assert.Equal(17, KeyboardLayout.Numpad.Keys.Count);
    }

    [Fact]
    public void TheFullNavigationClusterIsPresentIncludingPrintScreenAndPause()
    {
        var usages = KeyboardLayout.Navigation.Keys.Select(cap => cap.Usage).ToHashSet();
        foreach (var usage in Enumerable.Range(0x46, 0x52 - 0x46 + 1))
        {
            Assert.Contains(usage, usages);
        }
    }

    [Fact]
    public void TheKeysAnAnsiPictureCannotPlaceAreOfferedSeparatelyRatherThanOmitted()
    {
        // ISO boards genuinely have two keys ANSI does not, and Japanese boards
        // several more. Drawing them inside the ANSI picture would put them
        // somewhere they are not; omitting them would make them unbindable.
        var usages = KeyboardLayout.Other.Keys.Select(cap => cap.Usage).ToHashSet();
        Assert.Contains(0x64, usages);
        Assert.Contains(0x32, usages);
        Assert.Contains(0x89, usages);

        // And they are not smuggled into the drawn clusters.
        foreach (var cluster in KeyboardLayout.Clusters)
        {
            Assert.DoesNotContain(cluster.Keys, cap => usages.Contains(cap.Usage));
        }
    }

    [Fact]
    public void ASmallerKeyboardIsCoveredBecauseThisIsTheSuperset()
    {
        // The reason to draw full-size ANSI: a TKL owner never presses the numpad,
        // but a 60% owner still needs somewhere to click to bind F5 -- which a
        // picture matching their own board would not give them.
        var usages = KeyboardLayout.AllKeys.Select(cap => cap.Usage).ToHashSet();
        Assert.Contains(0x3E, usages);
        Assert.Contains(0x4B, usages);
        Assert.Contains(0x5F, usages);
    }

    [Fact]
    public void EveryMouseButtonIsAValidSource()
    {
        foreach (var cap in KeyboardLayout.MouseButtons)
        {
            var source = KeyboardLayout.MouseSource(cap);
            Assert.Equal(KbmSourceKind.MouseButton, source.Kind);
            Assert.InRange(source.Code, KbmSource.MouseButtonMin, KbmSource.MouseButtonMax);
        }
    }

    [Fact]
    public void AnUndrawnSourceStillDescribesItself()
    {
        Assert.Equal("Key 0xA5", KeyboardLayout.Describe(new KbmSource(KbmSourceKind.Key, 0xA5)));
        Assert.Equal("Mouse Left", KeyboardLayout.Describe(new KbmSource(KbmSourceKind.MouseButton, 1)));
        Assert.Equal("Num", KeyboardLayout.Describe(new KbmSource(KbmSourceKind.Key, 0x53)));
    }

    [Fact]
    public void SpaceIsWideAndALetterIsNot()
    {
        // Width is what makes the picture read as a keyboard rather than a grid.
        Assert.True(KeyboardLayout.Main.Keys.Single(cap => cap.Label == "Space").Width > 4);
        Assert.Equal(1.0, KeyboardLayout.Main.Keys.Single(cap => cap.Label == "A").Width);
    }
}
