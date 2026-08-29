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
/// Carries the WinRT device id rather than a device object: the transport
/// resolves the object when it is ready to connect, and holding a
/// <c>BluetoothLEDevice</c> across a discovery result is how a stale handle
/// survives into the next attempt.
/// </summary>
public sealed record DiscoveredManagementPeer(
    ulong BluetoothAddress,
    string Address,
    string? DeviceId = null,
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
    /// Whether Windows currently reports this adapter as paired, and whether it
    /// was seen advertising inside the attempt window.
    ///
    /// Both halves feed <see cref="AdapterResetSignature"/>, which cannot decide
    /// anything without them: a refusal from an unpaired device is "not paired",
    /// and a refusal from a device that was never heard from is "out of range".
    /// </summary>
    TransportTrustSnapshot Trust { get; }
}

/// <summary>The two facts the bond-mismatch signature needs beside the failure itself.</summary>
public readonly record struct TransportTrustSnapshot(bool WindowsPaired, bool PeerReachable);
