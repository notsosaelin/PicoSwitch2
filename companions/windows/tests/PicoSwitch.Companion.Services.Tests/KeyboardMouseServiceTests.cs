using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The Keyboard and Mouse service operations.
///
/// What these hold is the difference between the app's idea of a mapping and the
/// adapter's: after every mutation the adapter's own answer wins, and state that
/// belonged to one adapter never survives into another's session.
/// </summary>
public sealed class KeyboardMouseServiceTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";

    // Product state and counters are read as TWO commands: together they exceed
    // the 512-byte wireless reply slot and the whole read is refused with
    // `response_too_large`.
    private const string Status =
        """{"ok":true,"mode":"keyboard","override":"auto","profile":"kb","keyboard":true,"mouse":false,"nativeMouse":false}""";

    private const string Counters =
        """{"keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,"rejectedNotOwner":0,"rejectedNoPeerKey":0,"rejectedUnclassified":0,"rejectedNoRole":0,"undecodedReports":0,"rollover":0,"roleLosses":0,"mapGeneration":0,"neutralizations":0,"publishes":0,"recenters":0}""";

    private const string Mouse =
        """{"ok":true,"sensitivityX":256,"sensitivityY":256,"recenterMs":8,"invertX":false,"invertY":false,"antiDeadzone":10,"sensitivityMin":64,"sensitivityMax":1024,"recenterMinMs":2,"recenterMaxMs":32,"antiDeadzoneMax":100}""";

    // Only CUSTOM profiles are stored; Default is a template the client
    // synthesises, which is what keeps all six adapter slots for the user.
    // Cursor pagination: `next` is the index of the first item NOT in this reply,
    // and null exactly when the walk is complete. A fixed page stride is what
    // silently dropped rows on hardware.
    private const string Profiles =
        """
        {"cursor":0,"total":1,"max":6,"profiles":[
          {"id":2,"position":1,"layout":"kb","name":"Splatoon","revision":3,"overrides":3,"fingerprint":111}
        ],"next":null}
        """;

    // One table for both layouts: a binding names a semantic position and the
    // adapter resolves it through whichever layout is derived at press time.
    private const string Switches =
        """{"switches":[{"src":"key:3B","position":1}],"positions":3}""";

    // What each layout is REALLY running. The Keyboard layout is running the
    // built-in Default here, not the Splatoon profile.
    private const string Active =
        """
        {"active":[
          {"layout":"kb","sourceId":1,"revision":0,"fingerprint":900,"matchesSaved":true},
          {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,"matchesSaved":true}
        ]}
        """;

    private static string Page(string profile, string source, string destination) =>
        $$"""{"ok":true,"profile":"{{profile}}","profileId":1,"cursor":0,"total":1,"bindings":[{"src":"{{source}}","dst":"{{destination}}","custom":true}],"next":null}""";

    private static void ScriptFullRead(ConnectionServiceFixture fixture)
    {
        fixture.Transport.Replies["kbm status"] = Status;
        fixture.Transport.Replies["kbm counters"] = Counters;
        fixture.Transport.Replies["kbm mouse"] = Mouse;
        fixture.Transport.Replies["kbm profiles 0"] = Profiles;
        fixture.Transport.Replies["kbm switches"] = Switches;
        fixture.Transport.Replies["kbm active"] = Active;
        fixture.Transport.Replies["kbm map kb 0"] = Page("kb", "key:04", "a");
        fixture.Transport.Replies["kbm map kbm 0"] = Page("kbm", "key:05", "b");
    }

    [Fact]
    public async Task AFullReadLoadsStatusMouseAndBothProfiles()
    {
        // One call rather than four: a half-read KB/M page would show one profile's
        // bindings under the other profile's name.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);

        var state = await fixture.Service.RefreshKeyboardMouseAsync();

        Assert.True(state.Loaded);
        Assert.Equal(CapabilityState.Available, state.Capability);
        Assert.Equal(KbmMode.Keyboard, state.Status.Mode);
        Assert.Equal(1024, state.Mouse.SensitivityMax);
        Assert.Equal(2, state.Mappings.Count);
        Assert.Single(state.Mapping(KbmLayout.Keyboard).Bindings);
        Assert.Single(state.Mapping(KbmLayout.KeyboardMouse).Bindings);
    }

    [Fact]
    public async Task FirmwareWithoutKbmIsUnsupportedRatherThanAnError()
    {
        // An older adapter must degrade this one family without failing anything
        // else, and without the page claiming the read merely failed.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Failures["kbm status"] =
            new AdapterCommandException("kbm status", null, "unknown command");

        var state = await fixture.Service.RefreshKeyboardMouseAsync();

        Assert.Equal(CapabilityState.Unsupported, state.Capability);
        Assert.False(state.Loaded);
    }

    [Fact]
    public async Task TheCapabilityIsDecidedByStatusAloneNotByAMappingThatFailedToLoad()
    {
        // A mapping read that fails is a transport problem, not an absent feature.
        // Disabling the page over it would hide a working capability.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["kbm status"] = Status;
        fixture.Transport.Replies["kbm counters"] = Counters;
        fixture.Transport.Replies["kbm mouse"] = Mouse;
        fixture.Transport.Replies["kbm profiles 0"] = Profiles;
        fixture.Transport.Replies["kbm switches"] = Switches;
        fixture.Transport.Failures["kbm map kb 0"] = new ManagementException("link dropped");

        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RefreshKeyboardMouseAsync());

        // Not marked Unsupported by the failure.
        Assert.NotEqual(CapabilityState.Unsupported, fixture.Service.KeyboardMouse.Value.Capability);
    }

    [Fact]
    public async Task ABindReloadsTheWholeProfileRatherThanTrustingTheRequest()
    {
        // The adapter owns conflict resolution: a destination already held by
        // another key may be moved or cleared, so the reply is the truth (I8).
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Replies["kbm bind kb key:04 x"] = """{"ok":true}""";
        fixture.Transport.Replies["kbm map kb 0"] = Page("kb", "key:04", "x");

        var mapping = await fixture.Service.BindAsync(
            KbmLayout.Keyboard,
            new KbmSource(KbmSourceKind.Key, 0x04),
            KbmDestination.X);

        Assert.Equal(KbmDestination.X, mapping.Bindings[0].Destination);
        Assert.Contains("kbm map kb 0", fixture.Transport.Sent);
    }

    [Fact]
    public async Task ABindOnlyReplacesItsOwnProfile()
    {
        // Showing one profile's bindings under the other's name would have the user
        // rebind the wrong thing; the same applies to storing them.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Replies["kbm bind kb key:04 x"] = """{"ok":true}""";
        fixture.Transport.Replies["kbm map kb 0"] = Page("kb", "key:04", "x");
        await fixture.Service.BindAsync(
            KbmLayout.Keyboard,
            new KbmSource(KbmSourceKind.Key, 0x04),
            KbmDestination.X);

        var state = fixture.Service.KeyboardMouse.Value;
        Assert.Equal(2, state.Mappings.Count);
        Assert.Equal(
            KbmDestination.B,
            state.Mapping(KbmLayout.KeyboardMouse).Bindings[0].Destination);
    }

    [Fact]
    public async Task ClearingABindingSendsNONE()
    {
        // "Unmapped" is a destination in its own right: the key does nothing.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Replies["kbm bind kb key:04 none"] = """{"ok":true}""";
        fixture.Transport.Replies["kbm map kb 0"] =
            """{"ok":true,"profile":"kb","profileId":1,"cursor":0,"total":0,"bindings":[],"next":null}""";

        await fixture.Service.BindAsync(
            KbmLayout.Keyboard,
            new KbmSource(KbmSourceKind.Key, 0x04),
            KbmDestination.None);

        Assert.Contains("kbm bind kb key:04 none", fixture.Transport.Sent);
        Assert.Empty(fixture.Service.KeyboardMouse.Value.Mapping(KbmLayout.Keyboard).Bindings);
    }

    [Fact]
    public async Task RestoringOneKeysDefaultSendsDEFAULTAndIsNotTheSameAsClearingIt()
    {
        // A NULL destination means "put this key back to the adapter's own
        // default", which is a different operation from unmapping it -- the wire
        // words are `default` and `none`, and conflating them either wipes a key
        // the user wanted restored or restores one they wanted silent.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Replies["kbm bind kb key:04 default"] = """{"ok":true}""";
        fixture.Transport.Replies["kbm map kb 0"] = Page("kb", "key:04", "a");

        await fixture.Service.BindAsync(
            KbmLayout.Keyboard,
            new KbmSource(KbmSourceKind.Key, 0x04),
            destination: null);

        Assert.Contains("kbm bind kb key:04 default", fixture.Transport.Sent);
        Assert.DoesNotContain("kbm bind kb key:04 none", fixture.Transport.Sent);
    }

    [Fact]
    public async Task MouseTuningPublishesTheAdaptersReadbackNotTheRequestedValue()
    {
        // The adapter clamps to its own reported range. A UI showing what it asked
        // for would disagree with the hardware and never notice.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Replies["kbm mouse sensitivity 9999"] =
            """{"ok":true,"sensitivityX":1024,"sensitivityY":1024,"recenterMs":8,"invertX":false,"invertY":false,"antiDeadzone":10,"sensitivityMin":64,"sensitivityMax":1024,"recenterMinMs":2,"recenterMaxMs":32,"antiDeadzoneMax":100}""";

        var mouse = await fixture.Service.SetMouseAsync(KbmMouseField.Sensitivity, 9999);

        Assert.Equal(1024, mouse.SensitivityX);
        Assert.Equal(1024, fixture.Service.KeyboardMouse.Value.Mouse.SensitivityX);
    }

    [Fact]
    public async Task ResettingAProfileLeavesTheOtherAlone()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Replies["kbm reset kb"] = """{"ok":true}""";
        fixture.Transport.Replies["kbm map kb 0"] =
            """{"ok":true,"profile":"kb","profileId":1,"cursor":0,"total":0,"bindings":[],"next":null}""";

        await fixture.Service.ResetProfileAsync(KbmLayout.Keyboard);

        var state = fixture.Service.KeyboardMouse.Value;
        Assert.Empty(state.Mapping(KbmLayout.Keyboard).Bindings);
        Assert.Single(state.Mapping(KbmLayout.KeyboardMouse).Bindings);
    }

    [Fact]
    public async Task DisconnectingDropsTheKeyboardMapWithTheSession()
    {
        // The map belongs to the adapter that was connected. Carrying it across a
        // disconnect would show one adapter's bindings under another's name, and a
        // bind against them would edit the wrong device.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();
        Assert.True(fixture.Service.KeyboardMouse.Value.Loaded);

        await fixture.Service.DisconnectAsync();

        Assert.False(fixture.Service.KeyboardMouse.Value.Loaded);
        Assert.Empty(fixture.Service.KeyboardMouse.Value.Mappings);
    }

    [Fact]
    public async Task TheBusyFlagIsScopedToKeyboardAndMouseAndClearsOnFailure()
    {
        // Section-scoped so live mouse tuning is never covered by a modal overlay
        // (§16.3). It must also survive a failure, or one error leaves the section
        // spinning forever.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Failures["kbm status"] = new ManagementException("link dropped");

        Assert.False(fixture.Service.KeyboardMouseBusy.Value);
        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RefreshKeyboardMouseAsync());
        Assert.False(fixture.Service.KeyboardMouseBusy.Value);
    }

    [Fact]
    public async Task ModeChangesPublishTheAdaptersOwnAnswer()
    {
        // A mode needing a device that is not connected resolves elsewhere, so the
        // requested value must never be shown as the active one.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["kbm mode kbmouse"] = """{"ok":true}""";
        fixture.Transport.Replies["kbm status"] =
            """{"ok":true,"mode":"controller","override":"kbmouse","profile":"kbm","keyboard":false,"mouse":true,"nativeMouse":false}""";
        fixture.Transport.Replies["kbm counters"] = Counters;

        var status = await fixture.Service.SetKeyboardMouseModeAsync(KbmMode.KeyboardMouse);

        Assert.Equal(KbmMode.Controller, status.Mode);
        Assert.Equal(KbmMode.KeyboardMouse, status.ModeOverride);
    }

    // ----------------------------------------------------------- profile writes

    [Fact]
    public async Task EditingADraftSendsNothingAtAll()
    {
        // THE requirement. The previous editor wrote `kbm bind` per keystroke,
        // which erased flash once per changed key and made Save and Discard
        // meaningless. Thirty edits must cost zero commands.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        await fixture.Service.RefreshKeyboardMouseAsync();

        var library = fixture.Service.KeyboardMouse.Value.Profiles;
        var row = library.For(KbmLayout.Keyboard).First(p => p.Id == 2);
        var draft = KeyboardMouseDraft.From(row, [], new KbmMouseConfig());

        fixture.Transport.Sent.Clear();
        for (var usage = 0x04; usage < 0x22; usage++)
        {
            draft = draft.With(new KbmSource(KbmSourceKind.Key, usage),
                               KbmDestination.A);
        }

        draft = draft
            .WithName("Renamed locally")
            .WithMouse(new KbmMouseConfig(SensitivityX: 900, SensitivityY: 900));
        Assert.True(draft.Dirty);

        // Discard is local too: there is nothing on the adapter to undo.
        Assert.False(draft.Discard().Dirty);
        Assert.Empty(fixture.Transport.Sent);
    }

    [Fact]
    public async Task SaveIsOneTransactionAndDoesNotApply()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        fixture.Transport.Replies["kbm draft begin kb 2 3 Work"] = Ok;
        fixture.Transport.Replies["kbm draft bind key:04 a"] = Ok;
        fixture.Transport.Replies["kbm draft mouse sensitivityx 0"] = Ok;
        fixture.Transport.Replies["kbm draft mouse sensitivityy 0"] = Ok;
        fixture.Transport.Replies["kbm draft mouse recenter 0"] = Ok;
        fixture.Transport.Replies["kbm draft mouse invertx 0"] = Ok;
        fixture.Transport.Replies["kbm draft mouse inverty 0"] = Ok;
        fixture.Transport.Replies["kbm draft mouse antideadzone 0"] = Ok;
        fixture.Transport.Replies["kbm draft commit"] =
            """{"ok":true,"id":2,"revision":4}""";
        await fixture.Service.RefreshKeyboardMouseAsync();

        var row = fixture.Service.KeyboardMouse.Value.Profiles
            .For(KbmLayout.Keyboard).First(p => p.Id == 2);
        var draft = KeyboardMouseDraft
            .From(row, [], new KbmMouseConfig())
            .WithName("Work")
            .With(new KbmSource(KbmSourceKind.Key, 0x04), KbmDestination.A);

        fixture.Transport.Sent.Clear();
        var saved = await fixture.Service.SaveKeyboardMouseProfileAsync(draft);

        // The draft is clean against the revision the ADAPTER reported, not one
        // the client assumed.
        Assert.Equal(4, saved.BaseRevision);
        Assert.False(saved.Dirty);

        // Exactly one staged transaction, and NO apply. Saving must not change
        // what the console is running.
        Assert.Contains("kbm draft begin kb 2 3 Work", fixture.Transport.Sent);
        Assert.Contains("kbm draft commit", fixture.Transport.Sent);
        Assert.DoesNotContain(fixture.Transport.Sent,
                              sent => sent.StartsWith("kbm apply",
                                                      StringComparison.Ordinal));
        Assert.DoesNotContain(fixture.Transport.Sent,
                              sent => sent.StartsWith("kbm bind",
                                                      StringComparison.Ordinal));
    }

    [Fact]
    public async Task AFailedSaveAbortsTheDraftRatherThanLeavingItStaged()
    {
        // A half-transferred mapping left staged is one the next Commit would
        // store by accident.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        fixture.Transport.Replies["kbm draft begin kb 2 3 Work"] = Ok;
        fixture.Transport.Failures["kbm draft bind key:04 a"] =
            new AdapterCommandException("kbm draft bind", null, "mapping storage full");
        fixture.Transport.Replies["kbm draft abort"] = Ok;
        await fixture.Service.RefreshKeyboardMouseAsync();

        var row = fixture.Service.KeyboardMouse.Value.Profiles
            .For(KbmLayout.Keyboard).First(p => p.Id == 2);
        var draft = KeyboardMouseDraft
            .From(row, [], new KbmMouseConfig())
            .WithName("Work")
            .With(new KbmSource(KbmSourceKind.Key, 0x04), KbmDestination.A);

        await Assert.ThrowsAsync<AdapterCommandException>(
            () => fixture.Service.SaveKeyboardMouseProfileAsync(draft));

        Assert.Contains("kbm draft abort", fixture.Transport.Sent);
        Assert.DoesNotContain("kbm draft commit", fixture.Transport.Sent);
    }

    [Fact]
    public async Task ApplyIsItsOwnCommandAndRereadsTheResult()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        fixture.Transport.Replies["kbm apply kb 2"] =
            """{"ok":true,"layout":"kb","id":2,"changed":true}""";
        fixture.Transport.Replies["kbm active"] =
            """
            {"active":[
              {"layout":"kb","sourceId":2,"revision":3,"fingerprint":111,"matchesSaved":true},
              {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,"matchesSaved":true}
            ]}
            """;
        await fixture.Service.RefreshKeyboardMouseAsync();

        fixture.Transport.Sent.Clear();
        var state = await fixture.Service.ApplyKeyboardMouseProfileAsync(
            KbmLayout.Keyboard, 2);

        Assert.Contains("kbm apply kb 2", fixture.Transport.Sent);
        // Verified by READBACK, not by the acknowledgement.
        Assert.Contains("kbm active", fixture.Transport.Sent);
        Assert.Equal(2, state.Profiles.ActiveFor(KbmLayout.Keyboard)?.SourceId);
    }

    [Fact]
    public async Task AnAdapterWithoutAProfileLibraryIsOutOfDateNotDegraded()
    {
        // This companion targets ONE firmware contract. Older firmware used to
        // fall through to a pre-profile editor, which turned "your adapter needs
        // updating" into "this app looks half-built" — and hid a genuine protocol
        // failure behind the same screen.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        fixture.Transport.Replies.Remove("kbm profiles 0");
        fixture.Transport.Failures["kbm profiles 0"] =
            new AdapterCommandException("kbm profiles 0", null, "unknown command");

        var state = await fixture.Service.RefreshKeyboardMouseAsync();

        Assert.Equal(KeyboardMouseReadiness.FirmwareUpdateRequired, state.Readiness);
        Assert.False(state.Loaded);
        Assert.Contains("kbm profiles 0", state.Fault);
    }

    [Fact]
    public async Task CountersAreRequiredRatherThanSynthesizedAsZero()
    {
        // Zeroed counters read as a healthy adapter receiving no input, which is
        // the single most misleading thing this page can say — it is the display
        // that exists to diagnose exactly that condition.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        ScriptFullRead(fixture);
        fixture.Transport.Replies.Remove("kbm counters");
        fixture.Transport.Failures["kbm counters"] =
            new AdapterCommandException("kbm counters", null, "unknown command");

        var state = await fixture.Service.RefreshKeyboardMouseAsync();

        Assert.Equal(KeyboardMouseReadiness.FirmwareUpdateRequired, state.Readiness);
        Assert.Contains("kbm counters", state.Fault);
    }

    private const string Ok = """{"ok":true}""";
}
