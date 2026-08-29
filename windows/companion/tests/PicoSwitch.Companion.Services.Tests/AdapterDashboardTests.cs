using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The adapter dashboard projection: every connection-dependent state.
///
/// A pure function, so every state the page can be in is reachable in a test
/// rather than by clicking. That is the whole reason the projection exists
/// separately from the XAML.
/// </summary>
public sealed class AdapterDashboardTests
{
    private static readonly AdapterRecord Row = AdapterRecord.Of("AA:BB:CC:DD:EE:01", "PicoSwitch2")!;

    private static AdapterDashboardView Project(
        AdapterSnapshot? snapshot = null,
        AdapterRelationshipPhase phase = AdapterRelationshipPhase.Connected,
        bool connected = true,
        AdapterRecord? selected = null) =>
        AdapterDashboard.Project(
            snapshot ?? Connected(),
            new AdapterRelationshipStatus { Phase = phase },
            new ConnectionState { Phase = connected ? ConnectionPhase.Connected : ConnectionPhase.Idle },
            selected ?? Row);

    private static AdapterSnapshot Connected() => new()
    {
        Firmware = new FirmwareInfo("picoswitch", "PicoSwitch2", "2.0", 4, "d531b8b7+dirty"),
        Capabilities = new AdapterCapabilities(
            Core: CapabilityState.Available,
            Personality: CapabilityState.Available,
            Colors: CapabilityState.Available,
            ActiveInput: CapabilityState.Available),
    };

    /* ------------------------------------------------ the silence rule (I15) */

    [Fact]
    public void APendingContractStaysSilent()
    {
        // "We have not asked yet" is a transient state on EVERY healthy
        // connection. Warning during it made a healthy adapter flash an
        // incompatibility warning for the second before its info reply landed, and
        // a warning that fires on healthy hardware trains people to ignore it.
        var view = Project(new AdapterSnapshot());

        Assert.False(view.Contract.Visible);
        Assert.False(view.Contract.IsWarning);
    }

    [Fact]
    public void AnUnknownContractWarns()
    {
        // "We asked, and this firmware reports no contract" is the opposite
        // statement: the adapter predates contract reporting, which is exactly the
        // situation the mechanism exists to catch.
        var view = Project(new AdapterSnapshot
        {
            Firmware = new FirmwareInfo("picoswitch", "PicoSwitch2", "1.0", 0, "old"),
        });

        Assert.True(view.Contract.Visible);
        Assert.True(view.Contract.IsWarning);
        Assert.Contains("UNVERIFIED", view.Contract.Summary);
    }

    [Fact]
    public void AMatchingContractIsSilentBecauseThereIsNothingToSay()
    {
        var view = Project();
        Assert.False(view.Contract.Visible);
    }

    [Fact]
    public void AMismatchedContractWarnsAndNamesWhichSideToUpdate()
    {
        var view = Project(new AdapterSnapshot
        {
            Firmware = new FirmwareInfo("picoswitch", "PicoSwitch2", "2.0", 1, "old"),
        });

        Assert.True(view.Contract.IsWarning);
        Assert.Contains("INCOMPATIBLE", view.Contract.Summary);
        Assert.Contains("Flash current firmware", view.Contract.Summary);
    }

    [Fact]
    public void ADisconnectedAdapterHasNothingToCompareAndSaysNothing()
    {
        var view = Project(connected: false);
        Assert.False(view.Contract.Visible);
    }

    /* -------------------------------------------------------- section gating */

    [Fact]
    public void EverySectionIsDisabledWithAReasonWhenDisconnected()
    {
        var view = Project(connected: false);

        foreach (var section in new[]
                 {
                     view.ControllerMode.Availability,
                     view.Appearance.Availability,
                     view.ConsoleInput.Availability,
                 })
        {
            Assert.False(section.Enabled);
            Assert.Equal(AdapterDashboard.NotConnected, section.DisabledReason);
        }
    }

    [Fact]
    public void AnUnsupportedCapabilityDisablesItsSectionAndNamesTheMissingFeature()
    {
        var view = Project(Connected() with
        {
            Capabilities = new AdapterCapabilities(Colors: CapabilityState.Unsupported),
        });

        Assert.False(view.Appearance.Availability.Enabled);
        Assert.Contains("cannot change its colours", view.Appearance.Availability.DisabledReason);
    }

    [Fact]
    public void AnUNKNOWNCapabilityDoesNotDisableAnything()
    {
        // "The probe did not establish an answer" and "this firmware does not have
        // that command" are different statements, and only the second may disable a
        // control. Rendering Unknown as Unsupported hides working features behind a
        // probe that merely timed out.
        var view = Project(Connected() with
        {
            Capabilities = new AdapterCapabilities(
                Personality: CapabilityState.Unknown,
                Colors: CapabilityState.Unknown,
                ActiveInput: CapabilityState.Unknown),
        });

        Assert.True(view.ControllerMode.Availability.Enabled);
        Assert.True(view.Appearance.Availability.Enabled);
        Assert.True(view.ConsoleInput.Availability.Enabled);
    }

    [Fact]
    public void OneUnsupportedFamilyNeverDisablesTheOthers()
    {
        var view = Project(Connected() with
        {
            Capabilities = new AdapterCapabilities(
                Personality: CapabilityState.Unsupported,
                Colors: CapabilityState.Available,
                ActiveInput: CapabilityState.Available),
        });

        Assert.False(view.ControllerMode.Availability.Enabled);
        Assert.True(view.Appearance.Availability.Enabled);
        Assert.True(view.ConsoleInput.Availability.Enabled);
    }

    [Fact]
    public void ConsoleButtonsAreDisabledWithTheRealReasonRatherThanHidden()
    {
        // "This PC cannot act as a controller" is exactly the fact a user needs to
        // discover before filing a bug about it. Hiding the control hides the
        // explanation with it.
        var view = Project();

        Assert.False(view.ConsoleButtons.Enabled);
        Assert.NotNull(view.ConsoleButtons.DisabledReason);
    }

    /* ------------------------------------------------------------- rendering */

    [Fact]
    public void TheFirmwareLineCarriesTheBuildIdBecauseAVersionAloneIsNotActionable()
    {
        // "2.0" is true of a hundred different builds; the build id is what makes a
        // support report mean something.
        Assert.Equal("Firmware 2.0 · build d531b8b7+dirty", Project().FirmwareLine);
    }

    [Fact]
    public void NothingReadYetRendersNoFirmwareLineRatherThanAPlaceholderVersion()
    {
        // A fabricated "Firmware 0.0" would be indistinguishable from a real
        // reading of an adapter that reports nothing.
        Assert.Equal(string.Empty, Project(new AdapterSnapshot()).FirmwareLine);
    }

    [Fact]
    public void AControllerWithNoBatteryReportShowsNoBatteryRatherThanZeroPercent()
    {
        // An invalid reading is not a flat battery. Showing 0% for a controller
        // that simply does not report one is a false alarm about the user's
        // hardware.
        var view = Project(Connected() with
        {
            Controller = new ControllerInfo("DualSense", 0x054C, 0x0CE6, BatteryValid: false),
        });

        Assert.Equal("DualSense", view.ControllerLine);
        Assert.Null(view.BatteryLine);
    }

    [Fact]
    public void AChargingControllerSaysSo()
    {
        var view = Project(Connected() with
        {
            Controller = new ControllerInfo("DualSense", 0x054C, 0x0CE6, true, 62, Charging: true),
        });

        Assert.Equal("Battery 62% · charging", view.BatteryLine);
    }

    [Fact]
    public void NoControllerAttachedIsStatedPlainly() =>
        Assert.Equal("No controller", Project().ControllerLine);

    [Fact]
    public void PersonalitiesRenderAsProductNamesNotWireTokens()
    {
        // `joycon2_l` is a protocol token; nobody owns a controller called that.
        var view = Project(Connected() with
        {
            Personality = new PersonalityState
            {
                Current = Personality.JoyConLeft,
                Available = new ValueList<Personality>([Personality.Pro2, Personality.JoyConLeft]),
            },
        });

        Assert.Equal("Joy-Con 2 (L)", view.ControllerMode.CurrentLabel);
        Assert.Equal(2, view.ControllerMode.Available.Count);
    }

    [Fact]
    public void TheActiveInputSourceIsMarkedAndLabelled()
    {
        var view = Project(Connected() with
        {
            Input = new AdapterInputState
            {
                ActiveId = 7,
                Sources = new ValueList<AdapterInputSource>(
                [
                    new AdapterInputSource(7, 0, 0, 0, "DualSense"),
                    new AdapterInputSource(9, 0, 0, 0, "Pro Controller"),
                ]),
            },
        });

        Assert.Equal("DualSense", view.ConsoleInput.ActiveLabel);
        Assert.Collection(
            view.ConsoleInput.Sources,
            first => Assert.True(first.IsActive),
            second => Assert.False(second.IsActive));
    }

    [Fact]
    public void AnUnnamedSourceStillGetsALabel() =>
        Assert.Equal(
            "Source 3",
            Project(Connected() with
            {
                Input = new AdapterInputState
                {
                    Sources = new ValueList<AdapterInputSource>([new AdapterInputSource(3, 0, 0, 0, "")]),
                },
            }).ConsoleInput.Sources[0].Label);

    [Fact]
    public void TheTitlePrefersTheUsersOwnAliasOverAnythingTheAdapterSays()
    {
        var view = Project(selected: Row with { UserAlias = "Living room" });
        Assert.Equal("Living room", view.Title);
    }

    [Theory]
    [InlineData(AdapterRelationshipPhase.Idle, "Selected, not connected")]
    [InlineData(AdapterRelationshipPhase.RepairRequired, "Repair required")]
    [InlineData(AdapterRelationshipPhase.Connecting, "Connecting")]
    [InlineData(AdapterRelationshipPhase.Connected, "Connected")]
    public void EveryRelationshipPhaseRendersAsSomethingAPersonCanRead(
        AdapterRelationshipPhase phase,
        string expected) =>
        Assert.Equal(expected, Project(phase: phase, connected: false).PhaseText);

    [Fact]
    public void TheAppearanceSectionSaysColoursNeedReenumerationToReachTheConsole()
    {
        // I7. Without this the user changes a colour, sees nothing on the console,
        // and concludes the feature is broken.
        Assert.Contains("re-enumerates", Project().Appearance.ApplyHint);
    }
}
