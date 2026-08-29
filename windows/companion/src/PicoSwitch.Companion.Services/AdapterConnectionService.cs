using System.Diagnostics.CodeAnalysis;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The orchestration the UI actually talks to.
///
/// Everything Phase 2 built — the transport, the repository, the registry, the
/// two coordinators, peer history — is composed here into the handful of
/// operations a person performs: pair an adapter, connect to a remembered one,
/// refresh it, repair it, forget it. A ViewModel calls these and nothing below
/// them, which is what keeps the App project free of both GATT objects and
/// management command strings (WINDOWS_PASS.md §12.5).
///
/// It also owns the two side effects the individual pieces deliberately do not:
/// persisting the registry and peer history, and writing the diagnostic trail.
/// </summary>
public sealed class AdapterConnectionService
{
    private readonly AdapterRepository repository;
    private readonly AdapterRegistryStore registryStore;
    private readonly PeerHistoryStore historyStore;
    private readonly DiagnosticLog diagnostics;
    private readonly Func<long> nowMillis;

    private readonly StateValue<AdapterRegistry> registry;
    private readonly StateValue<PeerHistoryBook> history;
    private readonly StateValue<AdapterRelationshipStatus> relationship;
    private readonly StateValue<BluetoothRadioCapabilities> radio =
        new(new BluetoothRadioCapabilities());

    private readonly ActiveAdapterCoordinator active;
    private readonly AdapterRelationshipCoordinator lifecycle;
    private readonly SemaphoreSlim operationGate = new(1, 1);

    private readonly Func<Task<BluetoothRadioCapabilities>> probeRadio;

    private readonly IAdapterPairingGateway pairing;

    /// <param name="probeRadio">
    /// Injectable so the service is testable without a radio. Production passes
    /// nothing; a test that had to depend on the developing machine's own
    /// Bluetooth hardware would be a test that fails on a build agent for reasons
    /// unrelated to the code.
    /// </param>
    /// <param name="pairing">
    /// Injectable for the same reason as <paramref name="probeRadio"/>, and for a
    /// sharper one: the repair path's whole job is to call Windows' unpair, so a
    /// test that cannot observe that call cannot tell a working repair from one
    /// that silently does nothing. That is precisely the defect found on hardware
    /// on 2026-08-29.
    /// </param>
    public AdapterConnectionService(
        AdapterRepository repository,
        WindowsDocumentStore documents,
        DiagnosticLog diagnostics,
        Func<long>? nowMillis = null,
        Func<Task<BluetoothRadioCapabilities>>? probeRadio = null,
        IAdapterPairingGateway? pairing = null)
    {
        this.repository = repository;
        this.diagnostics = diagnostics;
        this.nowMillis = nowMillis ?? (() => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        this.probeRadio = probeRadio ?? WindowsBluetoothRadio.ProbeAsync;
        this.pairing = pairing ?? new WindowsPairingGateway();

        registryStore = new AdapterRegistryStore(documents);
        historyStore = new PeerHistoryStore(documents);

        var loaded = registryStore.Load();
        registry = new StateValue<AdapterRegistry>(loaded);
        history = new StateValue<PeerHistoryBook>(historyStore.Load());
        active = new ActiveAdapterCoordinator(loaded.ActiveId);
        lifecycle = new AdapterRelationshipCoordinator(loaded.Active?.ToRelationship());
        relationship = new StateValue<AdapterRelationshipStatus>(lifecycle.Status);

        diagnostics.Info(
            "app",
            $"registry loaded: {loaded.Records.Count} adapter(s), active={loaded.ActiveId?.Value ?? "none"}");
    }

    /// <summary>
    /// Build the production composition: the real BLE transport, behind
    /// <see cref="ManagementOwner"/>, over the default document location.
    ///
    /// Here rather than in the App project so the shell never names a transport
    /// implementation. That is not tidiness — it is what makes the layering guard
    /// meaningful: a page that can reach the transport can eventually open a
    /// characteristic, and a second management session is the defect this whole
    /// subsystem is shaped around.
    /// </summary>
    public static AdapterConnectionService CreateDefault(DiagnosticLog diagnostics)
    {
        var documents = new WindowsDocumentStore();
        var repository = ManagementOwner.Get(
            diagnostics,
            () => new AdapterRepository(
                new BleGattManagementTransport(
                    (source, detail) => diagnostics.Debug(source, detail))));

        diagnostics.Info("app", $"documents at {documents.Directory}");
        return new AdapterConnectionService(repository, documents, diagnostics);
    }

    public IReadOnlyStateValue<ConnectionState> Connection => repository.Connection;

    public IReadOnlyStateValue<AdapterSnapshot> Snapshot => repository.Snapshot;

    public IReadOnlyStateValue<AdapterRegistry> Registry => registry;

    public IReadOnlyStateValue<AdapterRelationshipStatus> Relationship => relationship;

    public IReadOnlyStateValue<BluetoothRadioCapabilities> Radio => radio;

    public DiagnosticLog Diagnostics => diagnostics;

    public ControllerInventoryView Inventory =>
        ControllerInventory.Build(Snapshot.Value.Peers, history.Value.ForAdapter(active.State.ActiveId));

    public async Task<BluetoothRadioCapabilities> ProbeRadioAsync()
    {
        var capabilities = await probeRadio().ConfigureAwait(false);
        radio.Set(capabilities);
        diagnostics.Info("radio", capabilities.Describe());
        return capabilities;
    }

    /// <summary>
    /// Pair a new adapter: discover, pair, connect, validate, and only then
    /// remember it.
    ///
    /// The order is the safety property. A registry row is created ONLY after
    /// <c>info</c> answers <c>id == "picoswitch"</c> — discovering a Bluetooth
    /// device that advertises the right service is not permission to adopt it as
    /// the user's adapter.
    /// </summary>
    public Task<AdapterId?> PairNewAdapterAsync(CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            await ProbeRadioAsync().ConfigureAwait(false);
            WindowsBluetoothRadio.RequireManagementCapable(radio.Value);

            var generation = lifecycle.BeginDiscovery();
            Publish();

            DiscoveredManagementPeer peer;
            try
            {
                peer = await repository.DiscoverForPairingAsync(
                    new ManagementConnectionContext { Reason = "first-pair" },
                    cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                lifecycle.DiscoveryFailed(generation, error.Message);
                Publish();
                diagnostics.Error("pair", $"discovery failed: {error.Message}");
                throw;
            }

            diagnostics.Info(
                "pair",
                $"found {peer.Address} rssi={peer.SignalStrengthDbm?.ToString() ?? "?"} " +
                $"name={peer.DisplayName ?? "(none)"}");

            var pairingState = await ReadPairingStateAsync(peer.BluetoothAddress).ConfigureAwait(false);
            if (pairingState == WindowsPairingState.Paired)
            {
                // No ceremony will run: Windows already holds a bond, so PairAsync
                // is skipped entirely and the flow goes straight to connect. If that
                // bond is stale the connect fails, and this line is what tells the
                // difference between "pairing succeeded" and "pairing never
                // happened". Observed 2026-08-29, where four Pair presses in a row
                // reused a bond the adapter had already forgotten.
                diagnostics.Info(
                    "pair",
                    $"{peer.Address}: Windows already holds a pairing; " +
                    "no new pairing ceremony will run");
            }

            var found = new AdapterRelationship(peer.Address, peer.DisplayName ?? AdapterRecord.DefaultProductName);
            var decision = lifecycle.DeviceDiscovered(generation, found, pairingState);
            Publish();

            if (decision is AdapterLifecycleDecision.AwaitPairing)
            {
                var result = await pairing
                    .PairAsync(peer.BluetoothAddress, cancellationToken).ConfigureAwait(false);
                diagnostics.Info("pair", $"{result.Outcome} ({result.Status}) {result.Message}");

                lifecycle.PairingCompleted(
                    generation,
                    result.Succeeded ? WindowsPairingState.Paired : WindowsPairingState.NotPaired);
                Publish();

                if (!result.Succeeded)
                {
                    throw new ManagementException(result.Message);
                }
            }
            else if (decision is not AdapterLifecycleDecision.Connect)
            {
                throw new ManagementException(
                    lifecycle.Status.Message ?? "The adapter could not be paired.");
            }

            return await CompleteConnectionAsync(
                peer.Address,
                peer.DisplayName,
                generation,
                firstPair: true,
                cancellationToken).ConfigureAwait(false);
        });

    /// <summary>Connect to a remembered adapter.</summary>
    public Task<AdapterId?> ConnectAsync(AdapterId id, CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            if (registry.Value.Record(id) is not { } record)
            {
                throw new ManagementException("That adapter is no longer remembered.");
            }

            await ProbeRadioAsync().ConfigureAwait(false);
            WindowsBluetoothRadio.RequireManagementCapable(radio.Value);

            active.Adopt(id);

            // Read the real state rather than assuming one, from the address --
            // the only identifier this app persists.
            //
            // Unknown is safe: RequestReconnect attempts the connection anyway and
            // lets the failure classify itself.
            var pairingState = MapPairing((await pairing
                .ReadAsync(id.ToBluetoothAddress(), cancellationToken)
                .ConfigureAwait(false)).State);

            var decision = lifecycle.RequestReconnect(
                record.ToRelationship(),
                AdapterConnectReason.Manual,
                pairingState);
            Publish();

            if (decision is AdapterLifecycleDecision.RepairRequired repair)
            {
                MarkRepairRequired(id, repair.Message);
                return null;
            }

            if (decision is not AdapterLifecycleDecision.Connect connect)
            {
                return null;
            }

            return await CompleteConnectionAsync(
                record.Address,
                record.DisplayName,
                connect.Attempt.Generation,
                firstPair: false,
                cancellationToken).ConfigureAwait(false);
        });

    public Task DisconnectAsync() => RunExclusiveAsync(async () =>
    {
        await repository.DisconnectAsync().ConfigureAwait(false);
        lifecycle.ConnectionEnded("Disconnected.");
        active.MarkDisconnected();
        Publish();
        diagnostics.Info("app", "disconnected by request");
        return (object?)null;
    });

    /// <summary>
    /// Read everything the dashboard shows, and fold a COMPLETE peer inventory
    /// into history.
    ///
    /// A partial read is never folded: a missing row would be indistinguishable
    /// from a peer the adapter has forgotten, and history would then demote a live
    /// saved controller to "Recent".
    /// </summary>
    public Task RefreshAsync(CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            await repository.RefreshAllAsync(cancellationToken).ConfigureAwait(false);
            await repository.ProbeManagementCapabilitiesAsync(cancellationToken).ConfigureAwait(false);

            if (Snapshot.Value.Capabilities.Peers == CapabilityState.Available)
            {
                var peers = await repository.RefreshPeersAsync(cancellationToken).ConfigureAwait(false);
                if (peers.Complete && active.State.ActiveId is { } id)
                {
                    var updated = history.Value.With(
                        id,
                        history.Value.ForAdapter(id).Observing(peers, nowMillis()));
                    history.Set(updated);
                    historyStore.Save(updated);
                }
            }

            CacheDisplayState();
            diagnostics.Info("app", $"refreshed: {Describe(Snapshot.Value)}");
            return (object?)null;
        });

    /// <summary>
    /// Replace the Windows pairing for one adapter.
    ///
    /// **Never automatic.** Unpairing destroys a trust relationship, so this is
    /// only ever reached from an explicit confirmation. The row's alias, history
    /// and selected identity are retained; only the Windows-side trust is
    /// replaced.
    /// </summary>
    public Task RepairAsync(AdapterId id, CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            if (registry.Value.Record(id) is not { } record)
            {
                throw new ManagementException("That adapter is no longer remembered.");
            }

            await repository.DisconnectAsync().ConfigureAwait(false);

            // Resolve the Windows pairing object FRESH, from the address. Repair
            // must work with no live management session and no cached state --
            // that is the whole situation it exists for.
            var outcome = await pairing
                .UnpairAsync(id.ToBluetoothAddress(), cancellationToken)
                .ConfigureAwait(false);
            diagnostics.Info("repair", $"unpair {record.Address}: {outcome.DiagnosticName()}");

            if (!outcome.TrustRemoved())
            {
                // The stale bond is still there, so the row is still broken. The
                // repair flag deliberately STAYS set: clearing it while the pairing
                // survived is exactly the defect this replaced -- it reported a
                // repair that had not happened.
                RepairFailed(record.Address, outcome.Message());
            }

            // Verify rather than assume. UnpairAsync reporting success and Windows
            // still holding the pairing would leave the user in the loop this whole
            // change exists to break, and the check costs one resolve.
            var after = await pairing
                .ReadAsync(id.ToBluetoothAddress(), cancellationToken)
                .ConfigureAwait(false);
            if (after.State == WindowsPairingKnown.Paired)
            {
                RepairFailed(
                    record.Address,
                    "Windows still reports this adapter as paired after removing the pairing. " +
                    "Remove it from Windows Bluetooth settings and try again.");
            }

            UpdateRegistry(current => current.Update(id, row => row with
            {
                RepairRequired = false,
            }));

            lifecycle.CancelAndRetainRelationship(outcome.Message());
            Publish();
            return (object?)null;
        });

    /// <summary>
    /// Remove one adapter from the app, and drop its Windows pairing with it.
    ///
    /// **The unpair is part of Remove, per WINDOWS_PASS.md §16.2** — "Remove
    /// adapter is per-adapter, unpairs that Windows relationship after
    /// confirmation, and deletes that row and its peer history". The confirmation
    /// belongs to the caller; this method assumes it has already happened, which is
    /// why nothing here is reachable from an automatic path.
    ///
    /// It was previously local-only, and that was a misreading of §19.5 on my part:
    /// the sentence "it never unregisters, unpairs or deletes the prior row" is
    /// about what PAIRING ANOTHER UNIT must not do to an existing row, not about
    /// what Remove does to its own. Leaving the OS pairing behind strands a bond
    /// the app can no longer see or offer to clear.
    ///
    /// <paramref name="unpairWindows"/> exists for the deliberate "forget locally
    /// only" case, and the caller must be explicit about wanting it.
    ///
    /// The row is removed even if the unpair fails, because Remove must reliably do
    /// what it says; the outcome reports whether Windows trust actually went, so
    /// the caller can say so rather than implying more than happened.
    /// </summary>
    public Task<AdapterRemoval> RemoveAsync(AdapterId id, bool unpairWindows = true) =>
        RunExclusiveAsync(async () =>
        {
            if (active.State.ActiveId == id)
            {
                await repository.DisconnectAsync().ConfigureAwait(false);
                lifecycle.Forget();
                active.Cleared();
            }

            AdapterUnpairResult? unpaired = null;
            if (unpairWindows)
            {
                try
                {
                    unpaired = await pairing.UnpairAsync(id.ToBluetoothAddress()).ConfigureAwait(false);
                }
                catch (Exception error)
                {
                    // A radio that is off must not strand the row in the list.
                    diagnostics.Warn("app", $"unpair during remove failed: {error.Message}");
                    unpaired = AdapterUnpairResult.Failed;
                }
            }

            UpdateRegistry(current => current.Without(id));
            var trimmed = history.Value.Without(id);
            history.Set(trimmed);
            historyStore.Save(trimmed);

            Publish();
            diagnostics.Info(
                "app",
                $"removed adapter {id.Value} from this app; windows-pairing=" +
                (unpaired is { } outcome ? outcome.DiagnosticName() : "kept-by-request") +
                "; adapter-side controller bonds untouched");

            return new AdapterRemoval(unpairWindows, unpaired);
        });

    /* ------------------------------------------------ Phase 3 dashboard */

    /// <summary>
    /// Switch the emulated controller and make it host-visible.
    ///
    /// The re-enumeration usually drops the management link, so this reconnects
    /// afterwards with <see cref="AdapterConnectReason.AfterPersonality"/>. That
    /// reason exists so a support log says WHY a reconnect happened; a reconnect
    /// with no cause reads as instability.
    /// </summary>
    public Task<PersonalitySwitchOutcome> SetPersonalityAsync(
        Personality personality,
        CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            var outcome = await repository
                .SetPersonalityAsync(personality, cancellationToken)
                .ConfigureAwait(false);

            diagnostics.Info(
                "personality",
                $"{personality.WireName()}: unchanged={outcome.Unchanged} " +
                $"reenumerated={outcome.Reenumerated}" +
                (outcome.ReenumerationError is { } reason ? $" error={reason}" : string.Empty));

            if (outcome.Reenumerated)
            {
                // The adapter detached from USB. The management link may or may not
                // have survived; reconnecting is cheap and idempotent, and doing it
                // here means the dashboard is never left showing a session that has
                // already gone.
                await ReconnectAfterAsync(AdapterConnectReason.AfterPersonality, cancellationToken)
                    .ConfigureAwait(false);
            }

            CacheDisplayState();
            return outcome;
        });

    /// <summary>
    /// Set one colour and publish the adapter's readback (I8).
    ///
    /// No re-enumeration: doing it per slider release would detach USB every time
    /// the user drags. <see cref="ApplyAppearanceAsync"/> is the explicit action
    /// that makes pending colours host-visible (I7).
    /// </summary>
    public Task<AdapterConfig> SetColorAsync(
        ColorTarget target,
        RgbColor color,
        CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            var config = await repository.SetColorAsync(target, color, persist: true, cancellationToken)
                .ConfigureAwait(false);
            diagnostics.Info(
                "color",
                $"{target} -> body=({config.BodyColor.Wire()}) left=({config.LeftAccent.Wire()}) " +
                $"right=({config.RightAccent.Wire()})");
            return config;
        });

    /// <summary>Re-enumerate so pending colour changes reach the console (I7).</summary>
    public Task ApplyAppearanceAsync(CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            await repository.ReenumerateAsync(cancellationToken).ConfigureAwait(false);
            diagnostics.Info("color", "re-enumerated so the console sees the new colours");
            await ReconnectAfterAsync(AdapterConnectReason.AfterPersonality, cancellationToken)
                .ConfigureAwait(false);
            return (object?)null;
        });

    public Task<AdapterInputState> SetActiveInputAsync(
        long sourceId,
        CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            var input = await repository.SetActiveInputAsync(sourceId, cancellationToken)
                .ConfigureAwait(false);
            diagnostics.Info("input", $"active source -> {input.ActiveId}");
            CacheDisplayState();
            return input;
        });

    /// <summary>
    /// Wake the console.
    ///
    /// <c>Unknown</c> is "the adapter could not tell us", not failure — older
    /// firmware has no <c>wake status</c> — and the caller must not present it as
    /// an error.
    /// </summary>
    public Task<WakeStatus> WakeConsoleAsync(CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            var status = await repository.WakeConsoleAsync(cancellationToken).ConfigureAwait(false);
            diagnostics.Info(
                "wake",
                $"result={status.Result} asleep={status.ConsoleAsleep} attempts={status.Attempts}");
            return status;
        });

    /// <summary>
    /// Turn the adapter's in-band management gate off or on.
    ///
    /// Turning it OFF ends this session and locks out every future one until it is
    /// re-enabled over UART or by the physical gesture — the app cannot undo it.
    /// Only ever reached from an explicit confirmation.
    /// </summary>
    public Task<bool?> SetManagementEnabledAsync(
        bool enabled,
        CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            var reported = await repository.SetManagementEnabledAsync(enabled, cancellationToken)
                .ConfigureAwait(false);
            diagnostics.Warn(
                "gate",
                $"management gate set to {enabled}; adapter reports {reported?.ToString() ?? "unknown"}");
            return reported;
        });

    /// <summary>Raw LE bond slots, for Diagnostics only. Never drives Paired Controllers.</summary>
    public Task<BondEnumeration> RefreshBondDiagnosticsAsync(
        CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(() => repository.RefreshBondDiagnosticsAsync(cancellationToken));

    public Task<BondEnumeration> RemoveBondAsync(
        int index,
        CancellationToken cancellationToken = default) =>
        RunExclusiveAsync(async () =>
        {
            var bonds = await repository.RemoveBondAsync(index, cancellationToken).ConfigureAwait(false);
            diagnostics.Warn("bonds", $"removed raw LE bond slot {index}");
            return bonds;
        });

    /// <summary>
    /// Reconnect after an operation that detaches the adapter from USB.
    ///
    /// Best-effort by design: the operation itself already succeeded, and a failed
    /// reconnect is a session problem the user can retry, not a reason to report
    /// the personality switch or colour apply as failed.
    /// </summary>
    private async Task ReconnectAfterAsync(
        AdapterConnectReason reason,
        CancellationToken cancellationToken)
    {
        if (active.State.ActiveId is not { } id || registry.Value.Record(id) is not { } record)
        {
            return;
        }

        try
        {
            await repository.DisconnectAsync().ConfigureAwait(false);
            lifecycle.ConnectionEnded(null);
            var decision = lifecycle.RequestReconnect(
                record.ToRelationship(),
                reason,
                WindowsPairingState.Paired);
            Publish();

            if (decision is AdapterLifecycleDecision.Connect connect)
            {
                await CompleteConnectionAsync(
                    record.Address,
                    record.DisplayName,
                    connect.Attempt.Generation,
                    firstPair: false,
                    cancellationToken).ConfigureAwait(false);
            }
        }
        catch (Exception error)
        {
            diagnostics.Warn("connect", $"reconnect after {reason} failed: {error.Message}");
        }
    }

    public Task RenameAsync(AdapterId id, string? alias) => RunExclusiveAsync(() =>
    {
        UpdateRegistry(current => current.Update(id, row => row with
        {
            UserAlias = AdapterAlias.Sanitize(alias),
        }));
        return Task.FromResult<object?>(null);
    });

    /* --------------------------------------------------------------- internals */

    private async Task<AdapterId?> CompleteConnectionAsync(
        string address,
        string? displayName,
        long generation,
        bool firstPair,
        CancellationToken cancellationToken)
    {
        try
        {
            await repository.ConnectKnownAsync(
                address,
                new ManagementConnectionContext
                {
                    Reason = firstPair ? "first-pair" : "manual",
                    LogicalAttempt = generation,
                    PairingState = "paired",
                },
                cancellationToken).ConfigureAwait(false);
        }
        catch (AdapterIdentityException error)
        {
            lifecycle.IdentityRejected(generation);
            Publish();
            diagnostics.Error("connect", error.Message);
            throw;
        }
        catch (Exception error)
        {
            var trust = repository.Trust;
            var bondMismatch = AdapterResetSignature.IsBondMismatch(error, trust);
            var id = AdapterId.FromAddress(address);
            var remembered = id is { } candidate && registry.Value.Record(candidate) is not null;

            // A stale Windows bond reached through the PAIR flow has no remembered
            // row and therefore no Repair button. Saying "the adapter did not expose
            // its management service" there sent the user round the same loop four
            // times on 2026-08-29; name the actual problem and the actual way out.
            var repairMessage = remembered
                ? AdapterResetSignature.RepairMessage
                : AdapterResetSignature.StalePairingMessage;

            lifecycle.ConnectionFailed(generation, error.Message, bondMismatch, repairMessage);
            Publish();

            if (bondMismatch && remembered && id is { } known)
            {
                MarkRepairRequired(known, repairMessage);
            }
            else if (id is { } target)
            {
                active.ActivationFailed(target, bondMismatch ? repairMessage : error.Message);
            }

            // The full WinRT detail, not just the friendly message. This is the
            // line the stale-bond experiment reads: without the stage, the
            // GattCommunicationStatus, the ATT byte and the HRESULT, "it refused"
            // is unactionable and the signature cannot be judged right or wrong.
            diagnostics.Error(
                "connect",
                $"failed [{DescribeFailure(error)}] " +
                $"[{AdapterResetSignature.Explain(error, trust)}] {error.Message}");

            if (bondMismatch)
            {
                // Report the DIAGNOSIS, not the symptom. The relationship already
                // reached RepairRequired and the banner already says what to do; a
                // caller that rethrows the raw transport message puts "the adapter
                // did not expose its management service" in the most prominent
                // element on the page and contradicts it. Observed 2026-08-29.
                //
                // The tagged failure is kept as the inner exception, so the
                // diagnostic trail above loses nothing.
                throw new AdapterBondMismatchException(repairMessage, error);
            }

            throw;
        }

        // The carrier is up and identity is verified: the repository only calls
        // MarkValidated after `info` answered. Walk the lifecycle through the same
        // two steps so Connected still cannot be reached any other way.
        lifecycle.CarrierReady(generation);
        var verified = lifecycle.IdentityValidated(generation);
        Publish();

        if (AdapterId.FromAddress(address) is not { } adapterId)
        {
            return null;
        }

        var existing = registry.Value.Record(adapterId);
        var row = existing ?? AdapterRecord.Of(address, displayName)!;
        UpdateRegistry(current => current
            .With(row with
            {
                LastKnownName = string.IsNullOrWhiteSpace(displayName)
                    ? row.LastKnownName
                    : displayName,
                LastSeenAtMillis = nowMillis(),
                LastConnectedAtMillis = nowMillis(),
                RepairRequired = false,
            })
            .Selecting(adapterId));

        active.Adopt(adapterId);
        active.ActivationSucceeded(adapterId);
        CacheDisplayState();

        diagnostics.Info(
            "connect",
            $"connected {address} as {verified?.DisplayName ?? row.DisplayName}: {Describe(Snapshot.Value)}");
        return adapterId;
    }

    /// <summary>
    /// A repair that did not remove the Windows pairing.
    ///
    /// Leaves the repair flag set and reports the real reason. It does NOT drive
    /// the lifecycle through ConnectionFailed: there is no active connection
    /// attempt during a repair, so that call would be ignored and the user would be
    /// told nothing.
    /// </summary>
    [DoesNotReturn]
    private void RepairFailed(string address, string message)
    {
        lifecycle.CancelAndRetainRelationship(message);
        Publish();
        diagnostics.Warn("repair", $"{address}: {message}");
        throw new ManagementException(message);
    }

    private void MarkRepairRequired(AdapterId id, string message)
    {
        UpdateRegistry(current => current.Update(id, row => row with { RepairRequired = true }));
        active.ActivationFailed(id, message);
        Publish();
        diagnostics.Warn("repair", $"{id.Value}: {message}");
    }

    /// <summary>
    /// Cache the display-only fields for the adapter list.
    ///
    /// Explicitly NOT authoritative: this is what was true at the last verified
    /// connection, so the list can say something honest about an adapter that is
    /// not connected right now. Live truth stays in the snapshot.
    /// </summary>
    private void CacheDisplayState()
    {
        if (active.State.ActiveId is not { } id)
        {
            return;
        }

        var snapshot = Snapshot.Value;
        UpdateRegistry(current => current.Update(id, row => row with
        {
            LastFirmwareVersion = string.IsNullOrWhiteSpace(snapshot.Firmware.Version)
                ? row.LastFirmwareVersion
                : snapshot.Firmware.Version,
            LastPersonality = snapshot.Personality.Current == Management.Personality.Unknown
                ? row.LastPersonality
                : snapshot.Personality.Current.WireName(),
        }));
    }

    private void UpdateRegistry(Func<AdapterRegistry, AdapterRegistry> transform)
    {
        var next = transform(registry.Value);
        registry.Set(next);
        if (!registryStore.Save(next))
        {
            diagnostics.Warn("app", "the adapter registry could not be written to disk");
        }
    }

    private void Publish() => relationship.Set(lifecycle.Status);

    /// <summary>
    /// A freshly discovered peer has no cached device path, so its pairing state
    /// is read through the address. Unknown stays Unknown.
    /// </summary>
    private async Task<WindowsPairingState> ReadPairingStateAsync(ulong address) =>
        MapPairing((await pairing.ReadAsync(address).ConfigureAwait(false)).State);

    private static WindowsPairingState MapPairing(WindowsPairingKnown known) => known switch
    {
        WindowsPairingKnown.Paired => WindowsPairingState.Paired,
        WindowsPairingKnown.NotPaired => WindowsPairingState.NotPaired,
        _ => WindowsPairingState.Unknown,
    };

    /// <summary>
    /// Render every tagged transport failure in an exception, including both
    /// branches of the ladder's aggregate report.
    /// </summary>
    private static string DescribeFailure(Exception error)
    {
        var parts = new List<string>();
        Walk(error);
        return parts.Count == 0 ? error.GetType().Name : string.Join(" | ", parts);

        void Walk(Exception? cursor)
        {
            switch (cursor)
            {
                case null:
                    return;
                case GattTransportException failure:
                    parts.Add(failure.Describe());
                    Walk(failure.InnerException);
                    return;
                case AggregateException aggregate:
                    foreach (var branch in aggregate.InnerExceptions)
                    {
                        Walk(branch);
                    }

                    return;
                default:
                    Walk(cursor.InnerException);
                    return;
            }
        }
    }

    private static string Describe(AdapterSnapshot snapshot) =>
        $"fw={snapshot.Firmware.Version} build={snapshot.Firmware.Build} " +
        $"contract={snapshot.Firmware.BridgeContract} " +
        $"personality={snapshot.Personality.Current.WireName()} " +
        $"controller={snapshot.Controller.Name} peers={snapshot.Peers.Peers.Count}";

    private async Task<T> RunExclusiveAsync<T>(Func<Task<T>> operation)
    {
        // One user-initiated adapter operation at a time. The transport already
        // serialises exchanges; this stops two lifecycle transitions (a connect and
        // a repair, say) from interleaving above it.
        await operationGate.WaitAsync().ConfigureAwait(false);
        try
        {
            return await operation().ConfigureAwait(false);
        }
        finally
        {
            operationGate.Release();
        }
    }

    private Task RunExclusiveAsync(Func<Task<object?>> operation) =>
        RunExclusiveAsync<object?>(operation);
}
