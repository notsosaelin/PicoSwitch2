using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// The BlueZ pairing ceremony and the one place the Linux app unpairs.
///
/// The security rules are the Windows ones, unchanged:
///
/// - **Bonded and encrypted, never "authenticated".** The adapter has no display
///   and no keypad, so this is LE Just Works: encrypted, without MITM
///   protection. BlueZ negotiates it from the IO capabilities on both sides;
///   nothing here weakens it, and no copy here calls it secure.
/// - **Never auto-repair.** <see cref="UnpairAsync"/> removes a trust
///   relationship and is only reached behind an explicit user confirmation.
///
/// ## BlueZ specifics
///
/// - A device must exist as a BlueZ object before it can be paired, so a pairing
///   for an adapter BlueZ has not seen starts with a short filtered scan.
/// - After pairing, the device is marked <c>Trusted</c> so a reconnect after a
///   reboot needs no further authorization.
/// - Unpair is <c>Adapter1.RemoveDevice</c>, which deletes the stored keys.
///   Unlike Android this needs no system privilege, which is what lets the
///   Repair message be an action here rather than an instruction.
///
/// Status: implementation complete; **hardware validation pending** (Steam Deck
/// against a Pico 2 W adapter). Whether SteamOS's own agent shows a prompt for
/// Just Works in Game Mode is unknown.
/// </summary>
internal sealed class BluezPairingGateway(IBluezBus bus, Action<string, string>? log = null)
    : IAdapterPairingGateway
{
    internal const int DiscoverForPairingMillis = 10_000;
    internal const int PairTimeoutMillis = 45_000;

    public async Task<HostPairingSnapshot> ReadAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var adapter = await bus.GetAdapterAsync(cancellationToken).ConfigureAwait(false);
            if (adapter is null)
            {
                return new HostPairingSnapshot(HostPairingKnown.Unknown, CanPair: false, DeviceName: null);
            }

            var device = await bus.FindDeviceAsync(adapter.Path, bluetoothAddress, cancellationToken)
                .ConfigureAwait(false);

            // BlueZ loads an object for every stored bond at startup, so an absent
            // device is definitively NOT paired -- not unknown.
            return device is null
                ? new HostPairingSnapshot(HostPairingKnown.NotPaired, CanPair: adapter.Powered, DeviceName: null)
                : new HostPairingSnapshot(
                    device.Paired ? HostPairingKnown.Paired : HostPairingKnown.NotPaired,
                    CanPair: adapter.Powered && !device.Paired,
                    device.Name);
        }
        catch (BluezException error)
        {
            // Unknown, never a guess: "not paired" would start a ceremony for a
            // working adapter; "paired" would arm the bond-mismatch signature.
            log?.Invoke("pair", $"read failed: {error}");
            return new HostPairingSnapshot(HostPairingKnown.Unknown, CanPair: false, DeviceName: null);
        }
    }

    public async Task<AdapterPairingResult> PairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var adapter = await bus.GetAdapterAsync(cancellationToken).ConfigureAwait(false);
            if (adapter is null)
            {
                return Failed("NoAdapter", "This computer has no Bluetooth adapter.");
            }

            if (!adapter.Powered)
            {
                return Failed("NotPowered", "Bluetooth is turned off.");
            }

            var device = await bus.FindDeviceAsync(adapter.Path, bluetoothAddress, cancellationToken)
                .ConfigureAwait(false)
                ?? await DiscoverAsync(adapter.Path, bluetoothAddress, cancellationToken).ConfigureAwait(false);
            if (device is null)
            {
                return new AdapterPairingResult(
                    AdapterPairingOutcome.TimedOut,
                    "NotFound",
                    "The adapter was not found. Double-tap its pairing button and try again.");
            }

            if (device.Paired)
            {
                return new AdapterPairingResult(
                    AdapterPairingOutcome.AlreadyPaired,
                    "AlreadyPaired",
                    "This computer is already paired with the adapter.");
            }

            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            deadline.CancelAfter(PairTimeoutMillis);
            await bus.PairAsync(device.Path, deadline.Token).ConfigureAwait(false);

            try
            {
                await bus.SetTrustedAsync(device.Path, true, cancellationToken).ConfigureAwait(false);
            }
            catch (BluezException error)
            {
                // Trust only spares a later authorization prompt; the bond exists.
                log?.Invoke("pair", $"set trusted failed: {error}");
            }

            return new AdapterPairingResult(AdapterPairingOutcome.Paired, "Paired", "Paired with the adapter.");
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new AdapterPairingResult(
                AdapterPairingOutcome.TimedOut,
                "AuthenticationTimeout",
                "The adapter did not complete pairing. Double-tap its pairing button and try again.");
        }
        catch (BluezException error)
        {
            log?.Invoke("pair", $"pair failed: {error}");
            var outcome = BluezErrors.PairingOutcome(error);
            return new AdapterPairingResult(outcome, BluezErrors.StatusName(error), outcome switch
            {
                AdapterPairingOutcome.AlreadyPaired => "This computer is already paired with the adapter.",
                AdapterPairingOutcome.Rejected => "The pairing request was declined.",
                AdapterPairingOutcome.TimedOut =>
                    "The adapter did not complete pairing. Double-tap its pairing button and try again.",
                _ => $"Bluetooth could not pair with the adapter ({BluezErrors.StatusName(error)}).",
            });
        }
    }

    public async Task<AdapterUnpairResult> UnpairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var adapter = await bus.GetAdapterAsync(cancellationToken).ConfigureAwait(false);
            if (adapter is null)
            {
                return AdapterUnpairResult.Unresolved;
            }

            var device = await bus.FindDeviceAsync(adapter.Path, bluetoothAddress, cancellationToken)
                .ConfigureAwait(false);
            if (device is null)
            {
                return AdapterUnpairResult.AlreadyUnpaired;
            }

            await bus.RemoveDeviceAsync(adapter.Path, device.Path, cancellationToken).ConfigureAwait(false);
            return device.Paired ? AdapterUnpairResult.Unpaired : AdapterUnpairResult.AlreadyUnpaired;
        }
        catch (BluezException error)
        {
            log?.Invoke("pair", $"unpair failed: {error}");
            return error.ErrorName switch
            {
                BluezErrors.DoesNotExist or BluezErrors.UnknownObject => AdapterUnpairResult.AlreadyUnpaired,
                BluezErrors.NotAuthorized or BluezErrors.NotPermitted => AdapterUnpairResult.AccessDenied,
                BluezErrors.ServiceUnknown or BluezErrors.NotReady => AdapterUnpairResult.Unresolved,
                _ => AdapterUnpairResult.Failed,
            };
        }
    }

    private async Task<BluezDevice?> DiscoverAsync(
        string adapterPath,
        ulong address,
        CancellationToken cancellationToken)
    {
        var seen = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(DiscoverForPairingMillis);
        var scan = await bus.StartDiscoveryAsync(adapterPath, BleManagementContract.ServiceUuid, advertisement =>
        {
            if (advertisement.Address == address)
            {
                seen.TrySetResult();
            }
        }, deadline.Token).ConfigureAwait(false);

        try
        {
            using (deadline.Token.Register(() => seen.TrySetCanceled()))
            {
                await seen.Task.ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return null;
        }
        finally
        {
            await scan.DisposeAsync().ConfigureAwait(false);
        }

        return await bus.FindDeviceAsync(adapterPath, address, cancellationToken).ConfigureAwait(false);
    }

    private static AdapterPairingResult Failed(string status, string message) =>
        new(AdapterPairingOutcome.Failed, status, message);
}
