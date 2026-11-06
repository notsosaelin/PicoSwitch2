using PicoSwitch.Management;
using Windows.Devices.Bluetooth;
using Windows.Devices.Enumeration;

namespace PicoSwitch.Companion.Windows.Bluetooth;

/// <summary>What a pairing attempt did, in the app's own vocabulary.</summary>
public enum AdapterPairingOutcome
{
    Paired,

    /// <summary>Already paired before the attempt; nothing was done.</summary>
    AlreadyPaired,

    /// <summary>The user or the peer declined. Retryable.</summary>
    Rejected,

    /// <summary>The adapter never answered. Usually its pairing window was not open.</summary>
    TimedOut,

    Failed,
}

public sealed record AdapterPairingResult(
    AdapterPairingOutcome Outcome,
    DevicePairingResultStatus Status,
    string Message)
{
    public bool Succeeded =>
        Outcome is AdapterPairingOutcome.Paired or AdapterPairingOutcome.AlreadyPaired;
}

/// <summary>
/// The Windows pairing ceremony, and the one place the app is allowed to unpair.
///
/// ## Security rules that are not negotiable
///
/// - **Never pair with <c>DevicePairingProtectionLevel.None</c>.** Management
///   requires a bond and a 16-byte encryption key; a pairing that cannot satisfy
///   that is a failure to REPORT, not a fallback to take. The protection level is
///   passed as <c>Encryption</c> and there is deliberately no downgrade path.
/// - **Never present this as authenticated.** It is Just Works: bonded and
///   encrypted, WITHOUT MITM protection. Every string here says "bonded" or
///   "encrypted" and none says "authenticated" or "secure", and that is a copy
///   rule, not a style preference.
/// - **Never auto-repair.** <see cref="UnpairByAddressAsync"/> destroys a trust
///   relationship and is only ever called behind an explicit user confirmation.
///
/// ## Why <c>ConfirmOnly</c>
///
/// The adapter has no display and no keypad, so Just Works is the only ceremony
/// it can complete. Windows shows its own consent dialog for it; the app must not
/// imitate or duplicate that dialog, because a second consent prompt teaches
/// people to click through the real one.
/// </summary>
public static class WindowsAdapterPairing
{
    /// <summary>
    /// Read Windows' pairing state for one adapter, by address.
    ///
    /// The address is the only identifier the app persists, and it is sufficient:
    /// <c>BluetoothLEDevice.FromBluetoothAddressAsync</c> resolves the device --
    /// and with it <c>DeviceInformation.Pairing</c> -- with no live session and no
    /// cached path. A device-path overload used to exist beside this one and was
    /// unreachable, because nothing ever cached a path to pass it.
    ///
    /// Unknown on failure rather than a guess in either direction: guessing "not
    /// paired" would start a pairing ceremony for a working adapter, and guessing
    /// "paired" would let the bond-mismatch signature fire on a device that was
    /// never paired at all.
    /// </summary>
    public static async Task<WindowsPairingSnapshot> ReadByAddressAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        try
        {
            using var device = await BluetoothLEDevice
                .FromBluetoothAddressAsync(bluetoothAddress)
                .AsTask(cancellationToken).ConfigureAwait(false);
            if (device is null)
            {
                return new WindowsPairingSnapshot(WindowsPairingKnown.Unknown, false, null);
            }

            var pairing = device.DeviceInformation.Pairing;
            return new WindowsPairingSnapshot(
                pairing.IsPaired ? WindowsPairingKnown.Paired : WindowsPairingKnown.NotPaired,
                pairing.CanPair,
                device.Name);
        }
        catch (Exception)
        {
            return new WindowsPairingSnapshot(WindowsPairingKnown.Unknown, false, null);
        }
    }

    public static async Task<AdapterPairingResult> PairAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        using var device = await BluetoothLEDevice.FromBluetoothAddressAsync(bluetoothAddress)
            .AsTask(cancellationToken).ConfigureAwait(false);
        if (device is null)
        {
            return new AdapterPairingResult(
                AdapterPairingOutcome.Failed,
                DevicePairingResultStatus.Failed,
                "Windows could not open that adapter to pair with it.");
        }

        var pairing = device.DeviceInformation.Pairing;
        if (pairing.IsPaired)
        {
            return new AdapterPairingResult(
                AdapterPairingOutcome.AlreadyPaired,
                DevicePairingResultStatus.AlreadyPaired,
                "This PC is already paired with the adapter.");
        }

        var custom = pairing.Custom;

        // ConfirmOnly is the only ceremony a display-less, keypad-less peripheral
        // can complete. Accepting here is accepting the app's own request; Windows
        // still shows the user its own consent UI.
        void OnPairingRequested(
            DeviceInformationCustomPairing sender,
            DevicePairingRequestedEventArgs args) => args.Accept();

        custom.PairingRequested += OnPairingRequested;
        try
        {
            var result = await custom
                .PairAsync(DevicePairingKinds.ConfirmOnly, DevicePairingProtectionLevel.Encryption)
                .AsTask(cancellationToken).ConfigureAwait(false);
            return Interpret(result.Status);
        }
        finally
        {
            custom.PairingRequested -= OnPairingRequested;
        }
    }

    /// <summary>
    /// Drop the Windows pairing for one adapter, resolved from its ADDRESS.
    ///
    /// **This is the one place Windows is better than Android.** The Kotlin
    /// backend cannot do this at all — <c>BluetoothDevice.removeBond()</c> is a
    /// privileged <c>@SystemApi</c> — so its repair message has to send the user
    /// to Bluetooth settings. Here the app performs the unpair itself, which is
    /// what makes <see cref="AdapterResetSignature.RepairMessage"/> an action
    /// rather than an instruction.
    ///
    /// ## Why the address, and not a saved device path
    ///
    /// It used to take a WinRT device path that the app cached at pairing time.
    /// It never did: nothing in the codebase ever populated that field, so Repair
    /// always took its "no device path" branch, logged a warning, cleared the
    /// repair flag and returned — reporting success while leaving the stale
    /// Windows bond exactly where it was. Observed on hardware 2026-08-29.
    ///
    /// The address is the identifier the app actually persists, and Windows will
    /// re-resolve the paired device from it at any time, with no live management
    /// session and no cached state. That is what the transport already does on
    /// every connect, which is why the same run could report <c>paired=True</c>
    /// four times while Repair claimed it could not find the device.
    ///
    /// Scoped to one adapter. The caller retains that row's alias, peer history
    /// and selected identity; only the Windows-side trust is replaced.
    /// </summary>
    public static async Task<AdapterUnpairResult> UnpairByAddressAsync(
        ulong bluetoothAddress,
        CancellationToken cancellationToken = default)
    {
        try
        {
            using var device = await BluetoothLEDevice.FromBluetoothAddressAsync(bluetoothAddress)
                .AsTask(cancellationToken).ConfigureAwait(false);
            if (device is null)
            {
                return AdapterUnpairResult.Unresolved;
            }

            var pairing = device.DeviceInformation.Pairing;
            if (!pairing.IsPaired)
            {
                // Idempotent: there is nothing to remove, and the caller's goal --
                // no stale Windows trust for this adapter -- already holds.
                return AdapterUnpairResult.AlreadyUnpaired;
            }

            var result = await pairing.UnpairAsync().AsTask(cancellationToken).ConfigureAwait(false);
            return result.Status switch
            {
                DeviceUnpairingResultStatus.Unpaired => AdapterUnpairResult.Unpaired,
                DeviceUnpairingResultStatus.AlreadyUnpaired => AdapterUnpairResult.AlreadyUnpaired,
                DeviceUnpairingResultStatus.AccessDenied => AdapterUnpairResult.AccessDenied,
                _ => AdapterUnpairResult.Failed,
            };
        }
        catch (Exception error)
        {
            throw new ManagementException(
                "Windows could not remove the pairing for this adapter.",
                error);
        }
    }

    private static AdapterPairingResult Interpret(DevicePairingResultStatus status) => status switch
    {
        DevicePairingResultStatus.Paired => new AdapterPairingResult(
            AdapterPairingOutcome.Paired,
            status,
            "Paired. The link is bonded and encrypted."),

        DevicePairingResultStatus.AlreadyPaired => new AdapterPairingResult(
            AdapterPairingOutcome.AlreadyPaired,
            status,
            "This PC is already paired with the adapter."),

        DevicePairingResultStatus.RejectedByHandler or
        DevicePairingResultStatus.PairingCanceled => new AdapterPairingResult(
            AdapterPairingOutcome.Rejected,
            status,
            "The pairing request was declined."),

        DevicePairingResultStatus.AuthenticationTimeout or
        DevicePairingResultStatus.ConnectionRejected or
        DevicePairingResultStatus.Failed => new AdapterPairingResult(
            AdapterPairingOutcome.TimedOut,
            status,

            // The adapter admits a new management-client bond ONLY while its
            // physical double-tap window is open. Naming that turns a mysterious
            // timeout into an instruction the user can act on.
            "The adapter did not complete pairing. Double-tap its pairing button and try again."),

        _ => new AdapterPairingResult(
            AdapterPairingOutcome.Failed,
            status,
            $"Windows could not pair with the adapter ({status})."),
    };
}

/// <summary>
/// What an unpair actually did.
///
/// A bool cannot carry this. "Windows could not resolve the device" and "Windows
/// removed the pairing" both used to return false and true respectively, with no
/// way to distinguish either from "there was nothing paired" — and Repair has to
/// tell them apart, because only a genuine removal may clear the repair flag.
/// </summary>
public enum AdapterUnpairResult
{
    /// <summary>The pairing existed and was removed.</summary>
    Unpaired,

    /// <summary>Windows held no pairing for this adapter. Idempotent success.</summary>
    AlreadyUnpaired,

    /// <summary>Windows could not open the device at all — usually radio-off.</summary>
    Unresolved,

    AccessDenied,

    Failed,
}

public static class AdapterUnpairResults
{
    /// <summary>
    /// Whether Windows now holds no pairing for the adapter.
    ///
    /// The ONLY condition under which Repair may clear the repair flag: reporting
    /// a repair that did not happen is what the 2026-08-29 run actually did.
    /// </summary>
    public static bool TrustRemoved(this AdapterUnpairResult value) =>
        value is AdapterUnpairResult.Unpaired or AdapterUnpairResult.AlreadyUnpaired;

    public static string DiagnosticName(this AdapterUnpairResult value) => value switch
    {
        AdapterUnpairResult.Unpaired => "removed",
        AdapterUnpairResult.AlreadyUnpaired => "already-unpaired",
        AdapterUnpairResult.Unresolved => "unresolved",
        AdapterUnpairResult.AccessDenied => "access-denied",
        _ => "failed",
    };

    public static string Message(this AdapterUnpairResult value) => value switch
    {
        AdapterUnpairResult.Unpaired or AdapterUnpairResult.AlreadyUnpaired =>
            "Pairing removed. Pair the adapter again.",
        AdapterUnpairResult.Unresolved =>
            "Windows could not reach that adapter to remove its pairing. " +
            "Check that Bluetooth is on and try again.",
        AdapterUnpairResult.AccessDenied =>
            "Windows refused to remove the pairing for this adapter.",
        _ => "Windows could not remove the pairing for this adapter.",
    };
}

public enum WindowsPairingKnown
{
    Unknown,
    NotPaired,
    Paired,
}

public readonly record struct WindowsPairingSnapshot(
    WindowsPairingKnown State,
    bool CanPair,
    string? DeviceName);
