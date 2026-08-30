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

    private const string Status =
        """{"ok":true,"mode":"keyboard","override":"auto","profile":"kb","keyboard":true,"mouse":false,"nativeMouse":false}""";

    private const string Mouse =
        """{"ok":true,"sensitivityX":256,"sensitivityY":256,"recenterMs":8,"invertX":false,"invertY":false,"antiDeadzone":10,"sensitivityMin":64,"sensitivityMax":1024,"recenterMinMs":2,"recenterMaxMs":32,"antiDeadzoneMax":100}""";

    private const string Profiles =
        """
        {"profiles":[
          {"id":0,"layout":"kb","name":"Default","active":true,"builtin":true,"overrides":0},
          {"id":1,"layout":"kbm","name":"Default","active":true,"builtin":true,"overrides":0},
          {"id":2,"layout":"kb","name":"Splatoon","active":false,"builtin":false,"overrides":3}
        ],"max":6}
        """;

    private static string Page(string profile, string source, string destination) =>
        $$"""{"ok":true,"profile":"{{profile}}","page":0,"pageSize":1,"total":1,"bindings":[{"src":"{{source}}","dst":"{{destination}}","custom":true}],"more":false}""";

    private static void ScriptFullRead(ConnectionServiceFixture fixture)
    {
        fixture.Transport.Replies["kbm status"] = Status;
        fixture.Transport.Replies["kbm mouse"] = Mouse;
        fixture.Transport.Replies["kbm profiles"] = Profiles;
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
        fixture.Transport.Replies["kbm mouse"] = Mouse;
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
            """{"ok":true,"profile":"kb","page":0,"pageSize":1,"total":0,"bindings":[],"more":false}""";

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
            """{"ok":true,"profile":"kb","page":0,"pageSize":1,"total":0,"bindings":[],"more":false}""";

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

        var status = await fixture.Service.SetKeyboardMouseModeAsync(KbmMode.KeyboardMouse);

        Assert.Equal(KbmMode.Controller, status.Mode);
        Assert.Equal(KbmMode.KeyboardMouse, status.ModeOverride);
    }
}
