using PicoSwitch.Management;

namespace PicoSwitch.Companion.Windows.Bluetooth;

/// <summary>
/// Which layer produced a failure.
///
/// Ported verbatim from the Kotlin backend because the reason it exists is
/// platform-independent: a status value means nothing without knowing which
/// namespace it came from, and reading a link-layer code as an attribute-layer
/// code produces a confident, wrong diagnosis.
/// </summary>
public enum GattFailureStage
{
    Connect,
    Services,
    Subscribe,
    Command,
}

public static class GattFailureStages
{
    public static string DiagnosticName(this GattFailureStage value) => value switch
    {
        GattFailureStage.Connect => "connect",
        GattFailureStage.Services => "services",
        GattFailureStage.Subscribe => "subscribe",
        _ => "command",
    };
}

/// <summary>
/// A transport failure, tagged with the layer that produced it.
///
/// Windows reports failure through three disjoint channels, and the whole point
/// of carrying them separately is that they are NOT interchangeable:
///
/// - <c>GattCommunicationStatus</c> — the WinRT operation result
///   (<c>Unreachable</c>, <c>ProtocolError</c>, <c>AccessDenied</c>);
/// - <c>GattProtocolError</c> — the actual ATT error byte, present only when the
///   status is <c>ProtocolError</c>;
/// - an <c>HRESULT</c> on a thrown exception.
///
/// Android's single integer status has no Windows equivalent, so nothing here
/// pretends to one. <see cref="Describe"/> renders whichever of the three is
/// present, naming its namespace.
/// </summary>
public sealed class GattTransportException(
    string message,
    GattFailureStage stage,
    GattCommunicationOutcome? outcome = null,
    byte? protocolError = null,
    int? hresult = null,
    Exception? innerException = null)
    : ManagementException(message, innerException)
{
    public GattFailureStage Stage { get; } = stage;

    public GattCommunicationOutcome? Outcome { get; } = outcome;

    /// <summary>The ATT error byte. Meaningful only alongside <c>ProtocolError</c>.</summary>
    public byte? ProtocolError { get; } = protocolError;

    public int? Hresult { get; } = hresult;

    public string Describe() =>
        GattStatusFormatter.Describe(Stage, Outcome, ProtocolError, Hresult);
}

/// <summary>
/// <c>GattCommunicationStatus</c>, mirrored so the pure policy layer does not need
/// the WinRT projection.
///
/// The Windows transport maps the real enum onto this at its boundary. Keeping
/// the policy testable without a radio is worth one small mapping; every value
/// here exists in the WinRT enum and the names match deliberately.
/// </summary>
public enum GattCommunicationOutcome
{
    Success,
    Unreachable,
    ProtocolError,
    AccessDenied,
}

/// <summary>
/// Renders a failure with the namespace its status came from.
///
/// The Kotlin <c>GattStatusFormatter</c> exists specifically to stop an HCI code
/// being read as an ATT code. Windows hides HCI entirely, so the equivalent
/// discipline here is to keep <c>GattCommunicationStatus</c>, the ATT error byte
/// and the <c>HRESULT</c> distinguishable rather than flattening them into one
/// number.
/// </summary>
public static class GattStatusFormatter
{
    /// <summary>
    /// Well-known <c>HRESULT</c>s the Bluetooth stack surfaces.
    ///
    /// <c>E_BLUETOOTH_ATT_INSUFFICIENT_AUTHENTICATION</c> and <c>E_ACCESSDENIED</c>
    /// are the two that carry diagnostic weight, because together with a held
    /// pairing they are the Windows shape of "the peer has no key for us".
    /// </summary>
    public const int EAccessDenied = unchecked((int)0x80070005);

    public const int EBluetoothAttInsufficientAuthentication = unchecked((int)0x80650005);

    public const int EBluetoothAttInsufficientEncryption = unchecked((int)0x8065000F);

    public const int EDeviceNotAvailable = unchecked((int)0x800710DF);

    public const int RpcSServerUnavailable = unchecked((int)0x800706BA);

    public static string Describe(
        GattFailureStage stage,
        GattCommunicationOutcome? outcome,
        byte? protocolError,
        int? hresult)
    {
        var parts = new List<string> { $"stage={stage.DiagnosticName()}" };
        if (outcome is { } status)
        {
            parts.Add($"GattCommunicationStatus={status}");
        }

        if (protocolError is { } att)
        {
            parts.Add($"ATT=0x{att:X2} {AttName(att)}");
        }

        if (hresult is { } code)
        {
            parts.Add($"HRESULT=0x{code:X8} {HresultName(code)}");
        }

        return string.Join(' ', parts);
    }

    private static string AttName(byte error) => error switch
    {
        0x01 => "INVALID_HANDLE",
        0x02 => "READ_NOT_PERMITTED",
        0x03 => "WRITE_NOT_PERMITTED",
        0x05 => "INSUFFICIENT_AUTHENTICATION",
        0x08 => "INSUFFICIENT_AUTHORIZATION",
        0x0D => "INVALID_ATTRIBUTE_VALUE_LENGTH",
        0x0F => "INSUFFICIENT_ENCRYPTION",
        0x13 => "VALUE_NOT_ALLOWED",
        _ => "UNKNOWN",
    };

    private static string HresultName(int hresult) => hresult switch
    {
        EAccessDenied => "E_ACCESSDENIED",
        EBluetoothAttInsufficientAuthentication => "E_BLUETOOTH_ATT_INSUFFICIENT_AUTHENTICATION",
        EBluetoothAttInsufficientEncryption => "E_BLUETOOTH_ATT_INSUFFICIENT_ENCRYPTION",
        EDeviceNotAvailable => "E_DEVICE_NOT_AVAILABLE",
        RpcSServerUnavailable => "RPC_S_SERVER_UNAVAILABLE",
        _ => "UNKNOWN",
    };
}

/// <summary>Small, bounded recovery policy. Ported shape, Windows conditions.</summary>
public static class GattRecoveryPolicy
{
    public const int MaxCleanRetries = 1;
    public const int RetryBackoffMillis = 350;

    /// <summary>
    /// Worth one clean retry?
    ///
    /// Only transient connect-stage conditions qualify. <c>AccessDenied</c> and an
    /// authentication <c>HRESULT</c> deliberately do NOT: they are the bond-mismatch
    /// shape, and retrying them is what produced six futile attempts over fourteen
    /// minutes on the Android side.
    /// </summary>
    public static bool IsRetryable(Exception error)
    {
        if (FirstTransportFailure(error) is not { } failure)
        {
            return false;
        }

        if (failure.Stage != GattFailureStage.Connect)
        {
            return false;
        }

        if (failure.Outcome == GattCommunicationOutcome.Unreachable)
        {
            return true;
        }

        return failure.Hresult is
            GattStatusFormatter.EDeviceNotAvailable or
            GattStatusFormatter.RpcSServerUnavailable;
    }

    public static bool ShouldRetry(Exception error, int retriesUsed) =>
        retriesUsed < MaxCleanRetries && IsRetryable(error);

    /// <summary>
    /// The first tagged transport failure anywhere in the exception, including
    /// across an <see cref="AggregateException"/>'s branches.
    ///
    /// The branches matter: the recovery ladder reports the direct failure and the
    /// fallback failure together, and the bond-mismatch evidence is in the DIRECT
    /// one. Following only <c>InnerException</c> would inspect whichever branch
    /// happened to be first and could miss it entirely.
    /// </summary>
    internal static GattTransportException? FirstTransportFailure(Exception? error)
    {
        if (error is null)
        {
            return null;
        }

        if (error is GattTransportException failure)
        {
            return failure;
        }

        if (error is AggregateException aggregate)
        {
            foreach (var branch in aggregate.InnerExceptions)
            {
                if (FirstTransportFailure(branch) is { } found)
                {
                    return found;
                }
            }

            return null;
        }

        return FirstTransportFailure(error.InnerException);
    }
}

/// <summary>
/// Did the adapter forget us?
///
/// The adapter wipes its Bluetooth bonds on every firmware install, deliberately,
/// so a stale controller cannot reconnect after a flash and make a test result
/// meaningless. That is a lifecycle event, not a fault — but it leaves a
/// three-party relationship with only one party cleared: the host keeps its LTK
/// and the app keeps its saved relationship.
///
/// ## The Windows detection is NOT the Android one
///
/// Android surfaces the HCI disconnect reason to the app, so the Kotlin side
/// matches on status 0x05 (`AUTHENTICATION_FAILURE`) or 0x06
/// (`PIN_OR_KEY_MISSING`) at the connect stage. **Windows does not expose HCI
/// status codes to user mode at all.** What is available is
/// <c>GattCommunicationStatus</c> plus the <c>HRESULT</c> on a thrown exception,
/// so the signature has to be reconstructed from three facts held together:
///
/// 1. Windows reports the peer as **paired**;
/// 2. the device was **reachable** — an advertisement was seen inside the attempt
///    window, so this is not simply an absent adapter;
/// 3. an operation against an **encrypted attribute** returned <c>AccessDenied</c>
///    or threw <c>E_ACCESSDENIED</c> /
///    <c>E_BLUETOOTH_ATT_INSUFFICIENT_AUTHENTICATION</c>.
///
/// All three are load-bearing. Without (1) this is simply "not paired", which is a
/// different message and a different flow. Without (2) an out-of-range adapter
/// would be misdiagnosed as reflashed. Without (3) any ordinary connect failure
/// would trip it.
///
/// **Confidence: Hypothesis.** Reasoned from the Windows API surface and from the
/// hardware-confirmed Android behaviour, and NOT yet observed on Windows against a
/// reflashed adapter. WINDOWS_PASS.md §12.2 and §31 Phase 2 name the exit
/// criterion: first-attempt <c>RepairRequired</c> against a genuinely reflashed
/// adapter. Until that runs, treat the specific condition set below as the thing
/// under test.
/// </summary>
public static class AdapterResetSignature
{
    public static bool IsBondMismatch(
        GattFailureStage stage,
        GattCommunicationOutcome? outcome,
        int? hresult,
        bool windowsStillPaired,
        bool peerReachable)
    {
        if (!windowsStillPaired || !peerReachable)
        {
            return false;
        }

        // Any stage qualifies EXCEPT a pure discovery failure: the refusal can
        // surface when services are resolved, when the CCC is written, or on the
        // first command, depending on where encryption is first required.
        if (stage == GattFailureStage.Connect && outcome is null && hresult is null)
        {
            return false;
        }

        if (outcome == GattCommunicationOutcome.AccessDenied)
        {
            return true;
        }

        return hresult is
            GattStatusFormatter.EAccessDenied or
            GattStatusFormatter.EBluetoothAttInsufficientAuthentication or
            GattStatusFormatter.EBluetoothAttInsufficientEncryption;
    }

    /// <summary>As above, for a thrown transport failure.</summary>
    public static bool IsBondMismatch(Exception error, bool windowsStillPaired, bool peerReachable) =>
        GattRecoveryPolicy.FirstTransportFailure(error) is { } failure &&
        IsBondMismatch(failure.Stage, failure.Outcome, failure.Hresult, windowsStillPaired, peerReachable);

    /// <summary>
    /// What the user has to do, and why.
    ///
    /// **This is the one place Windows is better than Android.** The Kotlin message
    /// has to tell the user to go to Bluetooth settings, because
    /// <c>BluetoothDevice.removeBond()</c> is a privileged <c>@SystemApi</c>. On
    /// Windows the app can call <c>DeviceInformationPairing.UnpairAsync()</c>
    /// itself, so the message names an action the app will actually perform rather
    /// than instructing the user to leave.
    /// </summary>
    public const string RepairMessage =
        "The adapter was reset and no longer recognises this pairing. " +
        "Repair pairing to continue.";
}

/// <summary>Pure authority check used before any WinRT callback may mutate session state.</summary>
public static class GattCallbackAuthority
{
    public static bool IsAuthoritative(
        long? currentGeneration,
        long callbackGeneration,
        bool callbackOwnerClosed) =>
        currentGeneration == callbackGeneration && !callbackOwnerClosed;
}
