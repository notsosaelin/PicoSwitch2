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

    public static string KbmMap(KbmProfile profile, int page)
    {
        if (page is < 0 or > 32)
        {
            throw new ArgumentOutOfRangeException(nameof(page));
        }

        return $"kbm map {profile.Wire()} {page}";
    }

    public static string KbmMode(KbmMode mode) => $"kbm mode {mode.Wire()}";

    public static string KbmBind(KbmProfile profile, KbmSource source, KbmDestination? destination) =>
        $"kbm bind {profile.Wire()} {source.Wire} {(destination is null ? "default" : destination.Value.Wire())}";

    public static string KbmReset(KbmProfile profile) => $"kbm reset {profile.Wire()}";

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
