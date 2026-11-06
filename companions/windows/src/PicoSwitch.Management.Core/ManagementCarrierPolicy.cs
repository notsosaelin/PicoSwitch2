namespace PicoSwitch.Management;

/// <summary>
/// Timing for the adapter's one-slot wireless request/reply carrier.
/// </summary>
/// <remarks>
/// A DELIBERATE MIRROR of the Kotlin <c>ManagementTurnaroundPolicy</c>. This is a
/// property of the FIRMWARE, not of either client: the adapter bridges commands
/// between cores through a single slot, and Windows can deliver the final
/// notification to a process before the firmware's next task turn has released
/// that slot. A new write in that window is accepted by GATT and intermittently
/// produces no reply at all.
///
/// The Android client has honoured this since the behaviour was characterised.
/// The Windows client did not, which is consistent with it stalling more often
/// than Android on the same operation.
/// </remarks>
public static class ManagementTurnaroundPolicy
{
    /// <summary>Minimum gap between one reply and the next command.</summary>
    public const long MinMillis = 100L;

    /// <summary>How long to wait before sending, given when the last reply landed.</summary>
    public static long DelayMillis(long nowMillis, long lastReplyMillis)
    {
        if (lastReplyMillis <= 0L || nowMillis < lastReplyMillis)
        {
            return 0L;
        }

        return Math.Max(0L, MinMillis - (nowMillis - lastReplyMillis));
    }
}

/// <summary>
/// Which management commands may safely be sent again after no reply.
/// </summary>
/// <remarks>
/// <para>
/// A DELIBERATE MIRROR of the Kotlin <c>ManagementRetryPolicy</c>.
/// </para>
/// <para>
/// THE FIRMWARE CAN DROP A COMMAND WITHOUT ANSWERING IT, by design. Its wireless
/// bridge holds one command and one response at a time, and a complete command
/// arriving while either slot is occupied is discarded outright — the line
/// buffer is reset and BUSY returned. Both clients respond to the resulting
/// failure by tearing the session down, so a single dropped chunk ends a
/// transfer and disconnects the adapter.
/// </para>
/// <para>
/// CORRECTED 2026-08-31: the drop is NOT silent, and the correction matters. The
/// ATT write returns <c>INSUFFICIENT_RESOURCES</c>, both clients write with
/// response, and the failure therefore arrives as a GATT protocol error rather
/// than a timeout. So a reply timeout is evidence AGAINST this mechanism, not
/// for it — which is what excluded it as the cause of the resident-upload stall
/// in docs/experiments/kbm-resident-upload-notify-stall-2026-08-31.md, whose
/// real cause was a notification that was never scheduled. The retry allowlist
/// here still earns its place: what it covers is a command whose write succeeded
/// and whose reply went missing for any reason at all.
/// </para>
/// <para>
/// REPEATABLE MEANS THE SECOND SEND CANNOT CHANGE THE OUTCOME OF THE FIRST, even
/// if the first was received and only its reply went missing. That is stronger
/// than "read-only", and it is why this is an allowlist: a command whose safety
/// has not been reasoned about is treated as unsafe.
/// </para>
/// <para>
/// <c>amiibo chunk</c> writes at an absolute offset, so a repeat stores the same
/// bytes in the same place; reads and status change nothing. <c>amiibo begin</c>
/// is excluded although it looks harmless — repeating one that was in fact
/// received would reset the staging buffer, the transfer would continue from the
/// wrong place, and the result would be a corrupt tag that still passed its own
/// CRC.
/// </para>
/// </remarks>
public static class ManagementRetryPolicy
{
    /// <summary>Extra attempts after the first, for a repeatable command.</summary>
    public const int MaxRetries = 2;

    private static readonly string[] RepeatablePrefixes =
    [
        "amiibo chunk ",
        "amiibo read ",
        "amiibo status",
        // A staged KB/M draft is assembled in RAM and both of these are ABSOLUTE
        // writes into it: `bind` sets the entry keyed by its source, `mouse` sets
        // one named field to one value. Sending either twice leaves the draft
        // byte-identical, and neither touches stored or realized state — only
        // `commit` does. So the amiibo-chunk argument applies verbatim.
        //
        // `draft begin`, `draft commit` and `draft abort` are excluded and must
        // stay excluded: begin RESETS the staging buffer, and a repeat of a
        // commit whose reply was lost would find no draft and report failure for
        // a transfer that in fact succeeded.
        "kbm draft bind ",
        "kbm draft mouse ",
    ];

    public static bool IsRepeatable(string command)
    {
        var normalized = command.Trim().ToLowerInvariant();
        return RepeatablePrefixes.Any(prefix =>
            normalized.StartsWith(prefix, StringComparison.Ordinal));
    }
}
