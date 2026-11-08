using PicoSwitch.Companion.Platform.Bluetooth;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// BlueZ failure classification, in the vocabulary the shared recovery policy
/// (<see cref="GattRecoveryPolicy"/>, <see cref="AdapterResetSignature"/>) weighs.
///
/// ## Why this mapping matters
///
/// <see cref="AdapterResetSignature.IsBondMismatch(GattFailureStage, GattCommunicationOutcome?, int?, TransportTrustSnapshot)"/>
/// offers the user a destructive Repair when a paired, observed peer fails with
/// <see cref="GattCommunicationOutcome.AccessDenied"/> before the bond was
/// proven. So <c>AccessDenied</c> here must mean exactly what it means on
/// Windows: a SECURITY refusal at the attribute layer (insufficient
/// authentication, encryption, authorization or key size). Nothing else may map
/// to it.
///
/// Windows reports the outcome directly as <c>GattCommunicationStatus</c>. BlueZ
/// reports a D-Bus error name, and relays most ATT errors as text through
/// <c>org.bluez.Error.Failed</c> ("Operation failed with ATT error: 0x05").
///
/// Status: the ATT-code mapping is a **Hypothesis** (BlueZ's documented set). The
/// stale-bond case at CONNECT is **Confirmed** on hardware (Bazzite, BlueZ 5.87,
/// 2026-11-06): no ATT error at all, just <c>Failed: le-connection-abort-by-local</c>
/// followed by a <c>Device1.Disconnected</c> reason of
/// <see cref="AuthenticationReason"/>. The transport classifies it from that
/// reason (BluezManagementTransport.ConnectDeviceAsync), never from this text. The default is deliberately <see cref="GattCommunicationOutcome.ProtocolError"/>:
/// it records that the peer answered and can never, on its own, produce a Repair
/// offer.
/// </summary>
internal static class BluezErrors
{
    public const string Prefix = "org.bluez.Error.";
    public const string Failed = Prefix + "Failed";
    public const string NotConnected = Prefix + "NotConnected";
    public const string InProgress = Prefix + "InProgress";
    public const string AlreadyExists = Prefix + "AlreadyExists";
    public const string DoesNotExist = Prefix + "DoesNotExist";
    public const string NotPermitted = Prefix + "NotPermitted";
    public const string NotAuthorized = Prefix + "NotAuthorized";
    public const string NotReady = Prefix + "NotReady";
    public const string NotSupported = Prefix + "NotSupported";
    public const string InvalidArguments = Prefix + "InvalidArguments";
    public const string AuthenticationFailed = Prefix + "AuthenticationFailed";
    public const string AuthenticationCanceled = Prefix + "AuthenticationCanceled";
    public const string AuthenticationRejected = Prefix + "AuthenticationRejected";
    public const string AuthenticationTimeout = Prefix + "AuthenticationTimeout";
    public const string ConnectionAttemptFailed = Prefix + "ConnectionAttemptFailed";
    /// <summary>
    /// The <c>Device1.Disconnected</c> reason for a link the peer accepted and
    /// then refused at the security layer: BlueZ's key was rejected. Captured on
    /// hardware (Bazzite, BlueZ 5.87, 2026-11-06) against an adapter that no
    /// longer held this computer's bond: Connect failed with
    /// <c>Failed: le-connection-abort-by-local</c>, then this reason arrived.
    /// </summary>
    public const string AuthenticationReason = "org.bluez.Reason.Authentication";

    public const string UnknownObject = "org.freedesktop.DBus.Error.UnknownObject";
    public const string NoReply = "org.freedesktop.DBus.Error.NoReply";
    public const string ServiceUnknown = "org.freedesktop.DBus.Error.ServiceUnknown";

    /// <summary>What one GATT-layer failure says about the peer.</summary>
    public static GattCommunicationOutcome Outcome(BluezException error)
    {
        switch (error.ErrorName)
        {
            case NotConnected:
            case ConnectionAttemptFailed:
            case UnknownObject:
            case NoReply:
                return GattCommunicationOutcome.Unreachable;

            // BlueZ maps ATT "insufficient authorization" to NotAuthorized.
            case NotAuthorized:
                return GattCommunicationOutcome.AccessDenied;

            // Read/write-not-permitted: an attribute permission, not security.
            case NotPermitted:
                return GattCommunicationOutcome.ProtocolError;
        }

        if (AttError(error) is { } code)
        {
            return IsSecurityAttError(code)
                ? GattCommunicationOutcome.AccessDenied
                : GattCommunicationOutcome.ProtocolError;
        }

        var message = error.Message ?? string.Empty;
        if (message.Contains("Not connected", StringComparison.OrdinalIgnoreCase) ||
            message.Contains("abort", StringComparison.OrdinalIgnoreCase) ||
            message.Contains("Disconnected", StringComparison.OrdinalIgnoreCase) ||
            message.Contains("Timeout", StringComparison.OrdinalIgnoreCase) ||
            message.Contains("Timed out", StringComparison.OrdinalIgnoreCase))
        {
            return GattCommunicationOutcome.Unreachable;
        }

        return GattCommunicationOutcome.ProtocolError;
    }

    /// <summary>The ATT error byte BlueZ relayed in the message text, when there is one.</summary>
    public static byte? AttError(BluezException error)
    {
        var message = error.Message ?? string.Empty;
        var at = message.IndexOf("ATT error: 0x", StringComparison.OrdinalIgnoreCase);
        if (at < 0 || at + 15 > message.Length)
        {
            return null;
        }

        return byte.TryParse(
            message.AsSpan(at + 13, 2),
            System.Globalization.NumberStyles.HexNumber,
            System.Globalization.CultureInfo.InvariantCulture,
            out var code)
            ? code
            : null;
    }

    /// <summary>
    /// ATT 0x05 insufficient authentication, 0x08 insufficient authorization,
    /// 0x0C insufficient encryption key size, 0x0F insufficient encryption
    /// (Bluetooth Core Vol 3 Part F, 3.4.1.1).
    /// </summary>
    public static bool IsSecurityAttError(byte code) => code is 0x05 or 0x08 or 0x0C or 0x0F;

    /// <summary>A pairing failure, in the app's pairing vocabulary.</summary>
    public static AdapterPairingOutcome PairingOutcome(BluezException error) => error.ErrorName switch
    {
        AlreadyExists => AdapterPairingOutcome.AlreadyPaired,
        AuthenticationCanceled or AuthenticationRejected => AdapterPairingOutcome.Rejected,

        // The adapter admits a new bond only while its double-tap pairing window
        // is open, so "never answered" is the common shape of a closed window.
        AuthenticationTimeout or ConnectionAttemptFailed or NoReply => AdapterPairingOutcome.TimedOut,
        AuthenticationFailed when error.Message.Contains("Timeout", StringComparison.OrdinalIgnoreCase) =>
            AdapterPairingOutcome.TimedOut,
        _ => AdapterPairingOutcome.Failed,
    };

    /// <summary>The short status name recorded in <see cref="AdapterPairingResult.Status"/>.</summary>
    public static string StatusName(BluezException error) =>
        error.ErrorName.StartsWith(Prefix, StringComparison.Ordinal)
            ? error.ErrorName[Prefix.Length..]
            : error.ErrorName;
}
