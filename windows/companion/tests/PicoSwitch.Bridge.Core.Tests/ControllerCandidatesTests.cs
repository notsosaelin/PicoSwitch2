using PicoSwitch.Bridge.Core;
using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The shared usability/exclusion rule.
///
/// The case it was written from, identified on Android 2026-08-14: an entry named
/// <c>Virtual</c> with VID/PID <c>0000:0000</c> appeared even on a device with no
/// built-in controller at all. It reached the list because it advertises a D-pad
/// source, and the enumeration accepted a D-pad alone as evidence of a
/// controller. Windows enumerates its own share of synthetic devices, so the rule
/// is shared and the platform only supplies the signals.
/// </summary>
public sealed class ControllerCandidatesTests
{
    private static ControllerCandidate Real(
        int id = 1,
        string descriptor = "real",
        int vendorId = 0x054C,
        int productId = 0x0CE6) =>
        new(id, descriptor, "DualSense", vendorId, productId,
            HasMotionAxes: true, HasGamepadButtons: true);

    [Fact]
    public void ARealControllerIsUsable() => Assert.True(Real().IsUsable);

    [Fact]
    public void ThePlatformsOwnVirtualClassificationIsTheStrongestSignal()
    {
        var candidate = Real() with { IsVirtual = true };
        Assert.False(candidate.IsUsable);
        Assert.Equal("The system reports this as a virtual device", candidate.ExclusionReason);
    }

    [Fact]
    public void ADpadOrKeyboardSourceAloneIsNotAController()
    {
        var keyboard = new ControllerCandidate(
            2, "kb", "Virtual", 0, 0,
            HasMotionAxes: false, HasGamepadButtons: false, HasGamepadSource: false);
        Assert.False(keyboard.IsUsable);
        Assert.Equal(
            "Not a gamepad or joystick (D-pad/keyboard source only)",
            keyboard.ExclusionReason);
    }

    [Fact]
    public void AnonymousAndCapabilityLessIsExcludedOnlyWhenBOTHHalvesHold()
    {
        var anonymousAndEmpty = new ControllerCandidate(
            3, "ghost", "Virtual", 0, 0,
            HasMotionAxes: false, HasGamepadButtons: false);
        Assert.False(anonymousAndEmpty.IsUsable);

        // A device with a real VID/PID is never hidden however odd its capabilities.
        var identifiedButEmpty = anonymousAndEmpty with { VendorId = 0x1234 };
        Assert.True(identifiedButEmpty.IsUsable);

        // And a VID/PID-less device that genuinely reports controls is kept: some
        // kernel-level built-in controllers look exactly like that.
        var anonymousButCapable = anonymousAndEmpty with { HasGamepadButtons = true };
        Assert.True(anonymousButCapable.IsUsable);
    }

    [Fact]
    public void ExclusionIsNeverBasedOnTheDeviceName()
    {
        // Names are unstable across vendors and controller modes: the same physical
        // controls enumerate as "Odin Controller", then "Xbox Gamepad" after a mode
        // switch. A name blacklist would hide real hardware.
        var suspiciouslyNamed = Real() with { Name = "Virtual" };
        Assert.True(suspiciouslyNamed.IsUsable);
    }

    [Fact]
    public void ExclusionsAreExplainedSoAWronglyHiddenDeviceCanBeIdentified()
    {
        var excluded = Real() with { IsVirtual = true };
        Assert.NotNull(excluded.ExclusionReason);
        Assert.Null(Real().ExclusionReason);
    }

    [Fact]
    public void UsableAndExcludedPartitionTheListInEnumerationOrder()
    {
        List<ControllerCandidate> candidates =
        [
            Real(1, "a"),
            Real(2, "b") with { IsVirtual = true },
            Real(3, "c"),
        ];

        Assert.Equal(["a", "c"], ControllerCandidates.Usable(candidates).Select(c => c.Descriptor));
        Assert.Equal(["b"], ControllerCandidates.Excluded(candidates).Select(c => c.Descriptor));
    }

    [Fact]
    public void ExactlyOneUsableControllerIsSelectedWithoutAsking()
    {
        List<ControllerCandidate> candidates = [Real(1, "a"), Real(2, "b") with { IsVirtual = true }];
        Assert.Equal("a", ControllerCandidates.AutoSelect(candidates)?.Descriptor);
        Assert.False(ControllerCandidates.NeedsUserChoice(candidates));
    }

    [Fact]
    public void WithTwoOrMoreUsableDevicesTheAppMustNotGuess()
    {
        List<ControllerCandidate> candidates = [Real(1, "a"), Real(2, "b")];
        Assert.Null(ControllerCandidates.AutoSelect(candidates));
        Assert.True(ControllerCandidates.NeedsUserChoice(candidates));
    }

    [Fact]
    public void NothingUsableSelectsNothing()
    {
        List<ControllerCandidate> candidates = [Real(1, "a") with { IsVirtual = true }];
        Assert.Null(ControllerCandidates.AutoSelect(candidates));
        Assert.False(ControllerCandidates.NeedsUserChoice(candidates));
    }

    [Fact]
    public void ARefreshKeepsTheUsersExistingChoiceWhileItIsStillUsable()
    {
        List<ControllerCandidate> candidates = [Real(1, "a"), Real(2, "b")];
        Assert.Equal("b", ControllerCandidates.ResolveSelection(candidates, "b")?.Descriptor);
    }

    [Fact]
    public void AChoiceThatDisappearedFallsBackToAnUnambiguousAutoSelect()
    {
        List<ControllerCandidate> candidates = [Real(1, "a")];
        Assert.Equal("a", ControllerCandidates.ResolveSelection(candidates, "gone")?.Descriptor);
    }

    [Fact]
    public void AChoiceThatDisappearedWithSeveralAlternativesLeavesTheUserToChoose()
    {
        List<ControllerCandidate> candidates = [Real(1, "a"), Real(2, "b")];
        Assert.Null(ControllerCandidates.ResolveSelection(candidates, "gone"));
    }

    [Fact]
    public void AnExcludedDeviceIsNeverKeptEvenIfItWasTheStoredSelection()
    {
        List<ControllerCandidate> candidates =
        [
            Real(1, "a") with { IsVirtual = true },
            Real(2, "b"),
        ];
        Assert.Equal("b", ControllerCandidates.ResolveSelection(candidates, "a")?.Descriptor);
    }
}

public sealed class ControllerLayoutResolverTests
{
    [Fact]
    public void NoSourceMeansPositionalOrderRatherThanAGuess()
    {
        var resolved = ControllerLayoutResolver.Resolve(ControllerFaceLayout.Auto, null);
        Assert.Equal(ControllerFaceLayout.Xbox, resolved.Layout);
        Assert.Contains("No input source selected", resolved.Reason);
    }

    [Fact]
    public void AManualChoiceIsAlwaysAuthoritative()
    {
        var source = new ControllerSourceIdentity("odin", "Odin Controller", 0x2020, 0x0111);
        var resolved = ControllerLayoutResolver.Resolve(ControllerFaceLayout.Xbox, source);
        Assert.Equal(ControllerFaceLayout.Xbox, resolved.Layout);
        Assert.Equal("Selected manually", resolved.Reason);
    }

    [Fact]
    public void AnUnknownSourceIsAssumedToReportPositions()
    {
        var source = new ControllerSourceIdentity("ds", "DualSense", 0x054C, 0x0CE6);
        Assert.Equal(
            ControllerFaceLayout.Xbox,
            ControllerLayoutResolver.Resolve(ControllerFaceLayout.Auto, source).Layout);
    }

    /// <summary>
    /// AYN's button-layout toggle changes the DEVICE IDENTITY, and with it which
    /// key code each physical button sends. Both modes were read off a live
    /// Odin 2 Mini on 2026-08-24, so the two PIDs must NOT resolve alike however
    /// similar the hardware is: Xbox mode is an Xbox-style source that happens to
    /// live behind Nintendo-printed plastic, and treating it as Nintendo inverts
    /// every face button.
    /// </summary>
    [Fact]
    public void TheTwoOdinModesResolveDifferently()
    {
        var printedLegend = new ControllerSourceIdentity("a", "Odin Controller", 0x2020, 0x0111);
        var positional = new ControllerSourceIdentity("b", "Xbox Wireless Controller", 0x2020, 0x0112);

        Assert.Equal(
            ControllerFaceLayout.Nintendo,
            ControllerLayoutResolver.Resolve(ControllerFaceLayout.Auto, printedLegend).Layout);
        Assert.Equal(
            ControllerFaceLayout.Xbox,
            ControllerLayoutResolver.Resolve(ControllerFaceLayout.Auto, positional).Layout);
    }

    [Fact]
    public void TheAuditedRetroidIdentityAlsoReportsItsPrintedLegend()
    {
        var retroid = new ControllerSourceIdentity("r", "Retroid Pocket Controller", 0x2022, 0x3001);
        Assert.Equal(
            ControllerFaceLayout.Nintendo,
            ControllerLayoutResolver.Resolve(ControllerFaceLayout.Auto, retroid).Layout);
    }

    [Fact]
    public void LayoutKeysRoundTripSoAStoredPreferenceSurvivesARestart()
    {
        foreach (var layout in ControllerFaceLayouts.All)
        {
            Assert.Equal(layout, ControllerFaceLayouts.FromKey(layout.Key()));
        }

        // An unknown or absent stored value falls back to Auto rather than failing.
        Assert.Equal(ControllerFaceLayout.Auto, ControllerFaceLayouts.FromKey(null));
        Assert.Equal(ControllerFaceLayout.Auto, ControllerFaceLayouts.FromKey("playstation"));
    }

    [Fact]
    public void TheNullStoreIsUsableAndInert()
    {
        var store = IControllerLayoutStore.None.Instance;
        store.Save("descriptor", ControllerFaceLayout.Nintendo);
        Assert.Equal(ControllerFaceLayout.Auto, store.Load("descriptor"));
    }
}
