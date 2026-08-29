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
            new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x88), KbmDestination.Plus, Custom: true),
        ]));

        var undrawn = Assert.Single(view.Undrawn);
        Assert.Equal("Key 0x88", undrawn.Cap.Label);
        Assert.Equal("Plus", undrawn.DestinationLabel);
        Assert.DoesNotContain(view.Keys, cell => cell.Cap.Usage == 0x88);
    }

    [Fact]
    public void AnUndrawnKeyWithNoDestinationIsNotListedAsClutter() =>
        Assert.Empty(Project(State(bindings:
        [
            new KbmBinding(new KbmSource(KbmSourceKind.Key, 0x88), KbmDestination.None, Custom: false),
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
}

/// <summary>
/// The drawn keyboard itself.
/// </summary>
public sealed class KeyboardLayoutTests
{
    [Fact]
    public void EveryDrawnKeyIsAUsageTheProtocolAccepts()
    {
        // KbmSource enforces 0x04..0xE7. A layout entry outside that range would
        // throw the moment the user clicked it.
        foreach (var cap in KeyboardLayout.Rows.SelectMany(row => row))
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
        // and make one of them unclickable.
        var usages = KeyboardLayout.Rows.SelectMany(row => row).Select(cap => cap.Usage).ToList();
        Assert.Equal(usages.Count, usages.Distinct().Count());
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
    public void TheLettersAndDigitsAreAllPresent()
    {
        // The layout is data and easy to typo. This catches a missing row.
        var labels = KeyboardLayout.Rows.SelectMany(row => row).Select(cap => cap.Label).ToHashSet();
        foreach (var letter in "QWERTYUIOPASDFGHJKLZXCVBNM")
        {
            Assert.Contains(letter.ToString(), labels);
        }

        foreach (var digit in "1234567890")
        {
            Assert.Contains(digit.ToString(), labels);
        }
    }

    [Fact]
    public void AnUndrawnSourceStillDescribesItself()
    {
        Assert.Equal("Key 0x88", KeyboardLayout.Describe(new KbmSource(KbmSourceKind.Key, 0x88)));
        Assert.Equal("Mouse Left", KeyboardLayout.Describe(new KbmSource(KbmSourceKind.MouseButton, 1)));
    }

    [Fact]
    public void SpaceIsWideAndALetterIsNot()
    {
        // Width is what makes the picture read as a keyboard rather than a grid.
        var space = KeyboardLayout.Rows.SelectMany(row => row).Single(cap => cap.Label == "Space");
        Assert.True(space.Width > 4);
        Assert.Equal(1.0, KeyboardLayout.Rows.SelectMany(row => row).First(cap => cap.Label == "A").Width);
    }
}
