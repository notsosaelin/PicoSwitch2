using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The device answered, and it is not a PicoSwitch2.
///
/// A distinct type because it is NOT a connectivity failure and must not be
/// treated as one: the address was reached and something replied, so retrying
/// and falling back to a scan can only waste the deadline and produce a
/// confusing aggregate. Discovering *a* device is not permission to adopt it,
/// and no registry row or active selection is created or changed on this path.
/// </summary>
public sealed class AdapterIdentityException(string message) : ManagementException(message);

/// <summary>
/// The application-level operation surface for one adapter.
///
/// Wraps <see cref="ManagementClient"/> and owns the observable
/// <see cref="Snapshot"/>. The UI calls this and nothing below it: it never
/// builds a management command string and never opens a GATT characteristic.
///
/// Only Phase 2's operations are implemented — connect, validate, refresh, the
/// logical-peer inventory, selective forget, and capability probing. Keyboard &amp;
/// Mouse (Phase 4) and Virtual Amiibo (Phase 5) reach the same
/// <see cref="ManagementClient"/> and are added with their own pages rather than
/// stubbed here.
/// </summary>
public sealed class AdapterRepository(IManagementTransport transport)
{
    /// <summary>
    /// Bounded clean retry, then ONE address-restricted fallback scan.
    ///
    /// The comment on the Kotlin original survives translation because it is the
    /// rule and not an implementation note: discovering another valid Pico nearby
    /// is not permission to silently replace the user's relationship.
    /// </summary>
    private readonly ManagementClient client = new(transport);

    private readonly StateValue<AdapterSnapshot> snapshot = new(new AdapterSnapshot());

    public IReadOnlyStateValue<ConnectionState> Connection => transport.Connection;

    public IReadOnlyStateValue<AdapterSnapshot> Snapshot => snapshot;

    public ManagementClient Client => client;

    public TransportTrustSnapshot Trust => transport.Trust;

    public ValueTask DisposeAsync() => transport.DisposeAsync();

    public Task<DiscoveredManagementPeer> DiscoverForPairingAsync(
        ManagementConnectionContext context,
        CancellationToken cancellationToken = default)
    {
        transport.PrepareConnection(context);
        return transport.DiscoverAsync(cancellationToken);
    }

    /// <summary>First pairing: connect to whatever the management scan found, then validate it.</summary>
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        await transport.ScanAndConnectAsync(cancellationToken: cancellationToken).ConfigureAwait(false);
        await ValidateConnectedAdapterAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// Connect to a remembered adapter, with the two-stage recovery ladder.
    ///
    /// Direct connect, then at most one clean retry after a full disposal and a
    /// 350 ms backoff, then exactly one service-filtered fallback scan RESTRICTED
    /// TO THIS ADDRESS. Both failures are reported together: the fallback's
    /// exception carries the direct failure so a support bundle shows why the
    /// first route failed rather than only the last.
    /// </summary>
    public async Task ConnectKnownAsync(
        string address,
        ManagementConnectionContext? context = null,
        CancellationToken cancellationToken = default)
    {
        context ??= new ManagementConnectionContext();
        var retriesUsed = 0;
        Exception directFailure;
        while (true)
        {
            transport.PrepareConnection(context with
            {
                Retry = retriesUsed,
                PriorGattRetired = retriesUsed > 0,
            });

            try
            {
                await transport.ConnectKnownAsync(address, cancellationToken).ConfigureAwait(false);
                await ValidateConnectedAdapterAsync(cancellationToken).ConfigureAwait(false);
                return;
            }
            catch (AdapterIdentityException)
            {
                // Terminal. The adapter was reached and answered; a retry or a
                // fallback scan cannot turn a different device into this one.
                throw;
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                directFailure = error;
            }

            // A CONCLUSIVE bond mismatch ends the attempt HERE.
            //
            // The adapter has no key for us, so neither a clean retry nor an
            // address-restricted fallback scan can succeed -- they can only spend
            // the deadline and bury the one diagnosis that leads somewhere. This is
            // the Windows form of the Android defect where six futile attempts ran
            // across fourteen minutes before the OS dropped its own bond and repair
            // finally triggered.
            //
            // Only the attribute-layer shape is conclusive at this point. The
            // link-layer shape -- which is what this hardware actually produces --
            // requires two independently resolved devices to fail the same way, and
            // the fallback below is what produces the second one. So it correctly
            // does NOT short-circuit here; it classifies after the ladder, in
            // AdapterConnectionService. See AdapterResetSignature.
            if (AdapterResetSignature.IsBondMismatch(directFailure, transport.Trust))
            {
                throw directFailure;
            }

            if (!GattRecoveryPolicy.ShouldRetry(directFailure, retriesUsed))
            {
                break;
            }

            retriesUsed += 1;
            await SafeDisconnectAsync().ConfigureAwait(false);
            await Task.Delay(GattRecoveryPolicy.RetryBackoffMillis, cancellationToken).ConfigureAwait(false);
        }

        // Retire the direct connection completely before the fallback. On Windows
        // a connect cannot be cancelled, so the previous GattSession and
        // BluetoothLEDevice must be disposed before new ones exist, or the stack
        // keeps trying to reconnect behind the app's back.
        await SafeDisconnectAsync().ConfigureAwait(false);
        transport.PrepareConnection(context with
        {
            Reason = "scan-fallback",
            Retry = retriesUsed,
            PriorGattRetired = true,
        });

        try
        {
            await transport.ScanAndConnectAsync(address, cancellationToken).ConfigureAwait(false);
            await ValidateConnectedAdapterAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (AdapterIdentityException)
        {
            throw;
        }
        catch (Exception fallbackFailure)
        {
            throw new AggregateException(
                "Both the direct connection and the address-restricted fallback scan failed",
                fallbackFailure,
                directFailure);
        }
    }

    /// <summary>
    /// Product-level Connected is gated by one real protocol exchange, not merely
    /// by a GATT/CCC callback.
    ///
    /// Keep this boundary intentionally small. The old ten-command refresh made an
    /// optional bonds/KBM/Amiibo probe capable of rejecting a healthy,
    /// identity-verified carrier before the UI could even offer Refresh.
    /// </summary>
    private async Task ValidateConnectedAdapterAsync(CancellationToken cancellationToken)
    {
        try
        {
            var firmware = await client.FirmwareAsync(cancellationToken).ConfigureAwait(false);
            if (firmware.Id != "picoswitch")
            {
                throw new AdapterIdentityException(
                    "The discovered Bluetooth device is not a PicoSwitch2 adapter");
            }

            snapshot.Set(snapshot.Value with
            {
                Firmware = firmware,
                Capabilities = snapshot.Value.Capabilities with { Core = CapabilityState.Available },
                RefreshedAtMillis = NowMillis(),
            });
            transport.MarkValidated();
        }
        catch
        {
            await SafeDisconnectAsync().ConfigureAwait(false);
            throw;
        }

        // What the console currently sees this adapter as, and what is driving it.
        // Both are adapter truth and both were previously read only by the manual
        // Refresh button, so a freshly connected session showed "Acting as
        // Unknown" with no controller until the user pressed Refresh.
        //
        // Read here, AFTER validation, and OPTIONALLY: identity validation still
        // hinges on `info` alone, so a slow or unsupported reply can never reject
        // a healthy carrier. That is what the lean boundary above exists to
        // prevent.
        var personality = await OptionalAsync(() => client.PersonalityAsync(cancellationToken))
            .ConfigureAwait(false);
        if (personality is not null)
        {
            snapshot.Set(snapshot.Value with
            {
                Personality = personality,
                Capabilities = snapshot.Value.Capabilities with
                {
                    Personality = CapabilityState.Available,
                },
            });
        }

        var controller = await OptionalAsync(() => client.ControllerAsync(cancellationToken))
            .ConfigureAwait(false);
        if (controller is not null)
        {
            snapshot.Set(snapshot.Value with { Controller = controller });
        }
    }

    public async Task DisconnectAsync()
    {
        await transport.DisconnectAsync().ConfigureAwait(false);
        ClearDisconnectedSnapshot();
    }

    /// <summary>
    /// A new connection must start from the adapter's authoritative state rather
    /// than inheriting anything from the last one.
    /// </summary>
    public void ClearDisconnectedSnapshot() => snapshot.Set(new AdapterSnapshot());

    public async Task<ManagementRefresh> RefreshAllAsync(CancellationToken cancellationToken = default)
    {
        var refresh = await client.RefreshAllAsync(snapshot.Value, cancellationToken).ConfigureAwait(false);
        snapshot.Set(refresh.Snapshot);
        return refresh;
    }

    public async Task<AdapterInputState> RefreshInputSourcesAsync(
        CancellationToken cancellationToken = default)
    {
        var input = await client.InputSourcesAsync(cancellationToken).ConfigureAwait(false);
        snapshot.Set(snapshot.Value with
        {
            Input = input,
            Capabilities = snapshot.Value.Capabilities with { ActiveInput = CapabilityState.Available },
        });
        return input;
    }

    public async Task<AdapterInputState> SetActiveInputAsync(
        long sourceId,
        CancellationToken cancellationToken = default)
    {
        var input = await client.SetActiveInputAsync(sourceId, cancellationToken).ConfigureAwait(false);
        snapshot.Set(snapshot.Value with { Input = input });
        return input;
    }

    public async Task<ControllerInfo> RefreshControllerAsync(
        CancellationToken cancellationToken = default)
    {
        var controller = await client.ControllerAsync(cancellationToken).ConfigureAwait(false);
        snapshot.Set(snapshot.Value with { Controller = controller });
        return controller;
    }

    /// <summary>
    /// Read the complete logical-peer inventory.
    ///
    /// All-or-nothing: <see cref="ManagementClient.ListPeersAsync"/> throws rather
    /// than returning a short list, and a partial read must never be published or
    /// folded into history — a missing row is a saved controller the user would
    /// conclude is already gone.
    /// </summary>
    public async Task<PeerInventory> RefreshPeersAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            var peers = await client.ListPeersAsync(cancellationToken).ConfigureAwait(false);
            snapshot.Set(snapshot.Value with
            {
                Peers = peers,
                Capabilities = snapshot.Value.Capabilities with { Peers = CapabilityState.Available },
            });
            return peers;
        }
        catch (AdapterCommandException error) when (error.IsUnsupported())
        {
            snapshot.Set(snapshot.Value with
            {
                Peers = new PeerInventory(),
                Capabilities = snapshot.Value.Capabilities with { Peers = CapabilityState.Unsupported },
            });
            return new PeerInventory();
        }
    }

    /// <summary>
    /// Forget one peer, then re-read the complete inventory.
    ///
    /// The re-read happens even when the reported outcome is an error: the adapter
    /// is authoritative about what remains, and the observed <c>bonded</c> field
    /// plus a fresh list outrank optimistic client state. <c>already_absent</c> is
    /// an idempotent success; <c>incomplete</c> stays visible rather than being
    /// smoothed over.
    /// </summary>
    public async Task<PeerForgetOutcome> ForgetPeerAsync(
        string peerId,
        CancellationToken cancellationToken = default)
    {
        try
        {
            return await client.ForgetPeerAsync(peerId, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            await OptionalAsync(() => RefreshPeersAsync(cancellationToken)).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// Probe the three peer capability families INDEPENDENTLY.
    ///
    /// <c>peers list</c>, selective <c>peers forget</c> and remote <c>pairing</c>
    /// shipped in separate phases. Inferring all three from one success would
    /// either hide a working list or offer a Forget button that answers
    /// <c>unknown command</c>. A probe that cannot run at all leaves the state
    /// <c>Unknown</c>; only the protocol's explicit unsupported error shape
    /// establishes <c>Unsupported</c>.
    /// </summary>
    public async Task ProbeManagementCapabilitiesAsync(CancellationToken cancellationToken = default)
    {
        var peers = await ProbeAsync(() => client.ListPeersAsync(cancellationToken)).ConfigureAwait(false);
        var peerForget = await ProbeStateAsync(() => client.ProbePeerForgetAsync(cancellationToken))
            .ConfigureAwait(false);
        var remotePairing = await ProbeStateAsync(() => client.ProbeRemotePairingAsync(cancellationToken))
            .ConfigureAwait(false);

        snapshot.Set(snapshot.Value with
        {
            Capabilities = snapshot.Value.Capabilities with
            {
                Peers = peers,
                PeerForget = peerForget,
                RemotePairing = remotePairing,
            },
        });
    }

    public Task<PairingStatus> StartPairingAsync(CancellationToken cancellationToken = default) =>
        client.StartPairingAsync(cancellationToken);

    public Task<PairingStatus> PairingStatusAsync(CancellationToken cancellationToken = default) =>
        client.PairingStatusAsync(cancellationToken);

    public Task<PairingStatus> CancelPairingAsync(CancellationToken cancellationToken = default) =>
        client.CancelPairingAsync(cancellationToken);

    private async Task SafeDisconnectAsync()
    {
        try
        {
            await transport.DisconnectAsync().ConfigureAwait(false);
        }
        catch
        {
            // A failed teardown must not replace the failure that caused it.
        }
    }

    private static async Task<T?> OptionalAsync<T>(Func<Task<T>> block)
        where T : class
    {
        try
        {
            return await block().ConfigureAwait(false);
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Run a capability probe and classify the outcome.
    ///
    /// A transport failure leaves <c>Unknown</c> rather than claiming the firmware
    /// lacks the family: "the probe did not establish an answer" and "this
    /// firmware does not have that command" are different statements, and only the
    /// second may disable a feature in the UI.
    /// </summary>
    private static async Task<CapabilityState> ProbeAsync<T>(Func<Task<T>> block)
    {
        try
        {
            await block().ConfigureAwait(false);
            return CapabilityState.Available;
        }
        catch (AdapterCommandException error) when (error.IsUnsupported())
        {
            return CapabilityState.Unsupported;
        }
        catch
        {
            return CapabilityState.Unknown;
        }
    }

    private static async Task<CapabilityState> ProbeStateAsync(Func<Task<CapabilityState>> block)
    {
        try
        {
            return await block().ConfigureAwait(false);
        }
        catch
        {
            return CapabilityState.Unknown;
        }
    }

    private static long NowMillis() => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
}
