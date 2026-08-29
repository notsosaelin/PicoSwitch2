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
/// What a personality switch actually did.
///
/// Three outcomes that a bool cannot carry, and the distinction is the feature:
/// a switch the adapter reported as <c>unchanged</c> does not need a
/// re-enumeration, a switch that re-enumerated is live on the console, and a
/// switch whose re-enumeration failed has changed the adapter's mind but NOT the
/// console's view of it (I7). The last case needs its own instruction, so it must
/// not be reported as either success or failure.
/// </summary>
/// <summary>
/// What removing an adapter actually did.
///
/// Remove promises two things -- the app forgets the row, and Windows drops its
/// pairing -- and the second can fail independently (radio off, device
/// unresolvable). Reporting them separately lets the caller say what happened
/// instead of implying more than did.
/// </summary>
public sealed record AdapterRemoval(bool UnpairRequested, AdapterUnpairResult? Unpaired)
{
    /// <summary>Windows now holds no pairing for this adapter.</summary>
    public bool WindowsTrustRemoved => Unpaired is { } outcome && outcome.TrustRemoved();

    /// <summary>The row is gone but an OS pairing survived it, and the user should be told.</summary>
    public bool LeftOrphanPairing => UnpairRequested && !WindowsTrustRemoved;
}

public sealed record PersonalitySwitchOutcome(
    Personality Personality,
    bool Unchanged,
    bool Reenumerated,
    string? ReenumerationError = null)
{
    /// <summary>Is the console now seeing this personality?</summary>
    public bool HostVisible => Unchanged || Reenumerated;
}

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

    private readonly StateValue<KeyboardMouseState> keyboardMouse = new(new KeyboardMouseState());

    public IReadOnlyStateValue<ConnectionState> Connection => transport.Connection;

    public IReadOnlyStateValue<AdapterSnapshot> Snapshot => snapshot;

    public IReadOnlyStateValue<KeyboardMouseState> KeyboardMouse => keyboardMouse;

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
    public void ClearDisconnectedSnapshot()
    {
        snapshot.Set(new AdapterSnapshot());

        // The KB/M state belongs to the adapter that was connected. Carrying a
        // keyboard map across a disconnect would show one adapter's bindings under
        // another's name, and a bind against them would edit the wrong device.
        keyboardMouse.Set(new KeyboardMouseState());
    }

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

    /* -------------------------------------------------- Phase 3 dashboard */

    /// <summary>
    /// Switch the emulated controller, then re-enumerate USB.
    ///
    /// I7: a personality change is NOT host-visible until the adapter
    /// re-enumerates. Sending the switch and stopping there leaves the console
    /// seeing the old controller while the app claims the new one, which is the
    /// single easiest way to make this feature look broken.
    ///
    /// The re-enumeration is expected to drop the management link — the adapter
    /// detaches from USB, and on some paths the BLE link goes with it. That is
    /// reported, not treated as a fault: the caller reconnects with
    /// <c>AdapterConnectReason.AfterPersonality</c>, which exists so a support
    /// log says WHY a reconnect happened.
    /// </summary>
    public async Task<PersonalitySwitchOutcome> SetPersonalityAsync(
        Personality personality,
        CancellationToken cancellationToken = default)
    {
        var acknowledgement = await client.SetPersonalityAsync(personality, cancellationToken)
            .ConfigureAwait(false);

        if (acknowledgement.Unchanged)
        {
            // Already this personality. Re-enumerating would drop the console's
            // connection to prove nothing.
            return new PersonalitySwitchOutcome(personality, Unchanged: true, Reenumerated: false);
        }

        try
        {
            await client.ReenumerateUsbAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            // The switch was accepted; only the re-enumeration failed to confirm.
            // Report both facts rather than collapsing them, because "the console
            // still sees the old controller" is the user-visible consequence and it
            // needs a different instruction from "the switch was rejected".
            return new PersonalitySwitchOutcome(
                personality,
                Unchanged: false,
                Reenumerated: false,
                ReenumerationError: error.Message);
        }

        return new PersonalitySwitchOutcome(personality, Unchanged: false, Reenumerated: true);
    }

    /// <summary>
    /// Set one colour, persist it, and publish the adapter's READBACK.
    ///
    /// I8: the reply is the truth, never the value that was sent. The adapter may
    /// clamp, ignore or reinterpret a channel, and a UI that shows what it asked
    /// for rather than what the adapter holds will disagree with the hardware and
    /// never notice.
    ///
    /// I7 applies here too — a colour is not host-visible until re-enumeration —
    /// but re-enumerating on every slider release would be unusable, so the
    /// caller decides when to apply. <see cref="ReenumerateAsync"/> is the action.
    /// </summary>
    public async Task<AdapterConfig> SetColorAsync(
        ColorTarget target,
        RgbColor color,
        bool persist = true,
        CancellationToken cancellationToken = default)
    {
        var (config, _) = await client.SetColorAsync(target, color, persist, cancellationToken)
            .ConfigureAwait(false);
        snapshot.Set(snapshot.Value with { Config = config });
        return config;
    }

    /// <summary>Make pending colour/personality state host-visible.</summary>
    public Task ReenumerateAsync(CancellationToken cancellationToken = default) =>
        client.ReenumerateUsbAsync(cancellationToken);

    /// <summary>
    /// Wake the console.
    ///
    /// The client polls <c>wake status</c> until it settles, so the outcome is a
    /// real result rather than "the command was accepted". Firmware without the
    /// status command answers <c>Unknown</c>, which the UI must present as
    /// "could not tell", never as failure.
    /// </summary>
    public Task<WakeStatus> WakeConsoleAsync(CancellationToken cancellationToken = default) =>
        client.WakeConsoleAsync(cancellationToken);

    /// <summary>
    /// Turn the adapter's in-band management gate off or on.
    ///
    /// Turning it OFF ends this session and every future one until it is re-enabled
    /// over UART or by the physical gesture. The caller must confirm destructively;
    /// the repository does not second-guess an explicit instruction, but it does
    /// publish the adapter's own readback rather than the requested value.
    /// </summary>
    public async Task<bool?> SetManagementEnabledAsync(
        bool enabled,
        CancellationToken cancellationToken = default)
    {
        var reported = await client.SetManagementEnabledAsync(enabled, cancellationToken)
            .ConfigureAwait(false);
        snapshot.Set(snapshot.Value with { ManagementEnabled = reported });
        return reported;
    }

    /// <summary>
    /// The raw LE bond slots, for Diagnostics only.
    ///
    /// **This must never drive Paired Controllers.** Bond slots are an LE-only,
    /// index-addressed view of one credential store; logical peers are the
    /// adapter's own account of what it is paired with, across both transports.
    /// Deriving paired truth from this list is the exact defect Bluetooth
    /// Management 2.0 exists to prevent, and it is why this lives here under a
    /// name that says diagnostics.
    /// </summary>
    public async Task<BondEnumeration> RefreshBondDiagnosticsAsync(
        CancellationToken cancellationToken = default)
    {
        var bonds = await client.ListBondsAsync(cancellationToken).ConfigureAwait(false);
        snapshot.Set(snapshot.Value with
        {
            Bonds = bonds.Entries,
            BondsComplete = bonds.Complete,
            BondsTotal = bonds.Total,
        });
        return bonds;
    }

    /// <summary>Remove one raw LE bond slot by index. Diagnostics only; see above.</summary>
    public async Task<BondEnumeration> RemoveBondAsync(
        int index,
        CancellationToken cancellationToken = default)
    {
        var bonds = await client.RemoveBondAsync(index, cancellationToken).ConfigureAwait(false);
        snapshot.Set(snapshot.Value with
        {
            Bonds = bonds.Entries,
            BondsComplete = bonds.Complete,
            BondsTotal = bonds.Total,
        });
        return bonds;
    }

    /* -------------------------------------------------- Phase 4 keyboard/mouse */

    /// <summary>
    /// Read the whole KB/M picture: status, mouse tuning, and both profiles.
    ///
    /// One call rather than four, because a half-read KB/M page is worse than an
    /// unread one — a mapping screen showing one profile's bindings under the
    /// other profile's name would have the user rebind the wrong thing.
    ///
    /// The capability is decided by the STATUS probe alone. If `kbm status`
    /// answers, the family exists; a mapping that then fails to load is a
    /// transport problem, not an absent feature, and must not disable the page.
    /// </summary>
    public async Task<KeyboardMouseState> RefreshKeyboardMouseAsync(
        CancellationToken cancellationToken = default)
    {
        KbmStatus status;
        try
        {
            status = await client.KbmStatusAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (AdapterCommandException error) when (error.IsUnsupported())
        {
            keyboardMouse.Set(new KeyboardMouseState { Capability = CapabilityState.Unsupported });
            return keyboardMouse.Value;
        }

        var mouse = await client.KbmMouseAsync(cancellationToken).ConfigureAwait(false);

        var mappings = new List<KbmMapping>();
        foreach (var profile in KbmProfiles.All)
        {
            mappings.Add(await client.LoadKbmMappingAsync(profile, cancellationToken)
                .ConfigureAwait(false));
        }

        keyboardMouse.Set(new KeyboardMouseState
        {
            Status = status,
            Mouse = mouse,
            Mappings = new ValueList<KbmMapping>(mappings),
            Loaded = true,
            Capability = CapabilityState.Available,
        });

        snapshot.Set(snapshot.Value with
        {
            Capabilities = snapshot.Value.Capabilities with { Kbm = CapabilityState.Available },
        });

        return keyboardMouse.Value;
    }

    public async Task<KbmStatus> SetKbmModeAsync(KbmMode mode, CancellationToken cancellationToken = default)
    {
        var status = await client.SetKbmModeAsync(mode, cancellationToken).ConfigureAwait(false);
        keyboardMouse.Set(keyboardMouse.Value with { Status = status, Loaded = true });
        return status;
    }

    /// <summary>
    /// Bind one source, then reload the WHOLE profile.
    ///
    /// The reload is not belt-and-braces. A bind can change more than the one row
    /// requested — the adapter owns conflict resolution, and a destination already
    /// held by another key may be moved or cleared — so trusting the request would
    /// leave the UI describing a mapping the adapter does not have. This is I8 for
    /// a paged surface: the reply is the truth.
    /// </summary>
    public async Task<KbmMapping> BindKbmAsync(
        KbmProfile profile,
        KbmSource source,
        KbmDestination? destination,
        CancellationToken cancellationToken = default)
    {
        var mapping = await client.BindKbmAsync(profile, source, destination, cancellationToken)
            .ConfigureAwait(false);
        keyboardMouse.Set(keyboardMouse.Value.With(mapping));
        return mapping;
    }

    public async Task<KbmMapping> ResetKbmProfileAsync(
        KbmProfile profile,
        CancellationToken cancellationToken = default)
    {
        var mapping = await client.ResetKbmProfileAsync(profile, cancellationToken).ConfigureAwait(false);
        keyboardMouse.Set(keyboardMouse.Value.With(mapping));
        return mapping;
    }

    /// <summary>Reset every profile and the mouse tuning to adapter defaults.</summary>
    public async Task<KeyboardMouseState> ResetAllKbmAsync(CancellationToken cancellationToken = default)
    {
        var (status, mouse, mappings) = await client.ResetAllKbmAsync(cancellationToken)
            .ConfigureAwait(false);

        keyboardMouse.Set(new KeyboardMouseState
        {
            Status = status,
            Mouse = mouse,
            Mappings = new ValueList<KbmMapping>(mappings.Values.OrderBy(mapping => mapping.Profile)),
            Loaded = true,
            Capability = CapabilityState.Available,
        });

        return keyboardMouse.Value;
    }

    /// <summary>
    /// Set one mouse field and publish the adapter's readback (I8).
    ///
    /// Live: this is dragged, so it runs often and must stay cheap. It reads back
    /// the whole mouse config rather than assuming the field took the value sent,
    /// because the adapter clamps to its own reported range.
    /// </summary>
    public async Task<KbmMouseConfig> SetKbmMouseAsync(
        KbmMouseField field,
        int value,
        CancellationToken cancellationToken = default)
    {
        var mouse = await client.SetKbmMouseAsync(field, value, cancellationToken).ConfigureAwait(false);
        keyboardMouse.Set(keyboardMouse.Value with { Mouse = mouse, Loaded = true });
        return mouse;
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
