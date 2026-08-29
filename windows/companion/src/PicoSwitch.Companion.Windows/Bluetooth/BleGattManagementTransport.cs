using System.Text;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Management;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Storage.Streams;

namespace PicoSwitch.Companion.Windows.Bluetooth;

/// <summary>
/// The BLE GATT management carrier for Windows.
///
/// One adapter, one link, one exchange at a time. Everything platform-specific
/// about reaching a PicoSwitch2 over LE lives here; everything about what to say
/// to it lives in <c>PicoSwitch.Management.Core</c>.
///
/// ## Windows differences that shape this class
///
/// - **A connect cannot be cancelled.** There is no WinRT equivalent of
///   `BluetoothGatt.disconnect()` mid-attempt: `GattSession` with
///   `MaintainConnection = true` keeps trying until it is DISPOSED. So "ready"
///   here means the composite *services resolved and CCC written*, the deadline
///   is enforced around that composite, and every retry begins by fully disposing
///   the previous session and device.
/// - **Disposal is what disconnects.** Nothing else does. A retired
///   `GattSession` left alive keeps Windows reconnecting behind the app's back.
/// - **Service discovery is cached by default.** A reflashed adapter can be
///   served from a stale cache, so the first resolution of every session is
///   `BluetoothCacheMode.Uncached`.
/// - **HCI status codes are not exposed to user mode.** Failures carry
///   `GattCommunicationStatus`, the ATT error byte and an `HRESULT` instead —
///   see <see cref="GattTransportException"/> and
///   <see cref="AdapterResetSignature"/>.
///
/// ## Validation status
///
/// **Implementation complete; hardware validation pending.** No part of this
/// class has been executed against a PicoSwitch2 adapter. The two things that
/// must be measured rather than reasoned about are named in WINDOWS_PASS.md
/// §31 Phase 2: that a reflashed adapter reaches `RepairRequired` on the FIRST
/// attempt, and that `mgmt_watch.ps1` shows one client with no churn.
/// </summary>
public sealed class BleGattManagementTransport : IManagementTransport
{
    private const int ConnectTimeoutMillis = 15_000;
    private const int PairingConnectTimeoutMillis = 60_000;
    private const int ScanTimeoutMillis = 15_000;
    private const int DisconnectTimeoutMillis = 1_250;

    private static readonly Guid ServiceUuid = new(BleManagementContract.ServiceUuid);
    private static readonly Guid RxUuid = new(BleManagementContract.RxUuid);
    private static readonly Guid TxUuid = new(BleManagementContract.TxUuid);

    private readonly SerializedManagementSession session = new();
    private readonly StateValue<ConnectionState> connection = new(new ConnectionState());
    private readonly Lock gate = new();
    private readonly Action<string, string>? log;

    private OwnedGatt? owned;
    private long generation;
    private ManagementConnectionContext context = new();
    private bool disposed;
    private bool validated;

    /*
     * The evidence AdapterResetSignature weighs, recorded per LOGICAL ATTEMPT
     * rather than read off the live connection.
     *
     * This is not a style choice. Reading them from the connection could never
     * work: by the time a caller asks whether a failure was a bond mismatch, the
     * failed attempt's objects have already been disposed, so `WindowsPaired`
     * would always read false and the signature could never fire. They are
     * deliberately NOT cleared by teardown -- teardown runs BEFORE the caller
     * classifies the failure.
     *
     * They are cleared when the LOGICAL attempt changes (see PrepareConnection),
     * not per physical connect: one logical attempt spans the direct connect, any
     * clean retry and the address-restricted fallback, and the corroboration the
     * signature needs is precisely that those separate connects agree. Clearing
     * per physical connect would erase the advertisement the fallback scan just
     * found; never clearing would let a session from ten minutes ago vouch for a
     * peer that is now switched off.
     */
    private long evidenceAttempt = long.MinValue;
    private bool attemptPaired;
    private bool peerObserved;
    private bool peerAnsweredGatt;
    private int linkFailuresAfterResolve;

    /// <summary>
    /// The reply currently being assembled, if any.
    ///
    /// Set by the exchange under the single-flight lock, read by the notification
    /// handler on a pool thread. Volatile rather than locked because the handler
    /// runs at notification rate and the only write is the exchange's own.
    /// </summary>
    private volatile PendingReply? pending;

    public BleGattManagementTransport(Action<string, string>? log = null) => this.log = log;

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
                    linkFailuresAfterResolve);
            }
        }
    }

    public void PrepareConnection(ManagementConnectionContext next)
    {
        lock (gate)
        {
            context = next;

            // A NEW logical attempt starts with no evidence. Within one logical
            // attempt -- direct, retry, fallback -- it accumulates.
            if (next.LogicalAttempt != evidenceAttempt)
            {
                evidenceAttempt = next.LogicalAttempt;
                attemptPaired = false;
                peerObserved = false;
                peerAnsweredGatt = false;
                linkFailuresAfterResolve = 0;
            }
        }

        Log("prepare",
            $"reason={next.Reason} attempt={next.LogicalAttempt} retry={next.Retry} " +
            $"pairing={next.PairingState} priorRetired={next.PriorGattRetired}");
    }

    /// <summary>
    /// Watch for the management service and return the first accepted result.
    ///
    /// The watcher is stopped on the first accepted advertisement, exactly as the
    /// Android callback does: a watcher left running keeps the radio scanning
    /// through the whole connect, which is both a battery cost and a source of
    /// duplicate results the lifecycle then has to make inert.
    ///
    /// UUID matching is the discovery authority. A friendly name is not
    /// authentication and never gates acceptance.
    /// </summary>
    public async Task<DiscoveredManagementPeer> DiscoverAsync(
        CancellationToken cancellationToken = default) =>
        await ScanAsync(expectedAddress: null, cancellationToken).ConfigureAwait(false);

    public async Task ScanAndConnectAsync(
        string? expectedAddress = null,
        CancellationToken cancellationToken = default)
    {
        Publish(ConnectionPhase.Discovering, message: "Looking for the adapter.");
        var peer = await ScanAsync(expectedAddress, cancellationToken).ConfigureAwait(false);
        await ConnectAsync(peer.BluetoothAddress, peer.DisplayName, cancellationToken)
            .ConfigureAwait(false);
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
        var capabilities = await WindowsBluetoothRadio.ProbeAsync().ConfigureAwait(false);
        WindowsBluetoothRadio.RequireManagementCapable(capabilities);

        ulong? wanted = null;
        if (expectedAddress is not null)
        {
            if (!BluetoothAddressFormat.TryParse(expectedAddress, out var numeric))
            {
                throw new ManagementException($"Not a Bluetooth address: {expectedAddress}");
            }

            wanted = numeric;
        }

        var completion = new TaskCompletionSource<DiscoveredManagementPeer>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var watcher = new BluetoothLEAdvertisementWatcher
        {
            // Active scanning so the scan response carries the local name; the
            // service UUID in the filter is what actually decides acceptance.
            ScanningMode = BluetoothLEScanningMode.Active,
        };
        watcher.AdvertisementFilter.Advertisement.ServiceUuids.Add(ServiceUuid);

        void OnReceived(
            BluetoothLEAdvertisementWatcher sender,
            BluetoothLEAdvertisementReceivedEventArgs args)
        {
            if (wanted is { } expected && args.BluetoothAddress != expected)
            {
                // Restricted to the selected adapter's address. Seeing another
                // valid Pico nearby is never permission to substitute it.
                return;
            }

            if (BluetoothAddressFormat.ToText(args.BluetoothAddress) is not { } text)
            {
                return;
            }

            lock (gate)
            {
                // The exact expected address, seen advertising the management
                // service. On the remembered-adapter path `wanted` is set, so this
                // cannot be some other Pico standing in for the user's.
                peerObserved = true;
            }

            sender.Stop();
            completion.TrySetResult(new DiscoveredManagementPeer(
                args.BluetoothAddress,
                text,
                DisplayName: string.IsNullOrWhiteSpace(args.Advertisement.LocalName)
                    ? null
                    : args.Advertisement.LocalName,
                SignalStrengthDbm: args.RawSignalStrengthInDBm));
        }

        watcher.Received += OnReceived;

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(ScanTimeoutMillis);

        try
        {
            watcher.Start();
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
        finally
        {
            watcher.Received -= OnReceived;
            if (watcher.Status == BluetoothLEAdvertisementWatcherStatus.Started)
            {
                watcher.Stop();
            }
        }
    }

    /* ----------------------------------------------------------- connection */

    private async Task ConnectAsync(
        ulong address,
        string? displayName,
        CancellationToken cancellationToken)
    {
        var text = BluetoothAddressFormat.ToText(address) ??
            throw new ManagementException("Not a Bluetooth address");

        // A previous attempt must be completely gone before a new one exists: a
        // live GattSession keeps Windows reconnecting behind the app's back.
        await RetireAsync("reconnect").ConfigureAwait(false);

        long attemptGeneration;
        bool expectsPairing;
        lock (gate)
        {
            attemptGeneration = ++generation;
            validated = false;
            expectsPairing = context.PairingState is "notpaired" or "pairing";

            // Re-read per physical connect; the rest of the evidence belongs to the
            // logical attempt and is reset in PrepareConnection.
            attemptPaired = false;
        }

        Publish(ConnectionPhase.Connecting, displayName, text, "Connecting.");

        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(expectsPairing ? PairingConnectTimeoutMillis : ConnectTimeoutMillis);

        OwnedGatt? candidate = null;
        try
        {
            candidate = await OpenAsync(address, text, displayName, attemptGeneration, deadline.Token)
                .ConfigureAwait(false);

            lock (gate)
            {
                if (attemptGeneration != generation)
                {
                    // A newer attempt superseded this one while it was opening.
                    // Dispose what was built rather than publishing it.
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

    private async Task<OwnedGatt> OpenAsync(
        ulong address,
        string text,
        string? displayName,
        long attemptGeneration,
        CancellationToken cancellationToken)
    {
        var device = await GuardAsync(
            GattFailureStage.Connect,
            "Windows could not open a connection to that adapter.",
            () => BluetoothLEDevice.FromBluetoothAddressAsync(address).AsTask(cancellationToken))
            .ConfigureAwait(false) ??
            throw new GattTransportException(
                "Windows could not open a connection to that adapter.",
                GattFailureStage.Connect);

        var paired = false;
        try
        {
            var info = await DeviceInformation.CreateFromIdAsync(device.DeviceId).AsTask(cancellationToken)
                .ConfigureAwait(false);
            paired = info.Pairing.IsPaired;
        }
        catch (Exception)
        {
            // Pairing state is diagnostic input for the bond-mismatch signature,
            // not a precondition. An unreadable state means the signature cannot
            // fire, which is the safe direction: it never invents a repair.
        }

        lock (gate)
        {
            attemptPaired = paired;
        }

        Log("open", $"device resolved paired={paired}");

        var gattSession = await GuardAsync(
            GattFailureStage.Connect,
            "Windows could not open a GATT session with that adapter.",
            () => GattSession.FromDeviceIdAsync(device.BluetoothDeviceId).AsTask(cancellationToken))
            .ConfigureAwait(false);

        // The only way to ask Windows to keep the link up. It is also the only way
        // to ask it to stop: clearing this and disposing the session is what
        // actually disconnects.
        gattSession.MaintainConnection = true;

        var owner = new OwnedGatt(this, device, gattSession, attemptGeneration, text, displayName, paired);
        try
        {
            // Uncached on the first resolution of every session. A reflashed
            // adapter can otherwise be served the shape it had before the flash.
            //
            // This is load-bearing for the stale-bond diagnosis, not a nicety: it
            // is the reason a `services Unreachable` cannot be explained away as a
            // cache artefact. A cache hit would SUCCEED, returning the pre-flash
            // shape; it could not return Unreachable.
            var services = await GuardAsync(
                GattFailureStage.Services,
                "The adapter refused service discovery.",
                () => device.GetGattServicesForUuidAsync(ServiceUuid, BluetoothCacheMode.Uncached)
                    .AsTask(cancellationToken)).ConfigureAwait(false);
            if (services.Status != GattCommunicationStatus.Success)
            {
                LogLinkState(device, gattSession, services.Status);
            }

            RequireSuccess(services.Status, services.ProtocolError, GattFailureStage.Services,
                "The adapter did not expose its management service.");

            var service = services.Services.FirstOrDefault() ??
                throw new GattTransportException(
                    "The adapter did not expose its management service.",
                    GattFailureStage.Services,
                    ToOutcome(services.Status));

            owner.Service = service;
            owner.Rx = await RequireCharacteristicAsync(service, RxUuid, cancellationToken)
                .ConfigureAwait(false);
            owner.Tx = await RequireCharacteristicAsync(service, TxUuid, cancellationToken)
                .ConfigureAwait(false);

            owner.Tx.ValueChanged += owner.OnNotification;
            owner.Subscribed = true;

            // The session is NOT ready until this succeeds. Subscribing after the
            // first command would let a reply arrive with nobody listening.
            var ccc = await GuardAsync(
                GattFailureStage.Subscribe,
                "The adapter refused to enable management notifications.",
                () => owner.Tx
                    .WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.Notify)
                    .AsTask(cancellationToken)).ConfigureAwait(false);
            RequireSuccess(ccc.Status, ccc.ProtocolError, GattFailureStage.Subscribe,
                "The adapter refused to enable management notifications.");

            return owner;
        }
        catch
        {
            await owner.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    private async Task<GattCharacteristic> RequireCharacteristicAsync(
        GattDeviceService service,
        Guid uuid,
        CancellationToken cancellationToken)
    {
        var result = await GuardAsync(
            GattFailureStage.Services,
            "The adapter refused to expose a management characteristic.",
            () => service.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode.Uncached)
                .AsTask(cancellationToken)).ConfigureAwait(false);
        RequireSuccess(result.Status, result.ProtocolError, GattFailureStage.Services,
            $"The adapter's management service is missing characteristic {uuid}.");

        return result.Characteristics.FirstOrDefault() ??
            throw new GattTransportException(
                $"The adapter's management service is missing characteristic {uuid}.",
                GattFailureStage.Services,
                ToOutcome(result.Status));
    }

    /* ------------------------------------------------------------- exchange */

    private async Task<string> ExchangeAsync(string command, long timeoutMillis)
    {
        OwnedGatt owner;
        lock (gate)
        {
            owner = owned ?? throw new ManagementException("No adapter is connected.");
            if (owner.Generation != generation || owner.Closed)
            {
                throw new ManagementException("The management session was replaced.");
            }
        }

        var reply = new PendingReply();
        pending = reply;
        try
        {
            foreach (var chunk in BleManagementContract.CommandChunks(
                         command,
                         BleManagementContract.AttPayloadWithDefaultMtu))
            {
                var writer = new DataWriter();
                writer.WriteBytes(chunk);
                var result = await GuardAsync(
                    GattFailureStage.Command,
                    "The adapter refused a management command.",
                    () => owner.Rx!
                        .WriteValueWithResultAsync(writer.DetachBuffer(), GattWriteOption.WriteWithResponse)
                        .AsTask()).ConfigureAwait(false);
                RequireSuccess(result.Status, result.ProtocolError, GattFailureStage.Command,
                    $"The adapter refused the command '{command}'.");
            }

            var completed = await Task.WhenAny(
                reply.Completion.Task,
                Task.Delay(TimeSpan.FromMilliseconds(timeoutMillis))).ConfigureAwait(false);

            if (completed != reply.Completion.Task)
            {
                // A failure AFTER transmit invalidates the session. Never let the
                // next request consume a late reply: management replies carry no
                // request identifier, so a late one is indistinguishable from the
                // next one's.
                await RetireAsync("reply-timeout").ConfigureAwait(false);
                throw new ManagementException(
                    $"The adapter did not answer '{command}' within {timeoutMillis} ms.");
            }

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

    private void OnNotification(long callbackGeneration, bool ownerClosed, byte[] payload)
    {
        long current;
        lock (gate)
        {
            current = generation;
        }

        if (!GattCallbackAuthority.IsAuthoritative(current, callbackGeneration, ownerClosed))
        {
            // A retired connection's handler must not mutate live state.
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

    private async Task RetireAsync(string reason)
    {
        OwnedGatt? retiring;
        lock (gate)
        {
            retiring = owned;
            owned = null;
            validated = false;

            // attemptPaired / peerAnswered are deliberately NOT reset here.
            // Teardown runs BEFORE the caller classifies the failure, and clearing
            // them would delete the evidence the classification depends on.
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
        await retiring.DisposeAsync().WaitAsync(TimeSpan.FromMilliseconds(DisconnectTimeoutMillis))
            .ConfigureAwait(false);
        Log("retire", $"reason={reason} gen={retiring.Generation}");
        Publish(ConnectionPhase.Idle, message: null);
    }

    /* --------------------------------------------------------------- helpers */

    /// <summary>
    /// Run one WinRT call and turn ANY thrown failure into a tagged
    /// <see cref="GattTransportException"/> carrying its <c>HRESULT</c>.
    ///
    /// Without this the HRESULT half of <see cref="AdapterResetSignature"/> is
    /// unreachable. A stale-bond refusal that surfaces as a thrown
    /// <c>E_BLUETOOTH_ATT_INSUFFICIENT_AUTHENTICATION</c> would propagate as a
    /// bare exception, the signature would find no transport failure to inspect,
    /// and the user would get an ordinary connect error and a pointless retry
    /// instead of Repair.
    /// </summary>
    private async Task<T> GuardAsync<T>(
        GattFailureStage stage,
        string message,
        Func<Task<T>> operation)
    {
        try
        {
            return await operation().ConfigureAwait(false);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            var failure = new GattTransportException(
                message,
                stage,
                outcome: null,
                protocolError: null,
                hresult: error.HResult,
                innerException: error);
            Log("fail", failure.Describe());
            throw failure;
        }
    }

    private void RequireSuccess(
        GattCommunicationStatus status,
        byte? protocolError,
        GattFailureStage stage,
        string message)
    {
        lock (gate)
        {
            if (status != GattCommunicationStatus.Unreachable)
            {
                // The peer answered at the ATTRIBUTE layer -- even a refusal.
                peerAnsweredGatt = true;
            }
            else if (stage != GattFailureStage.Connect)
            {
                // Windows opened the device and then could not reach its GATT
                // server. Counted per resolved device object: two of these inside
                // one logical attempt is the direct connect and the fresh
                // scan-resolved connect independently agreeing, which is the
                // corroboration the link-refusal shape requires.
                linkFailuresAfterResolve += 1;
            }
        }

        if (status == GattCommunicationStatus.Success)
        {
            return;
        }

        var failure = new GattTransportException(message, stage, ToOutcome(status), protocolError);
        Log("fail", failure.Describe());
        throw failure;
    }

    /// <summary>
    /// What the LINK was doing when service discovery failed.
    ///
    /// Added after the 2026-08-29 stale-bond run, which established the observable
    /// shape (`services Unreachable`, no ATT byte, no HRESULT) but could not
    /// distinguish "Windows never established a link" from "Windows established a
    /// link and it dropped when encryption failed". Both are consistent with a
    /// bond mismatch; only the second would let the DIRECT connect prove the peer
    /// is physically present, which would in turn allow first-attempt
    /// classification without waiting for the fallback scan to see an
    /// advertisement.
    ///
    /// Diagnostic ONLY. It is deliberately not part of
    /// <see cref="AdapterResetSignature"/> yet: what these properties read at this
    /// exact moment has not been measured, and encoding a guess is how the first
    /// version of that signature came to be wrong. Promote it only if a run shows
    /// it reads reliably.
    /// </summary>
    private void LogLinkState(
        BluetoothLEDevice device,
        GattSession session,
        GattCommunicationStatus status)
    {
        try
        {
            Log("link",
                $"status={status} connection={device.ConnectionStatus} " +
                $"session={session.SessionStatus} maxPdu={session.MaxPduSize}");
        }
        catch (Exception error)
        {
            // Reading a disposed or torn-down projection must never replace the
            // failure it was trying to describe.
            Log("link", $"status={status} unreadable ({error.GetType().Name})");
        }
    }

    private static GattCommunicationOutcome ToOutcome(GattCommunicationStatus status) => status switch
    {
        GattCommunicationStatus.Success => GattCommunicationOutcome.Success,
        GattCommunicationStatus.Unreachable => GattCommunicationOutcome.Unreachable,
        GattCommunicationStatus.ProtocolError => GattCommunicationOutcome.ProtocolError,
        _ => GattCommunicationOutcome.AccessDenied,
    };

    /// <summary>
    /// Publish a connection state, refusing to claim <c>Connected</c> before the
    /// identity reply has been verified.
    ///
    /// The downgrade is not defensive decoration: it is I3 expressed at the
    /// transport as well as in the lifecycle coordinator. A subscribed GATT link
    /// is a working CARRIER, and the product's "Connected" means a verified
    /// PicoSwitch2. Any future path that publishes Connected without going through
    /// <see cref="MarkValidated"/> is a path that would show a stranger's device
    /// as the user's adapter, and it is corrected here rather than trusted.
    /// </summary>
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
            Phase = phase == ConnectionPhase.Connected && !identityVerified
                ? ConnectionPhase.Connecting
                : phase,
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

    /// <summary>
    /// One connection attempt's WinRT objects, disposed as a unit.
    ///
    /// Disposal ORDER matters: unsubscribe first so no notification can arrive
    /// against a half-disposed owner, then the session (which is what actually
    /// drops the link), then the device.
    /// </summary>
    private sealed class OwnedGatt(
        BleGattManagementTransport transport,
        BluetoothLEDevice device,
        GattSession session,
        long generation,
        string address,
        string? displayName,
        bool windowsPaired)
    {
        public long Generation { get; } = generation;

        public string Address { get; } = address;

        public string? DisplayName { get; } = displayName;

        public bool WindowsPaired { get; } = windowsPaired;

        public bool Closed { get; private set; }

        public bool Subscribed { get; set; }

        public GattDeviceService? Service { get; set; }

        public GattCharacteristic? Rx { get; set; }

        public GattCharacteristic? Tx { get; set; }

        public void OnNotification(GattCharacteristic sender, GattValueChangedEventArgs args)
        {
            var payload = new byte[args.CharacteristicValue.Length];
            DataReader.FromBuffer(args.CharacteristicValue).ReadBytes(payload);
            transport.OnNotification(Generation, Closed, payload);
        }

        public async Task DisposeAsync()
        {
            Closed = true;

            if (Subscribed && Tx is not null)
            {
                Tx.ValueChanged -= OnNotification;
                try
                {
                    await Tx.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                        GattClientCharacteristicConfigurationDescriptorValue.None).AsTask()
                        .ConfigureAwait(false);
                }
                catch (Exception)
                {
                    // Best effort. The link is going away regardless, and a failed
                    // unsubscribe must not stop the disposal that actually closes it.
                }
            }

            Service?.Dispose();

            // MaintainConnection is cleared before disposal so Windows stops trying
            // to re-establish the link the moment the session goes.
            try
            {
                session.MaintainConnection = false;
            }
            catch (Exception)
            {
                // A session already torn down by the stack throws here.
            }

            session.Dispose();
            device.Dispose();
        }
    }
}
