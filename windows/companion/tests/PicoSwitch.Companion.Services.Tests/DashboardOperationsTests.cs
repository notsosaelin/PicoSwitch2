using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The Phase 3 dashboard operations, at the service boundary.
///
/// Each one exists to hold an invariant that is easy to lose in a UI: I7 (not
/// host-visible until re-enumeration), I8 (the readback is the truth), and the
/// rule that an unsupported or unreadable answer is never rendered as failure.
/// </summary>
public sealed class DashboardOperationsTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";

    [Fact]
    public async Task APersonalitySwitchReenumeratesSoTheConsoleActuallySeesIt()
    {
        // I7. Sending the switch and stopping there leaves the console showing the
        // old controller while the app claims the new one -- the single easiest way
        // to make this feature look broken.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["personality pro2"] = """{"ok":true,"switching":true}""";
        fixture.Transport.Replies["reenumerate"] = """{"ok":true,"reenumerating":true}""";

        var outcome = await fixture.Service.SetPersonalityAsync(Personality.Pro2);

        Assert.True(outcome.Reenumerated);
        Assert.True(outcome.HostVisible);
        Assert.Contains("reenumerate", fixture.Transport.Sent);
    }

    [Fact]
    public async Task APersonalityAlreadyInEffectDoesNotDetachUsbToProveNothing()
    {
        // Re-enumerating drops the console's connection. Doing that for a no-op is
        // a visible cost with no benefit.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["personality pro2"] = """{"ok":true,"unchanged":true}""";

        var outcome = await fixture.Service.SetPersonalityAsync(Personality.Pro2);

        Assert.True(outcome.Unchanged);
        Assert.False(outcome.Reenumerated);
        Assert.True(outcome.HostVisible);
        Assert.DoesNotContain("reenumerate", fixture.Transport.Sent);
    }

    [Fact]
    public async Task ASwitchWhoseReenumerationFailsIsNeitherSuccessNorFailure()
    {
        // The adapter changed its mind; the console did not. That needs its own
        // instruction ("re-plug the adapter"), so it must not be reported as either.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["personality pro2"] = """{"ok":true,"switching":true}""";
        fixture.Transport.Failures["reenumerate"] = new ManagementException("no reply");

        var outcome = await fixture.Service.SetPersonalityAsync(Personality.Pro2);

        Assert.False(outcome.Unchanged);
        Assert.False(outcome.Reenumerated);
        Assert.False(outcome.HostVisible);
        Assert.Equal("no reply", outcome.ReenumerationError);
    }

    [Fact]
    public async Task AColourChangePublishesTheAdaptersReadbackNotTheRequestedValue()
    {
        // I8. The adapter may clamp or reinterpret a channel, and a UI showing what
        // it asked for rather than what the hardware holds will disagree with the
        // device and never notice.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["body 255 0 0"] = """{"ok":true}""";
        fixture.Transport.Replies["save"] = """{"ok":true,"requested":1}""";
        fixture.Transport.Replies["get"] =
            """{"ok":true,"body_color":[10,20,30],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}""";

        var config = await fixture.Service.SetColorAsync(ColorTarget.Body, new RgbColor(255, 0, 0));

        // Readback, not the request.
        Assert.Equal(new RgbColor(10, 20, 30), config.BodyColor);
        Assert.Equal(config, fixture.Service.Snapshot.Value.Config);
    }

    [Fact]
    public async Task AColourChangeDoesNotReenumerateOnEveryEdit()
    {
        // I7 still applies -- colours are not host-visible until re-enumeration --
        // but detaching USB on every slider release would be unusable. Applying is
        // a separate, explicit action.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["body 255 0 0"] = """{"ok":true}""";
        fixture.Transport.Replies["save"] = """{"ok":true,"requested":1}""";
        fixture.Transport.Replies["get"] =
            """{"ok":true,"body_color":[255,0,0],"joycon2_left_accent":[0,0,0],"joycon2_right_accent":[0,0,0]}""";

        await fixture.Service.SetColorAsync(ColorTarget.Body, new RgbColor(255, 0, 0));

        Assert.DoesNotContain("reenumerate", fixture.Transport.Sent);
    }

    [Fact]
    public async Task WakeReportingUnknownIsNotAnError()
    {
        // Older firmware has no `wake status`. "The adapter could not tell us" must
        // not be presented as "waking failed".
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["wake"] = """{"ok":true}""";
        fixture.Transport.Failures["wake status"] =
            new AdapterCommandException("wake status", null, "unknown command");

        var status = await fixture.Service.WakeConsoleAsync();

        Assert.Equal(WakeResult.Unknown, status.Result);
    }

    [Fact]
    public async Task TheManagementGatePublishesTheAdaptersOwnReadback()
    {
        // Turning the gate off ends this session and every future one until it is
        // re-enabled physically. The app must show what the ADAPTER reports, never
        // the value it asked for.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["mgmt off"] = """{"ok":true,"enabled":false}""";

        Assert.False(await fixture.Service.SetManagementEnabledAsync(false));
        Assert.False(fixture.Service.Snapshot.Value.ManagementEnabled);
    }

    [Fact]
    public async Task RawBondSlotsAreReadableButNeverBecomePairedControllers()
    {
        // Bond slots are an LE-only, index-addressed view of one credential store.
        // Logical peers are the adapter's own account across BOTH transports.
        // Deriving paired truth from the former is the defect Bluetooth Management
        // 2.0 exists to prevent.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Transport.Replies["bonds list"] =
            """{"ok":true,"version":2,"complete":true,"total":1,"bonds":[{"index":0,"addr":"11:22:33:44:55:66"}]}""";

        var bonds = await fixture.Service.RefreshBondDiagnosticsAsync();

        Assert.Single(bonds.Entries);
        Assert.Single(fixture.Service.Snapshot.Value.Bonds);

        // The peer inventory is untouched by a bond read.
        Assert.Empty(fixture.Service.Snapshot.Value.Peers.Peers);
    }
}

/// <summary>
/// Remove, whose semantics changed once the design spec was read properly.
/// </summary>
public sealed class RemoveAdapterSemanticsTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";

    [Fact]
    public async Task RemoveDropsTheWindowsPairingWithTheRow()
    {
        // WINDOWS_PASS.md §16.2: "Remove adapter is per-adapter, unpairs that
        // Windows relationship after confirmation, and deletes that row and its
        // peer history."
        //
        // This was previously local-only, from misreading §19.5 -- whose "never
        // unpairs" sentence is about what PAIRING ANOTHER UNIT must not do to an
        // existing row, not about what Remove does to its own. Leaving the OS
        // pairing behind strands a bond the app can no longer see or offer to clear.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var removal = await fixture.Service.RemoveAsync(fixture.Id);

        Assert.Empty(fixture.Service.Registry.Value.Records);
        Assert.Equal(1, fixture.Pairing.Unpairs);
        Assert.True(removal.WindowsTrustRemoved);
        Assert.False(removal.LeftOrphanPairing);
    }

    [Fact]
    public async Task RemoveStillRemovesTheRowWhenTheUnpairFails()
    {
        // Remove must reliably do what it says. A radio that is off cannot be
        // allowed to strand the row in the list -- but the caller is told that an
        // OS pairing survived, so it can say so rather than implying more.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        fixture.Pairing.UnpairResult = AdapterUnpairResult.Unresolved;

        var removal = await fixture.Service.RemoveAsync(fixture.Id);

        Assert.Empty(fixture.Service.Registry.Value.Records);
        Assert.True(removal.LeftOrphanPairing);
        Assert.False(removal.WindowsTrustRemoved);
    }

    [Fact]
    public async Task ForgettingLocallyOnlyIsPossibleButHasToBeAskedForExplicitly()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        var removal = await fixture.Service.RemoveAsync(fixture.Id, unpairWindows: false);

        Assert.Empty(fixture.Service.Registry.Value.Records);
        Assert.Equal(0, fixture.Pairing.Unpairs);
        Assert.False(removal.UnpairRequested);
        Assert.False(removal.LeftOrphanPairing);
    }

    [Fact]
    public async Task RemoveNeverTouchesTheAdaptersOwnControllerBonds()
    {
        // Three distinct destructive operations with three distinct owners: the
        // app's row, the Windows pairing, and the adapter's controller credentials.
        // Remove owns the first two and must never reach the third.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        await fixture.Service.RemoveAsync(fixture.Id);

        Assert.DoesNotContain(fixture.Transport.Sent, sent => sent.StartsWith("peers forget", StringComparison.Ordinal));
        Assert.DoesNotContain(fixture.Transport.Sent, sent => sent.StartsWith("bonds remove", StringComparison.Ordinal));
    }

    [Fact]
    public async Task RemoveTakesThatAdaptersPeerHistoryWithIt()
    {
        // History about an adapter the app no longer knows is orphaned.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.Service.RemoveAsync(fixture.Id);

        Assert.Empty(new PeerHistoryStore(fixture.Documents).Load().ForAdapter(fixture.Id).Records);
    }

    [Fact]
    public async Task RemoveSaysExactlyWhichTrustItDroppedAndWhichItDidNot()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        await fixture.Service.RemoveAsync(fixture.Id);

        Assert.Contains(
            fixture.Diagnostics.Snapshot(),
            entry => entry.Message.Contains("windows-pairing=removed", StringComparison.Ordinal) &&
                entry.Message.Contains("adapter-side controller bonds untouched", StringComparison.Ordinal));
    }
}
