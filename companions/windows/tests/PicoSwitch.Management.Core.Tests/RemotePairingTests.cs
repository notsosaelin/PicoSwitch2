using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// Remote controller pairing, from the client's side.
///
/// The properties worth pinning are the ones that decide whether the app can
/// mislead the user about their hardware: an operation generation that stops a
/// stale status describing the current attempt, a vocabulary that degrades
/// rather than failing, and a failure that names itself instead of collapsing
/// into "pairing failed".
/// </summary>
public sealed class RemotePairingTests
{
    private static string Status(
        long op = 1,
        string state = "discovering",
        string reason = "none",
        long remaining = 24000,
        int candidates = 0,
        bool ok = true) =>
        "{\"ok\":" + (ok ? "true" : "false") + ",\"op\":" + op +
        ",\"state\":\"" + state + "\",\"reason\":\"" + reason +
        "\",\"remaining_ms\":" + remaining + ",\"candidates\":" + candidates + "}";

    [Fact]
    public void ARunningOperationDecodesWithItsGenerationAndCountdown()
    {
        var status = ManagementProtocol.PairingStatus(
            "pairing start",
            Status(op: 7, remaining: 24000, candidates: 2));
        Assert.Equal(7L, status.Operation);
        Assert.Equal(PairingState.Discovering, status.State);
        Assert.Equal(PairingReason.None, status.Reason);
        Assert.Equal(24000L, status.RemainingMillis);
        Assert.Equal(2, status.Candidates);
        Assert.True(status.Active);
    }

    [Fact]
    public void EveryTerminalStateIsInactiveSoTheAppStopsPolling()
    {
        foreach (var wire in new[] { "paired", "timed_out", "cancelled", "blocked", "idle" })
        {
            var status = ManagementProtocol.PairingStatus(
                "pairing status",
                Status(state: wire, remaining: 0));
            Assert.False(status.Active, $"{wire} must not read as active");
        }

        // And the two running states must, or the app would stop following a
        // pairing that is still in progress.
        foreach (var wire in new[] { "discovering", "connecting" })
        {
            Assert.True(ManagementProtocol.PairingStatus("pairing status", Status(state: wire)).Active);
        }
    }

    [Fact]
    public void ARefusalNamesItsCauseRatherThanCollapsingToFailure()
    {
        var busy = ManagementProtocol.PairingStatus(
            "pairing start",
            Status(state: "blocked", reason: "busy", remaining: 0, ok: false));
        Assert.Equal(PairingState.Blocked, busy.State);
        Assert.Equal(PairingReason.Busy, busy.Reason);

        var disabled = ManagementProtocol.PairingStatus(
            "pairing start",
            Status(state: "blocked", reason: "management_disabled", remaining: 0, ok: false));
        Assert.Equal(PairingReason.ManagementDisabled, disabled.Reason);

        var timedOut = ManagementProtocol.PairingStatus(
            "pairing status",
            Status(state: "timed_out", reason: "no_controller", remaining: 0));
        Assert.Equal(PairingReason.NoController, timedOut.Reason);

        var storageFull = ManagementProtocol.PairingStatus(
            "pairing start",
            Status(state: "blocked", reason: "storage_full", remaining: 0, ok: false));
        Assert.Equal(PairingReason.StorageFull, storageFull.Reason);
    }

    [Fact]
    public void AnUnknownStateOrReasonDegradesInsteadOfFailingTheReply()
    {
        // A newer adapter may know words this build does not. Refusing the reply
        // would leave the app unable to read the generation or the countdown,
        // which are the parts that keep it honest.
        var status = ManagementProtocol.PairingStatus(
            "pairing status",
            Status(state: "negotiating", reason: "solar_flare", remaining: 5000));
        Assert.Equal(PairingState.Unknown, status.State);
        Assert.Equal(PairingReason.Unknown, status.Reason);
        Assert.False(status.Active);
        Assert.Equal(5000L, status.RemainingMillis);
    }

    [Fact]
    public void AStatusWithoutAnOperationGenerationIsRejected()
    {
        // Without it a late reply cannot be told from a current one, and the whole
        // staleness guard is gone.
        var error = Record.Exception(() => ManagementProtocol.PairingStatus(
            "pairing status",
            """{"ok":true,"state":"discovering","reason":"none","remaining_ms":1000,"candidates":0}"""));
        Assert.True(
            error is ManagementException,
            $"expected a protocol rejection, got {error?.GetType().Name ?? "nothing"}");
    }

    [Fact]
    public void TheCommandsCarryNoDurationArgument()
    {
        // The window belongs to the firmware and is the same one the adapter's own
        // pairing button opens. A client-supplied duration would make the physical
        // gesture depend on what an app asked for earlier.
        Assert.Equal("pairing start", ManagementCommands.PairingStart);
        Assert.Equal("pairing status", ManagementCommands.PairingStatus);
        Assert.Equal("pairing cancel", ManagementCommands.PairingCancel);
    }

    [Fact]
    public void MissingOptionalFieldsDefaultRatherThanFailing()
    {
        var status = ManagementProtocol.PairingStatus("pairing status", """{"ok":true,"op":3}""");
        Assert.Equal(3L, status.Operation);
        Assert.Equal(PairingState.Unknown, status.State);
        Assert.Equal(0L, status.RemainingMillis);
        Assert.Equal(0, status.Candidates);
    }
}
