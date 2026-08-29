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
/// - **Never auto-repair.** <see cref="UnpairAsync"/> destroys a trust
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
    public static async Task<WindowsPairingSnapshot> ReadAsync(string deviceId)
    {
        try
        {
            var information = await DeviceInformation.CreateFromIdAsync(deviceId);
            return new WindowsPairingSnapshot(
                information.Pairing.IsPaired
                    ? WindowsPairingKnown.Paired
                    : WindowsPairingKnown.NotPaired,
                information.Pairing.CanPair,
                information.Name);
        }
        catch (Exception)
        {
            // An unreadable pairing state is reported as Unknown rather than
            // guessed. Guessing "not paired" would offer a pairing flow for a
            // working adapter; guessing "paired" would let the bond-mismatch
            // signature fire on a device that was never paired at all.
            return new WindowsPairingSnapshot(WindowsPairingKnown.Unknown, false, null);
        }
    }

    /// <summary>
    /// Read the pairing state for a peer that has only just been discovered.
    ///
    /// A fresh advertisement carries an address and no device path, so the device
    /// has to be opened to reach <c>DeviceInformation.Pairing</c>. Unknown on
    /// failure rather than a guess in either direction: guessing "not paired"
    /// would start a pairing ceremony for a working adapter, and guessing "paired"
    /// would let the bond-mismatch signature fire on a device that was never
    /// paired at all.
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
    /// Drop the Windows pairing for one adapter.
    ///
    /// **This is the one place Windows is better than Android.** The Kotlin
    /// backend cannot do this at all — <c>BluetoothDevice.removeBond()</c> is a
    /// privileged <c>@SystemApi</c> — so its repair message has to send the user
    /// to Bluetooth settings. Here the app performs the unpair itself, which is
    /// what makes <see cref="AdapterResetSignature.RepairMessage"/> an action
    /// rather than an instruction.
    ///
    /// Scoped to one adapter. The caller retains that row's alias, peer history
    /// and selected identity; only the Windows-side trust is replaced.
    /// </summary>
    public static async Task<bool> UnpairAsync(
        string deviceId,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var information = await DeviceInformation.CreateFromIdAsync(deviceId)
                .AsTask(cancellationToken).ConfigureAwait(false);
            var result = await information.Pairing.UnpairAsync().AsTask(cancellationToken)
                .ConfigureAwait(false);
            return result.Status is DeviceUnpairingResultStatus.Unpaired or
                DeviceUnpairingResultStatus.AlreadyUnpaired;
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
