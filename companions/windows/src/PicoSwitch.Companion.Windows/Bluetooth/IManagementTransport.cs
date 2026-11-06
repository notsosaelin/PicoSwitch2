using PicoSwitch.Bridge.Core;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Windows.Bluetooth;

public enum ConnectionPhase
{
    Idle,
    Discovering,
    Pairing,
    Connecting,
    Connected,
    Reconnecting,
    Disconnecting,
    Failed,
    RepairRequired,
}

public sealed record ConnectionState
{
    public ConnectionPhase Phase { get; init; } = ConnectionPhase.Idle;

    public string? DeviceName { get; init; }

    public string? Address { get; init; }

    public string? Message { get; init; }

    public int Attempt { get; init; }

    public bool Connected => Phase == ConnectionPhase.Connected;
}

/// <summary>
/// One advertised management endpoint, as discovery found it.
///
/// Carries the address rather than a device object: the transport resolves the
/// object when it is ready to connect, and holding a <c>BluetoothLEDevice</c>
/// across a discovery result is how a stale handle survives into the next
/// attempt.
///
/// It deliberately carries no WinRT device path either. An advertisement does not
/// have one, so the field that used to be here was always null — and code that
/// branched on it therefore always took the null path. That is what made Repair
/// silently do nothing; see <c>WindowsAdapterPairing.UnpairByAddressAsync</c>.
/// The Bluetooth address is the durable identifier, and it is enough to re-resolve
/// everything Windows knows.
/// </summary>
public sealed record DiscoveredManagementPeer(
    ulong BluetoothAddress,
    string Address,
    string? DisplayName = null,
    short? SignalStrengthDbm = null);

/// <summary>
/// Why a connection is being attempted, for the diagnostic trail.
///
/// Every field is diagnostic except <c>PriorGattRetired</c>, which is a fact the
/// transport needs: on Windows a connect cannot be cancelled, so a retry must
/// fully dispose the previous <c>GattSession</c> and <c>BluetoothLEDevice</c>
/// before creating new ones, and this records that it did.
/// </summary>
public sealed record ManagementConnectionContext
{
    public long LogicalAttempt { get; init; }

    public string Reason { get; init; } = "unspecified";

    public string PairingState { get; init; } = "unknown";

    public int Retry { get; init; }

    public bool PriorGattRetired { get; init; }
}

/// <summary>
/// One management carrier: discovery, connection lifecycle, and the single
/// logical exchange the protocol layer needs.
///
/// Extends <see cref="IManagementChannel"/> rather than wrapping it, so
/// <see cref="ManagementClient"/> can be pointed straight at a live transport
/// while the lifecycle half stays out of the portable core.
/// </summary>
public interface IManagementTransport : IManagementChannel, IAsyncDisposable
{
    IReadOnlyStateValue<ConnectionState> Connection { get; }

    void PrepareConnection(ManagementConnectionContext context);

    /// <summary>Discover the advertised management endpoint without opening GATT.</summary>
    Task<DiscoveredManagementPeer> DiscoverAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Scan for the management service and connect to what is found.
    ///
    /// <paramref name="expectedAddress"/> restricts the scan to one adapter.
    /// Passing null is only correct during first pairing: for a remembered
    /// adapter, discovering another valid Pico nearby is not permission to
    /// silently replace the user's relationship.
    /// </summary>
    Task ScanAndConnectAsync(string? expectedAddress = null, CancellationToken cancellationToken = default);

    Task ConnectKnownAsync(string address, CancellationToken cancellationToken = default);

    Task DisconnectAsync();

    /// <summary>Promote a subscribed link only after the management identity reply is verified.</summary>
    void MarkValidated();

    /// <summary>
    /// The evidence <see cref="AdapterResetSignature"/> weighs beside the failure
    /// itself, accumulated across one LOGICAL attempt.
    ///
    /// Read after the attempt has failed and its objects are disposed, which is
    /// why the transport records these rather than exposing the live connection.
    /// </summary>
    TransportTrustSnapshot Trust { get; }
}

/// <summary>
/// What one logical connect attempt established about the peer.
///
/// The first version of this record carried a single <c>PeerReachable</c> flag
/// set by two unrelated events — an advertisement arriving, and a GATT operation
/// returning something other than <c>Unreachable</c>. Those are different facts
/// with different force, and flattening them made the one shape the hardware
/// actually produces inexpressible. They are separate here, deliberately.
/// </summary>
/// <param name="WindowsPaired">
/// Windows reported the peer as paired when the device was opened.
/// </param>
/// <param name="PeerObserved">
/// The EXACT expected address was seen advertising the management service inside
/// this attempt. A powered-off, absent or out-of-range adapter cannot satisfy
/// this, which is what makes it safe to reason from.
/// </param>
/// <param name="PeerAnsweredGatt">
/// A GATT operation returned an attribute-layer answer — even a refusal. This is
/// strictly stronger than <paramref name="PeerObserved"/> but is produced by a
/// different layer, and this hardware never produces it during a bond mismatch.
/// </param>
/// <param name="LinkFailuresAfterResolve">
/// How many separately resolved <c>BluetoothLEDevice</c> objects failed with
/// <c>Unreachable</c> at or after service discovery. One is noise; two is the
/// direct connect and the fresh scan-resolved connect agreeing.
///
/// Counted only for failures reaching the GATT SERVER for the first time —
/// service discovery, characteristic resolution and the CCC write. A command on
/// an already-subscribed session is deliberately excluded: by then the bond has
/// already been proven (see <paramref name="BondProven"/>), so a failure there
/// is a mid-session link event and cannot corroborate a bond mismatch.
/// </param>
/// <param name="BondProven">
/// This attempt completed service discovery, characteristic resolution AND the
/// CCC write against the adapter.
///
/// **This is positive evidence, and it is decisive.** Every one of those handles
/// is declared <c>ATT_SECURITY_ENCRYPTED</c> in the firmware's ATT database, so
/// reaching the end of that sequence means Windows and the adapter agreed on a
/// key. A stale bond cannot get there — it fails below the attribute layer,
/// which is the entire premise of <c>AdapterResetSignature</c>.
///
/// It exists because the signature was reachable without it. A management
/// command that failed <c>Unreachable</c> mid-session fed the same counter as a
/// connect-time failure, so a working, correctly bonded adapter that went
/// briefly unreachable during an upload could be diagnosed as reset and offered
/// a Repair that would have destroyed a good pairing. Observed 2026-08-31.
/// </param>
public readonly record struct TransportTrustSnapshot(
    bool WindowsPaired,
    bool PeerObserved,
    bool PeerAnsweredGatt = false,
    int LinkFailuresAfterResolve = 0,
    bool BondProven = false);
