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
/// ## What Windows actually does — measured, 2026-08-29
///
/// The first version of this class was reasoned from the API surface and was
/// WRONG. It expected the refusal to surface at the attribute layer, as
/// <c>AccessDenied</c> or an authentication <c>HRESULT</c>. Against a genuinely
/// reflashed adapter, Windows produced neither. What it produced, repeatedly,
/// was:
///
/// <code>
/// open device resolved paired=True
/// fail stage=services GattCommunicationStatus=Unreachable
/// </code>
///
/// with no ATT byte and no <c>HRESULT</c> at all — the status was RETURNED by
/// <c>GetGattServicesForUuidAsync</c>, not thrown, so there was never an
/// exception to carry one.
///
/// The mechanism explains the shape. Windows holds an LTK for a bonded peer and
/// encrypts the link on connection, before any ATT transaction exists. The
/// reflashed adapter has no matching key, SMP fails and the link drops. The
/// failure is therefore BELOW the attribute layer, and WinRT's word for "the
/// operation could not complete over the air" is <c>Unreachable</c>. There is no
/// attribute-layer error because no attribute was ever reached.
///
/// The adapter's own firmware corroborates this by elimination: `mgmt_access.h`
/// gates ADVERTISING and CONNECTION on neither bonding nor the pairing window,
/// and gates only command WRITES on a bonded, encrypted client. No firmware rule
/// refuses service discovery, so the refusal cannot come from the application
/// layer.
///
/// **Confidence: Strong Evidence.** The observable behaviour is Confirmed —
/// reproduced across four connect attempts and cleared instantly by an unpair and
/// re-pair with no firmware change. The SMP-level mechanism is inference, because
/// Windows exposes no HCI or SMP detail to user mode, and nothing here depends on
/// the inference being right.
///
/// ## Why the signature must be compound
///
/// <c>Unreachable</c> is ALSO what a powered-off adapter produces. It is not, on
/// its own, evidence of anything. Two shapes are recognised, and each carries its
/// own corroboration:
///
/// **Shape 1, the attribute-layer refusal.** Windows still reports the peer as
/// paired, the peer answered SOMETHING at the attribute layer, and that answer
/// was <c>AccessDenied</c> or an authentication/encryption <c>HRESULT</c>. This is
/// conclusive on its own and is retained even though this hardware did not
/// produce it: a different radio, driver or Windows build may, and it costs
/// nothing to keep.
///
/// **Shape 2, the link-layer refusal — what this hardware does.** All four facts
/// are required:
///
/// 1. Windows still reports the peer as **paired**;
/// 2. the **exact remembered address** was seen advertising by the
///    service-UUID-restricted watcher inside this attempt — a powered-off,
///    absent or out-of-range adapter can never satisfy this;
/// 3. the failure was <c>Unreachable</c> at a stage AFTER the device resolved, so
///    Windows opened the device and then could not reach its GATT server;
/// 4. that happened at least **twice** on separately resolved device objects —
///    the direct connect and the fresh scan-resolved connect — so a single
///    transient link failure cannot trip it.
///
/// Fact 4 is why the recovery ladder deliberately does NOT short-circuit on this
/// shape: the fallback scan is what PRODUCES the corroboration. Shape 1 still
/// ends the ladder immediately, because it is conclusive at the first failure.
/// </summary>
public static class AdapterResetSignature
{
    /// <summary>
    /// How many independently resolved device objects must fail the same way
    /// before a link-layer refusal counts as evidence rather than noise.
    ///
    /// Two, which is exactly what one logical attempt can produce: the direct
    /// connect and the address-restricted fallback. Raising this would make the
    /// signature unreachable; lowering it to one would let a single momentary link
    /// failure against a present adapter offer to destroy a working pairing.
    /// </summary>
    public const int CorroboratingLinkFailures = 2;

    public static bool IsBondMismatch(
        GattFailureStage stage,
        GattCommunicationOutcome? outcome,
        int? hresult,
        TransportTrustSnapshot trust)
    {
        if (!trust.WindowsPaired)
        {
            // Without a held pairing this is simply "not paired": a different
            // message, a different flow, and nothing to repair.
            return false;
        }

        // Shape 1 -- the attribute layer refused us. Conclusive on its own,
        // provided the peer was actually there to refuse.
        if (trust.PeerObserved || trust.PeerAnsweredGatt)
        {
            if (outcome == GattCommunicationOutcome.AccessDenied)
            {
                return true;
            }

            if (hresult is
                GattStatusFormatter.EAccessDenied or
                GattStatusFormatter.EBluetoothAttInsufficientAuthentication or
                GattStatusFormatter.EBluetoothAttInsufficientEncryption)
            {
                return true;
            }
        }

        // Shape 2 -- the link layer refused us, which is all Windows reports.
        // Every clause is load-bearing; see the class comment.
        return trust.PeerObserved &&
            outcome == GattCommunicationOutcome.Unreachable &&
            stage != GattFailureStage.Connect &&
            trust.LinkFailuresAfterResolve >= CorroboratingLinkFailures;
    }

    /// <summary>As above, for a thrown transport failure.</summary>
    public static bool IsBondMismatch(Exception error, TransportTrustSnapshot trust) =>
        GattRecoveryPolicy.FirstTransportFailure(error) is { } failure &&
        IsBondMismatch(failure.Stage, failure.Outcome, failure.Hresult, trust);

    /// <summary>
    /// Render the decision with every predicate that fed it.
    ///
    /// The 2026-08-29 hardware run cost a flash cycle and produced a log that said
    /// only <c>paired=True peerAnswered=False</c>, which was not enough to tell
    /// WHICH clause rejected the classification. This exists so the next run does
    /// not have that problem: it names each fact and the verdict together.
    /// </summary>
    public static string Explain(Exception error, TransportTrustSnapshot trust)
    {
        var failure = GattRecoveryPolicy.FirstTransportFailure(error);
        var verdict = failure is null
            ? "no tagged transport failure"
            : IsBondMismatch(failure.Stage, failure.Outcome, failure.Hresult, trust)
                ? "BOND MISMATCH"
                : "not a bond mismatch";

        return $"paired={trust.WindowsPaired} observed={trust.PeerObserved} " +
            $"answeredGatt={trust.PeerAnsweredGatt} " +
            $"linkFailures={trust.LinkFailuresAfterResolve}/{CorroboratingLinkFailures} " +
            $"-> {verdict}";
    }

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

    /// <summary>
    /// The same condition reached through the pairing flow, where there is no
    /// remembered row to repair.
    ///
    /// Observed 2026-08-29: after Remove, pressing Pair found the adapter, Windows
    /// reported the stale bond as paired, the ceremony was skipped entirely and the
    /// connect failed the same way — with no route out, because Repair belongs to a
    /// row that no longer existed. The message names the actual action instead.
    /// </summary>
    public const string StalePairingMessage =
        "Windows still holds an old pairing for this adapter, which the adapter no " +
        "longer recognises. Add the adapter again and use Repair pairing to replace it.";
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
