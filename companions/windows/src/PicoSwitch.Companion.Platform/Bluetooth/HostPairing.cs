namespace PicoSwitch.Companion.Platform.Bluetooth;

// The pairing vocabulary every platform backend reports in. Extracted from the
// Windows backend unchanged on 2026-11-06 so the Linux backend can share it.

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
    // The platform's own status name, for the diagnostic trail only. A string so
    // the vocabulary stays free of WinRT and BlueZ types alike.
    string Status,
    string Message)
{
    public bool Succeeded =>
        Outcome is AdapterPairingOutcome.Paired or AdapterPairingOutcome.AlreadyPaired;
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
            "This computer could not reach that adapter to remove its pairing. " +
            "Check that Bluetooth is on and try again.",
        AdapterUnpairResult.AccessDenied =>
            "This computer refused to remove the pairing for this adapter.",
        _ => "This computer could not remove the pairing for this adapter.",
    };
}

public enum HostPairingKnown
{
    Unknown,
    NotPaired,
    Paired,
}

public readonly record struct HostPairingSnapshot(
    HostPairingKnown State,
    bool CanPair,
    string? DeviceName);
