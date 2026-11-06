using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// Recovering from a management command that never got an answer.
///
/// ## What went wrong
///
/// The transport retires its GATT session on any failure after transmit — a
/// reply timeout, most often — and that is correct: management replies carry no
/// request identifier, so a late one is indistinguishable from the next one's.
///
/// But the retirement was invisible above the transport. The relationship
/// coordinator stayed in <c>Connected</c>, and <c>RequestReconnect</c> is
/// deliberately inert while Connected. Reconnect therefore did nothing, every
/// subsequent command threw "No adapter is connected", and the only thing that
/// cleared it was killing the process — which rebuilds the coordinator from the
/// registry. Observed 2026-08-31, during a KB/M resident upload.
///
/// The invariant these tests hold is one sentence: **the relationship may never
/// say Connected while the carrier is not.**
/// </summary>
public sealed class CarrierLossRecoveryTests
{
    private const string Address = "AA:BB:CC:DD:EE:01";

    [Fact]
    public async Task ARetiredSessionReturnsTheRelationshipToIdle()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);
        Assert.Equal(AdapterRelationshipPhase.Connected,
                     fixture.Service.Relationship.Value.Phase);

        // A command times out; the transport retires the session and throws.
        fixture.Transport.Failures["kbm status"] = new ManagementException(
            "The adapter did not answer 'kbm status' within 10000 ms.");
        fixture.Transport.RetireCarrier();

        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RefreshKeyboardMouseAsync());

        Assert.Equal(AdapterRelationshipPhase.Idle,
                     fixture.Service.Relationship.Value.Phase);
    }

    [Fact]
    public async Task ReconnectWorksAfterATimeoutWithoutRestartingTheApp()
    {
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        fixture.Transport.Failures["kbm status"] = new ManagementException("timed out");
        fixture.Transport.RetireCarrier();
        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RefreshKeyboardMouseAsync());

        // Exactly what the user does next. Before the fix this returned Ignored
        // and the app sat in a Connected state with nothing underneath it.
        fixture.Transport.ResetCounters();
        await fixture.Service.ConnectAsync(fixture.Id);

        Assert.Equal(AdapterRelationshipPhase.Connected,
                     fixture.Service.Relationship.Value.Phase);
        Assert.Equal(1, fixture.Transport.DirectConnects);
    }

    [Fact]
    public async Task ATimeoutNeverAsksTheUserToRepairAWorkingPairing()
    {
        // The escalation reported on 2026-08-31: a stalled upload ended with
        // "identity changed / repair dongle", and a power cycle then reconnected
        // over the same pairing with no repair at all.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        fixture.Transport.Failures["kbm status"] = new ManagementException("timed out");
        fixture.Transport.RetireCarrier();
        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RefreshKeyboardMouseAsync());

        Assert.NotEqual(AdapterRelationshipPhase.RepairRequired,
                        fixture.Service.Relationship.Value.Phase);
        Assert.Equal(0, fixture.Pairing.Unpairs);
    }

    [Fact]
    public async Task LosingTheLinkDoesNotForgetTheAdapter()
    {
        // Losing a link is not a change to the relationship. The row stays
        // selected and the pairing stays held — which is exactly what makes a
        // plain Reconnect the right recovery rather than a repair.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        fixture.Transport.Failures["kbm status"] = new ManagementException("timed out");
        fixture.Transport.RetireCarrier();
        await Assert.ThrowsAsync<ManagementException>(
            () => fixture.Service.RefreshKeyboardMouseAsync());

        Assert.Contains(fixture.Service.Registry.Value.Records,
                        record => record.Id == fixture.Id);
        Assert.Equal(0, fixture.Pairing.Unpairs);
    }

    [Fact]
    public async Task AHealthyConnectedSessionIsLeftAlone()
    {
        // The reconciliation runs after every exclusive operation, so it has to be
        // inert on the ordinary path. A false trip here would disconnect the app
        // from a working adapter after every successful command.
        using var fixture = new ConnectionServiceFixture();
        await fixture.RememberAdapterAsync(Address);

        fixture.Transport.Replies["kbm status"] =
            """{"ok":true,"mode":"keyboard","override":"auto","profile":"kb","keyboard":true,"mouse":false,"nativeMouse":false}""";
        fixture.Transport.Replies["kbm counters"] =
            """{"keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,"rejectedNotOwner":0,"rejectedNoPeerKey":0,"rejectedUnclassified":0,"rejectedNoRole":0,"undecodedReports":0,"rollover":0,"roleLosses":0,"mapGeneration":0,"neutralizations":0,"publishes":0,"recenters":0}""";
        fixture.Transport.Replies["kbm mouse"] =
            """{"ok":true,"sensitivityX":256,"sensitivityY":256,"recenterMs":8,"invertX":false,"invertY":false,"antiDeadzone":10,"sensitivityMin":64,"sensitivityMax":1024,"recenterMinMs":2,"recenterMaxMs":32,"antiDeadzoneMax":100}""";
        fixture.Transport.Replies["kbm profiles 0"] =
            """{"cursor":0,"total":0,"max":6,"profiles":[],"next":null}""";
        fixture.Transport.Replies["kbm switches"] = """{"switches":[],"positions":3}""";
        fixture.Transport.Replies["kbm active"] =
            """
            {"active":[
              {"layout":"kb","sourceId":1,"revision":0,"fingerprint":900,"matchesSaved":true},
              {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,"matchesSaved":true}
            ]}
            """;
        fixture.Transport.Replies["kbm map kb 0"] =
            """{"ok":true,"profile":"kb","profileId":1,"cursor":0,"total":0,"bindings":[],"next":null}""";
        fixture.Transport.Replies["kbm map kbm 0"] =
            """{"ok":true,"profile":"kbm","profileId":1,"cursor":0,"total":0,"bindings":[],"next":null}""";

        await fixture.Service.RefreshKeyboardMouseAsync();

        Assert.Equal(AdapterRelationshipPhase.Connected,
                     fixture.Service.Relationship.Value.Phase);
    }
}
