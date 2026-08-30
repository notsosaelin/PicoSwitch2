using System.Globalization;
using System.Text.RegularExpressions;

namespace PicoSwitch.Management;

/// <summary>
/// Exact command vocabulary and argument encoding accepted by production
/// management.
///
/// Verified against the <c>builders</c> vectors in
/// <c>tools/fixtures/management/protocol-v1.json</c>, so a divergence between
/// this and the Kotlin builder fails in both languages against one shared
/// authority rather than being discovered on hardware.
/// </summary>
public static partial class ManagementCommands
{
    public const string Info = "info";
    public const string Ping = "ping";
    public const string GetConfig = "get";
    public const string Device = "device";
    public const string InputSources = "input sources";
    public const string Personality = "personality";
    public const string Reenumerate = "reenumerate";
    public const string Wake = "wake";
    public const string WakeStatus = "wake status";
    public const string ManagementStatus = "mgmt status";
    public const string Save = "save";
    public const string SaveStatus = "save status";
    public const string AmiiboStatus = "amiibo status";
    public const string KbmStatus = "kbm status";
    public const string KbmMouseStatus = "kbm mouse";

    /// <summary>
    /// Remote controller pairing.
    ///
    /// No duration argument: the window is the firmware's to choose, and the same
    /// window the adapter's own pairing button opens. Letting a client set it
    /// would make the physical gesture's behaviour depend on what an app asked
    /// for earlier.
    /// </summary>
    public const string PairingStart = "pairing start";

    public const string PairingStatus = "pairing status";
    public const string PairingCancel = "pairing cancel";

    public const string KbmResetAll = "kbm reset all";
    public const string AmiiboCancel = "amiibo cancel";
    public const string AmiiboPersist = "amiibo persist";
    public const string AmiiboDownloaded = "amiibo downloaded";
    public const string AmiiboClear = "amiibo clear";

    public static string InputActive(long sourceId)
    {
        if (sourceId is < 0 or > 0xFFFF_FFFFL)
        {
            throw new ArgumentOutOfRangeException(nameof(sourceId));
        }

        return $"input active {(sourceId == 0L ? "none" : sourceId.ToString())}";
    }

    public static string SetPersonality(Personality value)
    {
        if (value is not (Management.Personality.Pro2 or Management.Personality.GameCube
            or Management.Personality.JoyConLeft or Management.Personality.JoyConRight))
        {
            throw new ArgumentOutOfRangeException(nameof(value), $"Not a selectable personality: {value}");
        }

        return $"personality {value.WireName()}";
    }

    public static string ManagementEnabled(bool enabled) => enabled ? "mgmt on" : "mgmt off";

    public static string BondsPage(int? cursor = null)
    {
        if (cursor is < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(cursor), "Bond cursor cannot be negative");
        }

        return cursor is null ? "bonds list v2" : $"bonds list v2 {cursor}";
    }

    public static string BondRemove(int index)
    {
        if (index < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }

        return $"bonds remove {index}";
    }

    /// <summary>The cursor is a peer index, never a database slot; slots are reused, peers are sorted.</summary>
    public static string PeersPage(int? cursor = null)
    {
        if (cursor is < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(cursor), "Peer cursor cannot be negative");
        }

        return cursor is null ? "peers list" : $"peers list {cursor}";
    }

    /// <summary>
    /// Forget one peer by the opaque id the adapter issued for it.
    ///
    /// The id is validated here as well as by the firmware, because the local
    /// failure is a thrown precondition the developer sees, while the remote one
    /// is a usage error the user sees.
    /// </summary>
    public static string PeersForget(string peerId)
    {
        if (!PeerIdPattern().IsMatch(peerId))
        {
            throw new ArgumentException($"Not a peer id: {peerId}", nameof(peerId));
        }

        return $"peers forget {peerId}";
    }

    /// <summary>
    /// One slice of a layout's REALIZED mapping, from logical item
    /// <paramref name="cursor"/>.
    /// </summary>
    /// <remarks>
    /// A cursor, not a page index: rows are variable width, so no fixed page
    /// size is both safe for the worst-case row and complete for the common one.
    /// The adapter answers with the cursor to resume from.
    /// </remarks>
    public static string KbmMap(KbmLayout profile, int cursor)
    {
        if (cursor is < 0 or > KbmLimits.MaxMappingItems)
        {
            throw new ArgumentOutOfRangeException(nameof(cursor));
        }

        return $"kbm map {profile.Wire()} {cursor}";
    }

    public static string KbmMode(KbmMode mode) => $"kbm mode {mode.Wire()}";

    /// <summary>
    /// The ingress counters, split out of <c>kbm status</c> because the two
    /// together outgrew the wireless response slot.
    /// </summary>
    public const string KbmCounters = "kbm counters";

    public const string KbmProfileList = "kbm profiles";

    /// <summary>
    /// One slice of the profile library, from logical item
    /// <paramref name="cursor"/>. Six rows do not fit one reply.
    /// </summary>
    public static string KbmProfilePage(int cursor) => $"kbm profiles {cursor}";

    /// <summary>The realized mapping of each layout, and its divergence state.</summary>
    public const string KbmActive = "kbm active";

    /// <summary>
    /// APPLY. The only command that changes what the console is doing.
    ///
    /// Deliberately separate from saving: a user who edits and saves a profile
    /// has changed the library, not the adapter's behaviour, and conflating the
    /// two is what made a mapping edit feel like it had silently failed.
    /// </summary>
    // Built as whole command strings rather than assembled from a bare "default"
    // fragment, matching the Kotlin builder: the parity checker reads string
    // literals in the command object as commands, and a fragment would look like
    // a verb the firmware never dispatches.
    public static string KbmApply(KbmLayout layout, int id) =>
        id == KbmProfileIds.Default
            ? $"kbm apply {layout.Wire()} default"
            : $"kbm apply {layout.Wire()} {id.ToString(CultureInfo.InvariantCulture)}";

    public static string KbmProfileRename(int id, string name) =>
        $"kbm profile rename {id} {name}";

    public static string KbmProfileDuplicate(int id, string name) =>
        $"kbm profile dup {id} {name}";

    public static string KbmProfileDelete(int id) => $"kbm profile delete {id}";

    /// <summary>Read one STORED profile's mapping, not the realized one.</summary>
    public static string KbmProfileMap(int id, int cursor) =>
        $"kbm pmap {id} {cursor}";

    // --- staged profile write -------------------------------------------------
    // A profile does not fit one management frame, and a loop of per-binding
    // writes is not a transaction: a disconnect halfway leaves the adapter
    // running half of one mapping and half of another. Nothing between Begin and
    // Commit touches stored or realized state.

    /// <param name="id">
    /// The profile being written, or <see cref="KbmProfileIds.None"/> to create.
    /// </param>
    /// <param name="baseRevision">
    /// The revision the draft was built from. The adapter rejects the commit if
    /// its stored profile has moved on, rather than overwriting it.
    /// </param>
    public static string KbmDraftBegin(KbmLayout layout, int id,
                                       int baseRevision, string name) =>
        id == KbmProfileIds.None
            ? $"kbm draft begin {layout.Wire()} new {baseRevision} {name}"
            : $"kbm draft begin {layout.Wire()} {id} {baseRevision} {name}";

    /// <summary>
    /// Begin an upload targeting a specific BANK POSITION.
    /// </summary>
    /// <remarks>
    /// The assignment form: "put this local profile in Keyboard Profile 2". If
    /// that position holds a profile the upload replaces its content and keeps
    /// its stable id, so a switch key bound to the position keeps working; if it
    /// is empty the profile is created there. The adapter refuses a position it
    /// cannot honour rather than landing somewhere else.
    /// </remarks>
    public static string KbmDraftBeginAt(KbmLayout layout, int position,
                                         int baseRevision, string name)
    {
        if (position is < 1 or > KbmLimits.PositionsPerLayout)
        {
            throw new ArgumentOutOfRangeException(nameof(position));
        }

        return $"kbm draft begin {layout.Wire()} pos:{position} {baseRevision} {name}";
    }

    /// <summary>
    /// Empty one bank position on the adapter.
    /// </summary>
    /// <remarks>
    /// Addressed by POSITION because that is what the user chose. The local
    /// library copy is a separate store and is untouched.
    /// </remarks>
    public static string KbmRemove(KbmLayout layout, int position)
    {
        if (position is < 1 or > KbmLimits.PositionsPerLayout)
        {
            throw new ArgumentOutOfRangeException(nameof(position));
        }

        return $"kbm remove {layout.Wire()} {position.ToString(CultureInfo.InvariantCulture)}";
    }

    /// <summary>The persisted boot position for one layout.</summary>
    public static string KbmBoot(KbmLayout layout, int position) =>
        position == KbmPositions.Default
            ? $"kbm boot {layout.Wire()} default"
            : $"kbm boot {layout.Wire()} {position.ToString(CultureInfo.InvariantCulture)}";

    /// <summary>The profile-switch key assignments. One table for both layouts.</summary>
    public const string KbmSwitches = "kbm switches";

    /// <summary>
    /// Assign or clear one profile-switch key.
    /// </summary>
    /// <remarks>
    /// No layout argument, deliberately: the binding names a semantic POSITION
    /// and the adapter resolves it through whichever layout is derived when the
    /// key is pressed. Requiring a layout here would force the user to configure
    /// two disjoint key ranges for the same four actions.
    /// </remarks>
    public static string KbmSwitchBind(KbmSource source, int? position) =>
        position is null
            ? $"kbm switch {source.Wire} none"
            : position == KbmPositions.Default
                ? $"kbm switch {source.Wire} default"
                : $"kbm switch {source.Wire} {position.Value.ToString(CultureInfo.InvariantCulture)}";

    public static string KbmDraftBind(KbmSource source, KbmDestination destination) =>
        $"kbm draft bind {source.Wire} {destination.Wire()}";

    public static string KbmDraftMouse(KbmMouseField field, int value) =>
        $"kbm draft mouse {field.Wire()} {value.ToString(CultureInfo.InvariantCulture)}";

    public const string KbmDraftCommit = "kbm draft commit";
    public const string KbmDraftAbort = "kbm draft abort";

    public static string KbmBind(KbmLayout profile, KbmSource source, KbmDestination? destination) =>
        $"kbm bind {profile.Wire()} {source.Wire} {(destination is null ? "default" : destination.Value.Wire())}";

    public static string KbmReset(KbmLayout profile) => $"kbm reset {profile.Wire()}";

    public static string KbmMouse(KbmMouseField field, int value) =>
        $"kbm mouse {field.Wire()} {value}";

    public static string Color(ColorTarget target, RgbColor color) =>
        $"{target.Command()} {color.Wire()}";

    public static string AmiiboBegin(int size, string crc32)
    {
        if (size is not (540 or 572 or 2048))
        {
            throw new ArgumentOutOfRangeException(nameof(size), "Unsupported Amiibo image size");
        }

        if (!Crc32Pattern().IsMatch(crc32))
        {
            throw new ArgumentException("CRC32 must contain exactly eight hex digits", nameof(crc32));
        }

        return $"amiibo begin {size} {crc32.ToUpperInvariant()}";
    }

    public static string AmiiboChunk(int offset, ReadOnlySpan<byte> bytes)
    {
        if (offset < 0 || bytes.Length == 0 || bytes.Length > ManagementProtocol.AmiiboChunkBytes)
        {
            throw new ArgumentOutOfRangeException(nameof(offset));
        }

        return $"amiibo chunk {offset} {Hex(bytes)}";
    }

    public static string AmiiboRead(int offset, int count)
    {
        if (offset < 0 || count < 1 || count > ManagementProtocol.AmiiboChunkBytes)
        {
            throw new ArgumentOutOfRangeException(nameof(count));
        }

        return $"amiibo read {offset} {count}";
    }

    public static string AmiiboCommit(bool useSave2) =>
        useSave2 ? "amiibo commit save2" : "amiibo commit";

    public static string AmiiboPresented(bool presented) =>
        presented ? "amiibo present" : "amiibo eject";

    public static string AmiiboSelect(bool used) =>
        used ? "amiibo select save2" : "amiibo select save1";

    public static string Hex(ReadOnlySpan<byte> data) => Convert.ToHexString(data);

    [GeneratedRegex("^p_[0-9A-F]{8}$")]
    private static partial Regex PeerIdPattern();

    [GeneratedRegex("^[0-9A-Fa-f]{8}$")]
    private static partial Regex Crc32Pattern();
}
