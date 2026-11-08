namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// The BlueZ operations the Linux backend performs, behind a seam.
///
/// ## Why a seam and not direct D-Bus calls
///
/// The management transport carries the rules that took the Windows companion
/// several hardware runs to get right: one live session, generation-guarded
/// callbacks, per-logical-attempt trust evidence, the one-slot turnaround gap,
/// and retire-before-reconnect. Those rules are worth testing without a radio,
/// and a fake of this interface is what makes them testable on any build agent.
///
/// It is deliberately the exact surface the transport, the pairing gateway and
/// the radio probe need, expressed in plain types. It is not a general BlueZ
/// binding, and widening it should be justified by a caller.
///
/// Every failure is a <see cref="BluezException"/> carrying the D-Bus error
/// name, so callers classify on <c>org.bluez.Error.*</c> rather than on text.
/// </summary>
internal interface IBluezBus : IAsyncDisposable
{
    /// <summary>The default adapter (the first <c>org.bluez.Adapter1</c>), or null when there is none.</summary>
    Task<BluezAdapter?> GetAdapterAsync(CancellationToken cancellationToken = default);

    /// <summary>The device object for an address under the adapter, or null when BlueZ has none.</summary>
    Task<BluezDevice?> FindDeviceAsync(
        string adapterPath,
        ulong address,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Start an LE discovery filtered to one service UUID and report every
    /// matching device seen, including devices BlueZ already knew about. Disposing
    /// the result stops discovery and removes the signal subscriptions.
    /// </summary>
    Task<IAsyncDisposable> StartDiscoveryAsync(
        string adapterPath,
        string serviceUuid,
        Action<BluezAdvertisement> found,
        CancellationToken cancellationToken = default);

    Task ConnectAsync(string devicePath, CancellationToken cancellationToken = default);

    Task DisconnectAsync(string devicePath);

    /// <summary>Wait until <c>ServicesResolved</c> is true. False when the device disconnected first.</summary>
    Task<bool> WaitServicesResolvedAsync(string devicePath, CancellationToken cancellationToken = default);

    /// <summary>The GATT service with this UUID under the device, with its characteristics, or null.</summary>
    Task<BluezGattService?> FindServiceAsync(
        string devicePath,
        string serviceUuid,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// <c>GattCharacteristic1.WriteValue</c>. <paramref name="withResponse"/>
    /// selects the ATT Write Request (<c>type=request</c>) versus Write Command
    /// (<c>type=command</c>, write-without-response).
    /// </summary>
    Task WriteAsync(
        string characteristicPath,
        byte[] value,
        bool withResponse,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// Subscribe to <c>Value</c> changes, then <c>StartNotify</c>. The match is
    /// added first so no notification can arrive with nobody listening. Disposing
    /// stops notification and removes the match.
    /// </summary>
    Task<IAsyncDisposable> SubscribeAsync(
        string characteristicPath,
        Action<byte[]> notification,
        CancellationToken cancellationToken = default);

    /// <summary>The characteristic's <c>MTU</c> property (BlueZ 5.62+), or null when not exposed.</summary>
    Task<int?> GetMtuAsync(string characteristicPath, CancellationToken cancellationToken = default);

    /// <summary>Raise <paramref name="disconnected"/> once when the device's <c>Connected</c> turns false.</summary>
    Task<IDisposable> WatchDisconnectAsync(
        string devicePath,
        Action disconnected,
        CancellationToken cancellationToken = default);

    /// <summary>
    /// <c>Device1.Disconnected(s name, s message)</c> (BlueZ 5.82+): BlueZ's own
    /// reason a link ended, such as <c>org.bluez.Reason.Authentication</c>. It is
    /// the only place BlueZ says WHY a connect failed; the method error is a
    /// generic <c>org.bluez.Error.Failed</c>. An older BlueZ never raises it.
    /// </summary>
    Task<IDisposable> WatchDisconnectReasonAsync(
        string devicePath,
        Action<string, string> disconnected,
        CancellationToken cancellationToken = default);

    Task PairAsync(string devicePath, CancellationToken cancellationToken = default);

    Task SetTrustedAsync(string devicePath, bool trusted, CancellationToken cancellationToken = default);

    /// <summary><c>Adapter1.RemoveDevice</c>: deletes the bond and the device object.</summary>
    Task RemoveDeviceAsync(string adapterPath, string devicePath, CancellationToken cancellationToken = default);
}

internal sealed record BluezAdapter(
    string Path,
    string? Address,
    bool Powered,
    IReadOnlyList<string>? Roles);

internal sealed record BluezDevice(
    string Path,
    ulong Address,
    string? Name,
    bool Paired,
    bool Connected,
    bool ServicesResolved);

internal sealed record BluezAdvertisement(
    string DevicePath,
    ulong Address,
    string? Name,
    short? Rssi);

internal sealed record BluezGattService(
    string Path,
    IReadOnlyDictionary<string, string> CharacteristicsByUuid)
{
    public string? Characteristic(string uuid) =>
        CharacteristicsByUuid.TryGetValue(uuid.ToLowerInvariant(), out var path) ? path : null;
}

/// <summary>A BlueZ or D-Bus failure, by its D-Bus error name.</summary>
internal sealed class BluezException(string errorName, string message, Exception? innerException = null)
    : Exception(message, innerException)
{
    public string ErrorName { get; } = errorName;

    public override string ToString() => $"{ErrorName}: {Message}";
}
