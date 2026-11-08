using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Bluetooth;
using PicoSwitch.Companion.Platform.CompanionAudio;
using PicoSwitch.Companion.Platform.ControllerLink;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// The management transport over BlueZ: the Linux counterpart of the Windows
/// <c>BleGattManagementTransport</c>, and deliberately the same shape.
///
/// ## What is shared with Windows, and must stay shared
///
/// Everything that is a rule about the ADAPTER rather than about the host stack
/// is reused verbatim from the portable layers: <see cref="BleManagementContract"/>
/// framing and chunking, <see cref="BleReplyAssembler"/>,
/// <see cref="SerializedManagementSession"/> (one exchange at a time),
/// <see cref="ManagementTurnaroundPolicy"/> (the one-slot carrier needs a gap
/// after a reply), <see cref="ManagementRetryPolicy"/> (only allowlisted
/// commands are re-sent after a timeout), and <see cref="GattCallbackAuthority"/>
/// (a retired connection's notification never mutates live state). If one of
/// those needs to change for Linux, that is evidence about the adapter and
/// belongs in the shared code, not in a fork here.
///
/// ## What is BlueZ-specific
///
/// - Discovery is <c>Adapter1.StartDiscovery</c> filtered to the management
///   service UUID; a remembered adapter is matched by its exact address.
/// - BlueZ owns the link: <c>Device1.Connect</c> brings it up and resolves
///   services, and notifications arrive as <c>Value</c> property changes after
///   <c>StartNotify</c>.
/// - The negotiated ATT MTU is the characteristic's <c>MTU</c> property.
/// - A dropped link is observed directly (<c>Connected</c> turning false), so
///   the session is retired at once instead of on the next command's failure.
///
/// ## Trust evidence
///
/// <see cref="TransportTrustSnapshot"/> is accumulated per LOGICAL attempt and
/// survives teardown, for the same reason as on Windows: the caller classifies
/// the failure after this transport has already disposed the attempt.
/// <c>BondProven</c> is set when <c>StartNotify</c> on TX succeeds; the
/// firmware declares that CCC <c>ATT_SECURITY_ENCRYPTED</c>, so BlueZ can only
/// complete it over an encrypted link with a key both sides hold.
/// </summary>
internal sealed class BluezManagementTransport : IManagementTransport, IControllerLinkDataPlaneProvider,
    ICompanionAudioDownlinkProvider, ICompanionMicUplinkProvider
{
    internal const int ConnectTimeoutMillis = 15_000;
    internal const int PairingConnectTimeoutMillis = 60_000;
    internal const int ScanTimeoutMillis = 15_000;
    private const int DisconnectTimeoutMillis = 1_250;

    /// <summary>
    /// How long to wait for one fresh LE advertisement before Connect, so BlueZ
    /// picks the LE bearer (see <see cref="RefreshLePresenceAsync"/>). The adapter
    /// advertises several times a second; a miss only means the old behaviour.
    /// </summary>
    internal int LeRefreshMillis { get; init; } = 3_000;

    private readonly IBluezBus bus;
    private readonly Func<Task<BluetoothRadioCapabilities>> probeRadio;
    private readonly SerializedManagementSession session = new();
    private readonly StateValue<ConnectionState> connection = new(new ConnectionState());
    private readonly Lock gate = new();
    private readonly Action<string, string>? log;

    private OwnedLink? owned;
    private long generation;
    private ManagementConnectionContext context = new();
    private bool disposed;
    private bool validated;

    // Per-LOGICAL-attempt evidence; see the class remarks and the Windows
    // transport's identical block for why teardown must not clear these.
    private long evidenceAttempt = long.MinValue;
    private bool attemptPaired;
    private bool peerObserved;
    private bool peerAnsweredGatt;
    private int linkFailuresAfterResolve;
    private bool bondProven;

    private volatile PendingReply? pending;

    public BluezManagementTransport(
        IBluezBus bus,
        Func<Task<BluetoothRadioCapabilities>> probeRadio,
        Action<string, string>? log = null)
    {
        this.bus = bus;
        this.probeRadio = probeRadio;
        this.log = log;
    }

    public IReadOnlyStateValue<ConnectionState> Connection => connection;

    public TransportTrustSnapshot Trust
    {
        get
        {
            lock (gate)
            {
                return new TransportTrustSnapshot(
                    attemptPaired,
                    peerObserved,
                    peerAnsweredGatt,
                    linkFailuresAfterResolve,
                    bondProven);
            }
        }
    }

    public IControllerLinkDataPlane? TryCreateDataPlane()
    {
        OwnedLink? current;
        lock (gate)
        {
            current = owned;
        }

        if (current is null || current.Closed)
        {
            return null;
        }

        var plane = new BluezControllerLinkDataPlane(bus, current.Service, () => current.AttMtu, log);
        current.Attach(plane);
        return plane;
    }

    /// <summary>
    /// Companion audio (EXPERIMENTAL) on the link this transport already owns --
    /// one more subscription, never a second connection, as on Windows.
    /// </summary>
    public ICompanionAudioDownlink? TryCreateAudioDownlink()
    {
        var current = LiveLink();
        return current is null ? null : new BluezCompanionAudioDownlink(bus, current.Service, () => current.AttMtu);
    }

    /// <summary>The companion microphone uplink on the same owned link.</summary>
    public ICompanionMicUplink? TryCreateMicUplink()
    {
        var current = LiveLink();
        return current is null ? null : new BluezCompanionMicUplink(bus, current.Service);
    }

    private OwnedLink? LiveLink()
    {
        OwnedLink? current;
        lock (gate)
        {
            current = owned;
        }

        return current is null || current.Closed ? null : current;
    }

    public void PrepareConnection(ManagementConnectionContext next)
    {
        lock (gate)
        {
            context = next;
            if (next.LogicalAttempt != evidenceAttempt)
            {
                evidenceAttempt = next.LogicalAttempt;
                attemptPaired = false;
                peerObserved = false;
                peerAnsweredGatt = false;
                linkFailuresAfterResolve = 0;
                bondProven = false;
            }
        }

        Log("prepare",
            $"reason={next.Reason} attempt={next.LogicalAttempt} retry={next.Retry} " +
            $"pairing={next.PairingState} priorRetired={next.PriorGattRetired}");
    }

    public Task<DiscoveredManagementPeer> DiscoverAsync(CancellationToken cancellationToken = default) =>
        ScanAsync(expectedAddress: null, cancellationToken);

    public async Task ScanAndConnectAsync(
        string? expectedAddress = null,
        CancellationToken cancellationToken = default)
    {
        Publish(ConnectionPhase.Discovering, message: "Looking for the adapter.");
        var peer = await ScanAsync(expectedAddress, cancellationToken).ConfigureAwait(false);
        await ConnectAsync(peer.BluetoothAddress, peer.DisplayName, cancellationToken).ConfigureAwait(false);
    }

    public Task ConnectKnownAsync(string address, CancellationToken cancellationToken = default)
    {
        if (!BluetoothAddressFormat.TryParse(address, out var numeric))
        {
            throw new ManagementException($"Not a Bluetooth address: {address}");
        }

        return ConnectAsync(numeric, displayName: null, cancellationToken);
    }

    public void MarkValidated()
    {
        lock (gate)
        {
            validated = true;
        }

        var state = connection.Value;
        Publish(ConnectionPhase.Connected, state.DeviceName, state.Address, message: null);
    }

    public Task<string> TransactAsync(
        string command,
        long timeoutMillis = ManagementChannel.DefaultTimeoutMillis,
        CancellationToken cancellationToken = default) =>
        session.ExchangeAsync(() => ExchangeAsync(command, timeoutMillis), cancellationToken);

    public Task DisconnectAsync() => session.MutateAsync(() => RetireAsync("disconnect"));

    public async ValueTask DisposeAsync()
    {
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
        }

        await RetireAsync("dispose").ConfigureAwait(false);
    }

    /* ------------------------------------------------------------ discovery */

    private async Task<DiscoveredManagementPeer> ScanAsync(
        string? expectedAddress,
        CancellationToken cancellationToken)
    {
        var capabilities = await probeRadio().ConfigureAwait(false);
        capabilities.RequireManagementCapable();

        ulong? wanted = null;
        if (expectedAddress is not null)
        {
            if (!BluetoothAddressFormat.TryParse(expectedAddress, out var numeric))
            {
                throw new ManagementException($"Not a Bluetooth address: {expectedAddress}");
            }

            wanted = numeric;
        }

        var adapter = await RequireAdapterAsync(cancellationToken).ConfigureAwait(false);
        var completion = new TaskCompletionSource<DiscoveredManagementPeer>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        void OnFound(BluezAdvertisement advertisement)
        {
            if (wanted is { } expected && advertisement.Address != expected)
            {
                // Restricted to the selected adapter. Another valid Pico nearby is
                // never permission to substitute it.
                return;
            }

            if (BluetoothAddressFormat.ToText(advertisement.Address) is not { } text)
            {
                return;
            }

            lock (gate)
            {
                peerObserved = true;
            }

            completion.TrySetResult(new DiscoveredManagementPeer(
                advertisement.Address,
                text,
                DisplayName: string.IsNullOrWhiteSpace(advertisement.Name) ? null : advertisement.Name,
                SignalStrengthDbm: advertisement.Rssi));
        }

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(ScanTimeoutMillis);
        IAsyncDisposable? scan = null;
        try
        {
            scan = await bus.StartDiscoveryAsync(adapter.Path, BleManagementContract.ServiceUuid, OnFound,
                deadline.Token).ConfigureAwait(false);
            using (deadline.Token.Register(() => completion.TrySetCanceled(deadline.Token)))
            {
                return await completion.Task.ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new ManagementException(
                "No PicoSwitch2 adapter answered. Make sure it is powered and, for a new pairing, " +
                "that its pairing window is open.");
        }
        catch (BluezException error)
        {
            Log("fail", $"discovery {error}");
            throw new ManagementException($"Bluetooth discovery failed: {error.Message}", error);
        }
        finally
        {
            if (scan is not null)
            {
                await scan.DisposeAsync().ConfigureAwait(false);
            }
        }
    }

    /* ----------------------------------------------------------- connection */

    private async Task ConnectAsync(ulong address, string? displayName, CancellationToken cancellationToken)
    {
        var text = BluetoothAddressFormat.ToText(address) ??
            throw new ManagementException("Not a Bluetooth address");

        // The previous attempt must be completely gone before a new one exists.
        await RetireAsync("reconnect").ConfigureAwait(false);

        long attemptGeneration;
        bool expectsPairing;
        lock (gate)
        {
            attemptGeneration = ++generation;
            validated = false;
            expectsPairing = context.PairingState is "notpaired" or "pairing";
            attemptPaired = false;
        }

        Publish(ConnectionPhase.Connecting, displayName, text, "Connecting.");

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(expectsPairing ? PairingConnectTimeoutMillis : ConnectTimeoutMillis);

        OwnedLink? candidate = null;
        try
        {
            candidate = await OpenAsync(address, text, displayName, attemptGeneration, deadline.Token)
                .ConfigureAwait(false);
            lock (gate)
            {
                if (attemptGeneration != generation)
                {
                    throw new ManagementException("Connection attempt superseded");
                }

                owned = candidate;
                candidate = null;
            }

            Publish(ConnectionPhase.Connecting, displayName, text, "Verifying the adapter.");
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new GattTransportException(
                "The adapter did not finish connecting in time.",
                GattFailureStage.Connect);
        }
        finally
        {
            if (candidate is not null)
            {
                await candidate.DisposeAsync().ConfigureAwait(false);
            }
        }
    }

    private async Task<OwnedLink> OpenAsync(
        ulong address,
        string text,
        string? displayName,
        long attemptGeneration,
        CancellationToken cancellationToken)
    {
        var adapter = await RequireAdapterAsync(cancellationToken).ConfigureAwait(false);
        var device = await GuardAsync(
            GattFailureStage.Connect,
            "Bluetooth could not find that adapter.",
            () => bus.FindDeviceAsync(adapter.Path, address, cancellationToken)).ConfigureAwait(false);

        if (device is null)
        {
            // BlueZ holds an object for every bonded device and for anything it
            // has recently seen advertise. Neither: the adapter must be found by
            // a scan first, which ScanAndConnectAsync does.
            throw new GattTransportException(
                "Bluetooth has not seen that adapter yet. Make sure it is powered and nearby.",
                GattFailureStage.Connect,
                GattCommunicationOutcome.Unreachable);
        }

        lock (gate)
        {
            attemptPaired = device.Paired;
        }

        Log("open", $"device resolved paired={device.Paired} connected={device.Connected}");

        var owner = new OwnedLink(this, device.Path, attemptGeneration, text, displayName ?? device.Name);
        try
        {
            // A connected device is only usable if the MANAGEMENT SERVICE is
            // there. On a dual-mode BlueZ "Connected" and "ServicesResolved" are
            // also true for a Classic (BR/EDR) link with SDP results, which has no
            // GATT at all: that link is dropped, and LE is asked for instead.
            if (device.Connected && device.ServicesResolved &&
                await HasManagementServiceAsync(device.Path).ConfigureAwait(false))
            {
                Log("open", "reusing the connected LE link");
            }
            else
            {
                if (device.Connected)
                {
                    Log("open", "connected without the management service (a Classic link); dropping it for LE");
                    await GuardAsync(GattFailureStage.Connect,
                        "Bluetooth could not drop a stale connection to the adapter.",
                        () => bus.DisconnectAsync(device.Path)).ConfigureAwait(false);
                }

                await RefreshLePresenceAsync(adapter.Path, address, cancellationToken).ConfigureAwait(false);
                await ConnectDeviceAsync(device.Path, cancellationToken).ConfigureAwait(false);
            }

            owner.Connected = true;

            var resolved = await GuardAsync(GattFailureStage.Services, "The adapter refused service discovery.",
                () => bus.WaitServicesResolvedAsync(device.Path, cancellationToken)).ConfigureAwait(false);
            if (!resolved)
            {
                RecordFailure(GattFailureStage.Services, GattCommunicationOutcome.Unreachable);
                throw new GattTransportException(
                    "The adapter disconnected during service discovery.",
                    GattFailureStage.Services,
                    GattCommunicationOutcome.Unreachable);
            }

            var service = await GuardAsync(GattFailureStage.Services,
                "The adapter did not expose its management service.",
                () => bus.FindServiceAsync(device.Path, BleManagementContract.ServiceUuid, cancellationToken))
                .ConfigureAwait(false);

            owner.Service = service ?? throw Refused(GattFailureStage.Services,
                "The adapter did not expose its management service.");
            owner.Rx = service.Characteristic(BleManagementContract.RxUuid) ?? throw Refused(
                GattFailureStage.Services,
                $"The adapter's management service is missing characteristic {BleManagementContract.RxUuid}.");
            owner.Tx = service.Characteristic(BleManagementContract.TxUuid) ?? throw Refused(
                GattFailureStage.Services,
                $"The adapter's management service is missing characteristic {BleManagementContract.TxUuid}.");

            // Watch the link before subscribing, so a drop during the CCC write
            // is still observed.
            owner.DisconnectWatch = await GuardAsync(GattFailureStage.Connect,
                "Bluetooth could not watch the adapter's connection.",
                () => bus.WatchDisconnectAsync(device.Path, () => _ = OnLinkLostAsync(attemptGeneration),
                    cancellationToken)).ConfigureAwait(false);

            // The session is NOT ready until this succeeds. Subscribing after the
            // first command would let a reply arrive with nobody listening.
            owner.Notifications = await GuardAsync(GattFailureStage.Subscribe,
                "The adapter refused to enable management notifications.",
                () => bus.SubscribeAsync(owner.Tx, owner.OnNotification, cancellationToken)).ConfigureAwait(false);

            lock (gate)
            {
                // Every handle touched above is ATT_SECURITY_ENCRYPTED in the
                // firmware's database: reaching here proves the bond.
                peerAnsweredGatt = true;
                bondProven = true;
            }

            owner.AttMtu = await bus.GetMtuAsync(owner.Tx, cancellationToken).ConfigureAwait(false)
                ?? BleManagementContract.DefaultAttMtu;
            Log("open", $"subscribed mtu={owner.AttMtu}");
            return owner;
        }
        catch
        {
            await owner.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    /* ------------------------------------------------------------- exchange */

    private async Task<string> ExchangeAsync(string command, long timeoutMillis, int attempt = 0)
    {
        OwnedLink owner;
        lock (gate)
        {
            owner = owned ?? throw new ManagementException("No adapter is connected.");
            if (owner.Generation != generation || owner.Closed)
            {
                throw new ManagementException("The management session was replaced.");
            }
        }

        // The adapter's one-slot carrier needs a gap after each reply; see
        // ManagementTurnaroundPolicy. Host-independent, so identical to Windows.
        var turnaround = ManagementTurnaroundPolicy.DelayMillis(Environment.TickCount64, owner.LastReplyAtMillis);
        if (turnaround > 0)
        {
            await Task.Delay((int)turnaround).ConfigureAwait(false);
        }

        var reply = new PendingReply();
        pending = reply;
        try
        {
            foreach (var chunk in BleManagementContract.CommandChunks(
                         command,
                         BleManagementContract.AttPayloadFor(owner.AttMtu)))
            {
                await GuardAsync(GattFailureStage.Command, $"The adapter refused the command '{command}'.",
                    () => WriteChunkAsync(owner.Rx!, chunk)).ConfigureAwait(false);
            }

            var completed = await Task.WhenAny(
                reply.Completion.Task,
                Task.Delay(TimeSpan.FromMilliseconds(timeoutMillis))).ConfigureAwait(false);

            if (completed != reply.Completion.Task)
            {
                // Management replies carry no request identifier, so a late reply
                // is indistinguishable from the next one's. Repeat only what the
                // shared allowlist says is safe; otherwise end the session.
                if (ManagementRetryPolicy.IsRepeatable(command) && attempt < ManagementRetryPolicy.MaxRetries)
                {
                    return await ExchangeAsync(command, timeoutMillis, attempt + 1).ConfigureAwait(false);
                }

                await RetireAsync("reply-timeout").ConfigureAwait(false);
                throw new ManagementException($"The adapter did not answer '{command}' within {timeoutMillis} ms.");
            }

            owner.LastReplyAtMillis = Environment.TickCount64;
            return await reply.Completion.Task.ConfigureAwait(false);
        }
        catch (GattTransportException)
        {
            await RetireAsync("command-failed").ConfigureAwait(false);
            throw;
        }
        finally
        {
            pending = null;
        }
    }

    private async Task<bool> WriteChunkAsync(string characteristic, byte[] chunk)
    {
        await bus.WriteAsync(characteristic, chunk, withResponse: true).ConfigureAwait(false);
        return true;
    }

    private void OnNotification(long callbackGeneration, bool ownerClosed, byte[] payload)
    {
        long current;
        lock (gate)
        {
            current = generation;
        }

        if (!GattCallbackAuthority.IsAuthoritative(current, callbackGeneration, ownerClosed))
        {
            return;
        }

        if (pending is not { } reply)
        {
            return;
        }

        try
        {
            if (reply.Assembler.Accept(payload) is { } complete)
            {
                reply.Completion.TrySetResult(complete);
            }
        }
        catch (ManagementReplyTooLargeException error)
        {
            reply.Completion.TrySetException(error);
        }
    }

    /* -------------------------------------------------------------- teardown */

    private async Task OnLinkLostAsync(long linkGeneration)
    {
        bool current;
        lock (gate)
        {
            current = owned is { } live && live.Generation == linkGeneration;
        }

        if (!current)
        {
            return;
        }

        Log("link", $"connected=false gen={linkGeneration}");

        // Through the session's mutation gate so a retire never interleaves with
        // an exchange that is mid-write.
        await session.MutateAsync(() => RetireAsync("link-lost")).ConfigureAwait(false);
    }

    private async Task RetireAsync(string reason)
    {
        OwnedLink? retiring;
        lock (gate)
        {
            retiring = owned;
            owned = null;
            validated = false;
            if (retiring is not null)
            {
                generation += 1;
            }
        }

        pending?.Completion.TrySetException(
            new ManagementException("The management session ended before a reply arrived."));
        pending = null;

        if (retiring is null)
        {
            Publish(ConnectionPhase.Idle, message: null);
            return;
        }

        Publish(ConnectionPhase.Disconnecting, retiring.DisplayName, retiring.Address, reason);
        try
        {
            await retiring.DisposeAsync().AsTask()
                .WaitAsync(TimeSpan.FromMilliseconds(DisconnectTimeoutMillis)).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            Log("retire", "teardown exceeded its budget; abandoning it");
        }

        Log("retire", $"reason={reason} gen={retiring.Generation}");
        Publish(ConnectionPhase.Idle, message: null);
    }

    /* --------------------------------------------------------------- helpers */

    private async Task<BluezAdapter> RequireAdapterAsync(CancellationToken cancellationToken)
    {
        try
        {
            return await bus.GetAdapterAsync(cancellationToken).ConfigureAwait(false)
                ?? throw new ManagementException("This computer has no Bluetooth adapter.");
        }
        catch (BluezException error)
        {
            throw new ManagementException($"Bluetooth is unavailable: {error.Message}", error);
        }
    }

    /// <summary>How long to wait, after Connect fails, for BlueZ to say why.</summary>
    private const int DisconnectReasonGraceMillis = 1500;

    /// <summary>
    /// Device1.Connect, with BlueZ's disconnect REASON attached to a failure.
    /// </summary>
    /// <remarks>
    /// A stale bond (the adapter forgot this computer; BlueZ still holds a key)
    /// fails Connect with a generic "le-connection-abort-by-local", which alone
    /// reads as Unreachable and can never offer Repair, so every retry reuses the
    /// dead key forever. BlueZ's separate Disconnected signal carries
    /// org.bluez.Reason.Authentication for exactly that case, and it arrives just
    /// AFTER the method error, hence the short wait. That reason is a security
    /// refusal by a peer that was there to refuse, so it is AccessDenied: the
    /// shared rule's conclusive shape, the same one Windows reaches through ATT
    /// "insufficient authentication". Any other reason, or none (BlueZ older
    /// than 5.82), keeps the old classification.
    /// </remarks>
    private async Task ConnectDeviceAsync(string devicePath, CancellationToken cancellationToken)
    {
        var reason = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        IDisposable? watch = null;
        try
        {
            watch = await bus.WatchDisconnectReasonAsync(
                devicePath, (name, _) => reason.TrySetResult(name), cancellationToken).ConfigureAwait(false);
        }
        catch (BluezException error)
        {
            // Diagnostics only: the connect itself must not depend on it.
            Log("open", $"disconnect reasons unavailable: {error.Message}");
        }

        try
        {
            await bus.ConnectAsync(devicePath, cancellationToken).ConfigureAwait(false);
        }
        catch (BluezException error)
        {
            var first = await Task.WhenAny(reason.Task, Task.Delay(DisconnectReasonGraceMillis, CancellationToken.None))
                .ConfigureAwait(false);
            var said = first == reason.Task ? reason.Task.Result : null;
            Log("open", $"connect failed: {error.ErrorName} '{error.Message}' reason={said ?? "(none)"}");
            if (said == BluezErrors.AuthenticationReason)
            {
                lock (gate)
                {
                    // The link came up and the peer refused our key: it was there.
                    peerObserved = true;
                    peerAnsweredGatt = true;
                }

                var refused = new GattTransportException(
                    "The adapter refused this computer's pairing. It no longer recognises it.",
                    GattFailureStage.Connect,
                    GattCommunicationOutcome.AccessDenied,
                    innerException: error);
                Log("fail", $"{refused.Describe()} dbus={error}");
                throw refused;
            }

            // DUAL-MODE BLUEZ. The adapter advertises without "BR/EDR not
            // supported", so a dual-mode BlueZ (ControllerMode=dual, the default)
            // treats it as dual-mode: Device1.Connect brings the LE link up AND
            // tries Classic profiles, which the adapter refuses (Classic is for
            // Controller Link only, after management is up), and Connect then
            // fails with a bare "Failed". Confirmed 2026-11-06 (Bazzite, BlueZ
            // 5.87): the adapter's btstate showed the management client connected
            // over LE while clink.refused_no_mgmt counted the Classic attempts.
            // PreferredBearer would say "LE only", but it is experimental in 5.87
            // and absent there. So: if the LE link is up with services resolved,
            // the failure was only the Classic half, and the session continues.
            // Nothing is trusted on that basis; subscribe and the identity check
            // that follow still have to succeed over this link.
            if (await LeLinkUsableAsync(devicePath).ConfigureAwait(false))
            {
                Log("open", $"connect reported {error.ErrorName} '{error.Message}' but the LE link is up " +
                            "with the management service; continuing (Classic half refused)");
                return;
            }

            var outcome = BluezErrors.Outcome(error);
            RecordFailure(GattFailureStage.Connect, outcome);
            var failure = new GattTransportException(
                $"Bluetooth could not connect to that adapter. ({BluezErrors.StatusName(error)})",
                GattFailureStage.Connect, outcome, protocolError: BluezErrors.AttError(error), innerException: error);
            Log("fail", $"{failure.Describe()} dbus={error}");
            throw failure;
        }
        finally
        {
            watch?.Dispose();
        }
    }

    /// <summary>How long a failed Connect may wait for the LE half to finish resolving.</summary>
    private const int LeLinkGraceMillis = 4000;

    /// <summary>
    /// True when the device is connected with the management service present.
    /// ServicesResolved alone is not enough: a Classic link's SDP sets it too
    /// (Ally, 2026-11-08: "continuing" on a BR/EDR link, then no service).
    /// </summary>
    private async Task<bool> LeLinkUsableAsync(string devicePath)
    {
        using var grace = new CancellationTokenSource(LeLinkGraceMillis);
        try
        {
            return await bus.WaitServicesResolvedAsync(devicePath, grace.Token).ConfigureAwait(false) &&
                   await HasManagementServiceAsync(devicePath).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch (BluezException)
        {
            return false;
        }
    }

    /// <summary>Whether BlueZ holds the management GATT service for this device right now.</summary>
    private async Task<bool> HasManagementServiceAsync(string devicePath)
    {
        try
        {
            return await bus.FindServiceAsync(devicePath, BleManagementContract.ServiceUuid).ConfigureAwait(false)
                is not null;
        }
        catch (BluezException)
        {
            return false;
        }
    }

    /// <summary>
    /// Makes BlueZ see the adapter over LE immediately before Connect.
    /// </summary>
    /// <remarks>
    /// The adapter is dual-mode (Classic for controllers and Controller Link, LE
    /// for management). BlueZ's select_conn_bearer() (src/device.c, 5.87) picks
    /// the bearer for Device1.Connect AND Device1.Pair: the bonded one if only
    /// one is bonded, otherwise the one seen most recently, with a TIE GOING TO
    /// BR/EDR. PreferredBearer and Bearer.LE1.Connect would name the bearer
    /// outright but are experimental and absent on the Ally (BlueZ 5.87).
    ///
    /// One advertisement received by an LE-only discovery makes the LE sighting
    /// the most recent, so a Connect right after it goes over LE -- PROVIDED the
    /// advertisement carries the "BR/EDR Not Supported" flag. Without it BlueZ
    /// (src/adapter.c) refreshes the BR/EDR sighting from the same advertisement,
    /// the tie goes to BR/EDR, and this refresh guarantees the wrong bearer:
    /// firmware before 2026-11-08 advertised flags 0x02 and every Pair and
    /// Connect from the Ally went Classic (Pair "AuthenticationRejected",
    /// Connect "br-connection-refused"). The adapter now advertises 0x06.
    /// A bond made over Classic by such firmware still pins BR/EDR (the bonded
    /// rule comes first); only removing it and pairing again clears that.
    ///
    /// Only a FRESH advertisement counts: the discovery's report of a cached or
    /// already-connected device carries no RSSI. If none arrives in
    /// <see cref="LeRefreshMillis"/> (the adapter is serving another host and not
    /// advertising, say), Connect runs anyway, exactly as before.
    /// </remarks>
    private async Task RefreshLePresenceAsync(string adapterPath, ulong address, CancellationToken cancellationToken)
    {
        var seen = new TaskCompletionSource<short>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var window = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        window.CancelAfter(LeRefreshMillis);
        IAsyncDisposable? scan = null;
        try
        {
            scan = await bus.StartDiscoveryAsync(adapterPath, BleManagementContract.ServiceUuid, advertisement =>
            {
                if (advertisement.Address == address && advertisement.Rssi is { } rssi)
                {
                    seen.TrySetResult(rssi);
                }
            }, window.Token).ConfigureAwait(false);

            using (window.Token.Register(() => seen.TrySetCanceled()))
            {
                var rssi = await seen.Task.ConfigureAwait(false);
                Log("open", $"seen over LE (rssi={rssi}) just before connect");
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            Log("open", $"no LE advertisement within {LeRefreshMillis} ms; connecting anyway");
        }
        catch (BluezException error)
        {
            Log("open", $"LE refresh scan unavailable ({error.ErrorName}); connecting anyway");
        }
        finally
        {
            if (scan is not null)
            {
                await scan.DisposeAsync().ConfigureAwait(false);
            }
        }
    }

    private async Task<T> GuardAsync<T>(GattFailureStage stage, string message, Func<Task<T>> operation)
    {
        try
        {
            return await operation().ConfigureAwait(false);
        }
        catch (BluezException error)
        {
            var outcome = BluezErrors.Outcome(error);
            RecordFailure(stage, outcome);
            var failure = new GattTransportException($"{message} ({BluezErrors.StatusName(error)})", stage, outcome,
                protocolError: BluezErrors.AttError(error), innerException: error);
            Log("fail", $"{failure.Describe()} dbus={error}");
            throw failure;
        }
    }

    private Task GuardAsync(GattFailureStage stage, string message, Func<Task> operation) =>
        GuardAsync(stage, message, async () =>
        {
            await operation().ConfigureAwait(false);
            return true;
        });

    /// <summary>
    /// Record what one failure says about the peer, with the same stage rule
    /// the Windows transport learned on hardware: only failures reaching the
    /// GATT server for the FIRST time (services, subscribe) corroborate a bond
    /// mismatch. A command failure on a subscribed session never does.
    /// </summary>
    private void RecordFailure(GattFailureStage stage, GattCommunicationOutcome outcome)
    {
        lock (gate)
        {
            if (outcome != GattCommunicationOutcome.Unreachable)
            {
                peerAnsweredGatt = true;
            }
            else if (stage is GattFailureStage.Services or GattFailureStage.Subscribe)
            {
                linkFailuresAfterResolve += 1;
            }
        }
    }

    private GattTransportException Refused(GattFailureStage stage, string message)
    {
        // A missing service or characteristic is an attribute-layer ANSWER: the
        // peer was reached and described itself.
        RecordFailure(stage, GattCommunicationOutcome.ProtocolError);
        return new GattTransportException(message, stage, GattCommunicationOutcome.ProtocolError);
    }

    private void Publish(
        ConnectionPhase phase,
        string? deviceName = null,
        string? address = null,
        string? message = null)
    {
        bool identityVerified;
        long attempt;
        lock (gate)
        {
            identityVerified = validated;
            attempt = generation;
        }

        connection.Set(new ConnectionState
        {
            Phase = phase == ConnectionPhase.Connected && !identityVerified ? ConnectionPhase.Connecting : phase,
            DeviceName = deviceName,
            Address = address,
            Message = message,
            Attempt = (int)Math.Min(attempt, int.MaxValue),
        });
    }

    private void Log(string stage, string detail) => log?.Invoke("ble", $"{stage} {detail}");

    private sealed class PendingReply
    {
        public BleReplyAssembler Assembler { get; } = new();

        public TaskCompletionSource<string> Completion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    /// <summary>One physical BlueZ connection and everything subscribed on it.</summary>
    private sealed class OwnedLink(
        BluezManagementTransport transport,
        string devicePath,
        long generation,
        string address,
        string? displayName) : IAsyncDisposable
    {
        private BluezControllerLinkDataPlane? dataPlane;

        public long Generation { get; } = generation;

        public string Address { get; } = address;

        public string? DisplayName { get; } = displayName;

        public bool Closed { get; private set; }

        public bool Connected { get; set; }

        public long LastReplyAtMillis { get; set; }

        public int AttMtu { get; set; } = BleManagementContract.DefaultAttMtu;

        public BluezGattService Service { get; set; } = new(string.Empty, new Dictionary<string, string>());

        public string? Rx { get; set; }

        public string? Tx { get; set; }

        public IAsyncDisposable? Notifications { get; set; }

        public IDisposable? DisconnectWatch { get; set; }

        public void OnNotification(byte[] payload) => transport.OnNotification(Generation, Closed, payload);

        public void Attach(BluezControllerLinkDataPlane plane) => dataPlane = plane;

        public async ValueTask DisposeAsync()
        {
            if (Closed)
            {
                return;
            }

            Closed = true;
            DisconnectWatch?.Dispose();

            // Controller Link cannot outlive its carrier.
            dataPlane?.NotifyClosed("The management connection ended.");

            if (Notifications is not null)
            {
                await Notifications.DisposeAsync().ConfigureAwait(false);
            }

            // Disconnect explicitly: BlueZ keeps an LE link up while any client
            // holds it, and the adapter has room for exactly one management
            // session.
            if (Connected)
            {
                await transport.bus.DisconnectAsync(devicePath).ConfigureAwait(false);
            }
        }
    }
}
