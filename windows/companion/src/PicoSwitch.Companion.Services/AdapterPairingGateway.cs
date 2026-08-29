using PicoSwitch.Companion.Windows.Bluetooth;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The three Windows pairing operations the connection service performs, behind a
/// seam.
///
/// ## Why this exists
///
/// Until 2026-08-29 the service called <see cref="WindowsAdapterPairing"/>'s
/// statics directly, and the repair path could therefore only be tested by
/// accident. The one repair test that existed passed because the code under test
/// did nothing: <c>AdapterRecord.DeviceId</c> was never populated, so Repair took
/// its "no device path" branch, logged a warning, cleared the repair flag and
/// returned — and the assertions about the row surviving all held, because the
/// row had survived a no-op.
///
/// A hardware run cost a flash cycle to discover that. The seam is what makes the
/// corrected behaviour — resolve the pairing object from the address, unpair it
/// once, verify Windows agrees, and only then clear the flag — assertable without
/// a radio.
///
/// It is deliberately three methods wide. This is not an abstraction over
/// Bluetooth; it is exactly the surface one class needs, and widening it would
/// invite the transport to start pairing things.
/// </summary>
public interface IAdapterPairingGateway
{
    /// <summary>What Windows currently believes about this adapter's pairing.</summary>
    Task<WindowsPairingSnapshot> ReadAsync(ulong bluetoothAddress, CancellationToken cancellationToken = default);

    /// <summary>Run the Just Works ceremony. Windows shows its own consent prompt.</summary>
    Task<AdapterPairingResult> PairAsync(ulong bluetoothAddress, CancellationToken cancellationToken = default);

    /// <summary>
    /// Destroy the Windows-side trust for one adapter.
    ///
    /// Only ever reached from an explicit user confirmation. Nothing in the
    /// service calls this on an ordinary failure.
    /// </summary>
    Task<AdapterUnpairResult> UnpairAsync(ulong bluetoothAddress, CancellationToken cancellationToken = default);
}

/// <summary>The real one. No logic of its own — every rule lives in the static it calls.</summary>
public sealed class WindowsPairingGateway : IAdapterPairingGateway
{
    public Task<WindowsPairingSnapshot> ReadAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default) =>
        WindowsAdapterPairing.ReadByAddressAsync(bluetoothAddress, cancellationToken);

    public Task<AdapterPairingResult> PairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default) =>
        WindowsAdapterPairing.PairAsync(bluetoothAddress, cancellationToken);

    public Task<AdapterUnpairResult> UnpairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default) =>
        WindowsAdapterPairing.UnpairByAddressAsync(bluetoothAddress, cancellationToken);
}
