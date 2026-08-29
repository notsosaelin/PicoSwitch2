using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/*
 * What controllers an adapter has known, as opposed to what it has stored.
 *
 * ## Two different questions
 *
 * The adapter's peer inventory answers "which security records exist right now,
 * and what can I currently prove about their owners". It is authoritative and
 * the app never second-guesses it. But it is also amnesiac by construction: role
 * classification is live evidence only, so after the adapter reboots a saved
 * controller that has not reconnected is reported `unknown` with no name, and a
 * controller the user forgot last week is simply absent.
 *
 * This is the app-side half. It remembers, per adapter, what each peer was the
 * last time the adapter could actually say -- and nothing else. It holds no key
 * material, and it is never consulted to contradict the adapter.
 *
 * ## Why the firmware half is not here
 *
 * Persisting the same metadata on the adapter would make it survive being
 * managed from a different host. It is deliberately not done: that work was
 * attempted, destabilised the Bluetooth core, and was withdrawn. Do not
 * reintroduce adapter flash writes for peer metadata without new evidence about
 * that failure.
 *
 * ## What history is and is not allowed to claim
 *
 * The management protocol requires that a client render `unknown` as
 * unidentified and never promote it to `controller`. History does not violate
 * that: it never rewrites `PeerInfo.Role`. It supplies a REMEMBERED label and a
 * remembered role alongside the adapter's live answer, and presentation says
 * which is which. The one thing a remembered role does decide is exclusion -- a
 * peer this app has proven to be the user's own PC stays out of the saved
 * controller list even when the adapter has forgotten what it is, because
 * offering to forget the user's PC as though it were a controller is the
 * specific failure the whole role model exists to prevent.
 */

public sealed record PeerHistoryRecord
{
    /// <summary>The adapter's opaque peer handle. Stable across reboots, not a slot index.</summary>
    public required string PeerId { get; init; }

    public string Address { get; init; } = "";

    /// <summary>Best remote-supplied name ever seen for this peer.</summary>
    public string? LastKnownName { get; init; }

    /// <summary>Best adapter-derived classification ever seen, e.g. <c>Sony DualSense</c>.</summary>
    public string? Classification { get; init; }

    public int VendorId { get; init; }

    public int ProductId { get; init; }

    /// <summary>The strongest role the adapter has ever actually proven for this peer.</summary>
    public PeerRole ProvenRole { get; init; } = PeerRole.Unknown;

    public PeerTransportSet Transports { get; init; }

    public long FirstSeenAtMillis { get; init; }

    /// <summary>When this peer was last present in an inventory read.</summary>
    public long LastSeenAtMillis { get; init; }

    public long? LastConnectedAtMillis { get; init; }

    /// <summary>Whether the adapter still held a security record at the last complete read.</summary>
    public bool Bonded { get; init; }

    /// <summary>The best name this app can offer without the adapter's live answer.</summary>
    public string RememberedName => PeerNaming.Label(
        address: Address,
        classification: Classification,
        name: LastKnownName,
        vendorId: VendorId,
        productId: ProductId);

    /// <summary>The user's own PC, in either of its two relationships.</summary>
    public bool IsCompanionRole =>
        ProvenRole is PeerRole.ManagementCompanion or PeerRole.ControllerLink;

    /// <summary>
    /// Merge one live observation into what is already remembered.
    ///
    /// Every field takes the newer answer only when the newer answer SAYS
    /// something. The adapter reporting <c>unknown</c> after a reboot is not
    /// evidence that the peer stopped being a controller, so it must not erase the
    /// role that was proven while it was connected.
    /// </summary>
    public PeerHistoryRecord UpdatedFrom(PeerInfo peer, long nowMillis) => this with
    {
        Address = string.IsNullOrWhiteSpace(peer.Address) ? Address : peer.Address,
        LastKnownName = peer.Name ?? LastKnownName,
        Classification = peer.Classification ?? Classification,
        VendorId = peer.HasUsbIdentity ? peer.VendorId : VendorId,
        ProductId = peer.HasUsbIdentity ? peer.ProductId : ProductId,
        ProvenRole = PeerRoleStrength.Stronger(ProvenRole, peer.Role),

        // Transports accumulate: a peer seen only over LE this session still holds
        // the Classic key the adapter reported last session.
        Transports = PeerTransportSet.FromMask(Transports.Mask | peer.Transports.Mask),
        FirstSeenAtMillis = FirstSeenAtMillis > 0 ? FirstSeenAtMillis : nowMillis,
        LastSeenAtMillis = nowMillis,
        LastConnectedAtMillis = peer.Connected ? nowMillis : LastConnectedAtMillis,
        Bonded = peer.Bonded,
    };
}

/// <summary>
/// Which of two role claims about one peer wins.
///
/// Written out rather than derived from the enum's declaration order, for the
/// same reason the firmware's <c>role_precedence()</c> is: reordering the enum
/// must not silently change which role a PC that is both the management
/// companion and a Controller Link peer ends up with.
/// </summary>
public static class PeerRoleStrength
{
    public static PeerRole Stronger(PeerRole a, PeerRole b) =>
        Precedence(b) > Precedence(a) ? b : a;

    private static int Precedence(PeerRole role) => role switch
    {
        PeerRole.ManagementCompanion => 3,
        PeerRole.ControllerLink => 2,
        PeerRole.PhysicalController => 1,
        _ => 0,
    };
}

/// <summary>Everything one adapter has known. Keyed by peer id, which the adapter owns.</summary>
public sealed record AdapterPeerHistory
{
    /// <summary>
    /// Twice the adapter's 32-record bond capacity, so a full adapter still leaves
    /// room for a comparable number of previously-forgotten devices.
    /// </summary>
    public const int MaxRecords = 64;

    public ValueList<PeerHistoryRecord> Records { get; init; } = ValueList<PeerHistoryRecord>.Empty;

    public PeerHistoryRecord? Record(string peerId) =>
        Records.FirstOrDefault(record => record.PeerId == peerId);

    /// <summary>Peers the adapter no longer holds a record for; the "Recent" section.</summary>
    public IReadOnlyList<PeerHistoryRecord> Forgotten =>
        Records.Where(record => !record.Bonded).ToList();

    public AdapterPeerHistory Without(string peerId) => this with
    {
        Records = new ValueList<PeerHistoryRecord>(
            Records.Where(record => record.PeerId != peerId)),
    };

    /// <summary>
    /// Fold one COMPLETE inventory read into history.
    ///
    /// Complete is load-bearing and the caller must not pass a partial read: a
    /// missing row would be indistinguishable from a peer the adapter has
    /// forgotten, and this method would then mark a live saved controller as
    /// historical.
    /// </summary>
    public AdapterPeerHistory Observing(PeerInventory inventory, long nowMillis)
    {
        if (!inventory.Complete)
        {
            throw new ArgumentException(
                "History may only observe a complete inventory read",
                nameof(inventory));
        }

        var seen = inventory.Peers.ToDictionary(peer => peer.Id, StringComparer.Ordinal);
        var updated = Records
            .Select(record => seen.TryGetValue(record.PeerId, out var peer)
                ? record.UpdatedFrom(peer, nowMillis)

                // Absent from a complete read means the adapter no longer holds a
                // record for it. The peer is kept -- that is the whole point of
                // history -- but it stops being a saved pairing.
                : record with { Bonded = false })
            .ToList();

        var known = Records.Select(record => record.PeerId).ToHashSet(StringComparer.Ordinal);
        updated.AddRange(inventory.Peers
            .Where(peer => !known.Contains(peer.Id))
            .Select(peer => NewRecord(peer, nowMillis)));

        return this with { Records = new ValueList<PeerHistoryRecord>(Prune(updated)) };
    }

    private static PeerHistoryRecord NewRecord(PeerInfo peer, long nowMillis) => new()
    {
        PeerId = peer.Id,
        Address = peer.Address,
        LastKnownName = peer.Name,
        Classification = peer.Classification,
        VendorId = peer.VendorId,
        ProductId = peer.ProductId,
        ProvenRole = peer.Role,
        Transports = peer.Transports,
        FirstSeenAtMillis = nowMillis,
        LastSeenAtMillis = nowMillis,
        LastConnectedAtMillis = peer.Connected ? nowMillis : null,
        Bonded = peer.Bonded,
    };

    /// <summary>
    /// Keep the newest <see cref="MaxRecords"/> and never evict something the
    /// adapter still holds a key for. A saved pairing dropped from history would
    /// silently lose the only name the app can show for it once the adapter
    /// reboots.
    /// </summary>
    private static List<PeerHistoryRecord> Prune(List<PeerHistoryRecord> records)
    {
        if (records.Count <= MaxRecords)
        {
            return records;
        }

        var kept = records.Where(record => record.Bonded).ToList();
        var room = Math.Max(MaxRecords - kept.Count, 0);
        var evictable = records
            .Where(record => !record.Bonded)
            .OrderByDescending(record => record.LastSeenAtMillis)
            .Take(room);
        return [.. kept, .. evictable];
    }
}

/// <summary>History for every adapter this app knows. One document, one file.</summary>
public sealed record PeerHistoryBook
{
    public IReadOnlyDictionary<AdapterId, AdapterPeerHistory> ByAdapter { get; init; } =
        new Dictionary<AdapterId, AdapterPeerHistory>();

    public AdapterPeerHistory ForAdapter(AdapterId? id) =>
        id is { } wanted && ByAdapter.TryGetValue(wanted, out var history)
            ? history
            : new AdapterPeerHistory();

    public PeerHistoryBook With(AdapterId id, AdapterPeerHistory history) => this with
    {
        ByAdapter = new Dictionary<AdapterId, AdapterPeerHistory>(ByAdapter) { [id] = history },
    };

    /// <summary>Dropped with the adapter: history about an adapter the app no longer knows is orphaned.</summary>
    public PeerHistoryBook Without(AdapterId id)
    {
        var next = new Dictionary<AdapterId, AdapterPeerHistory>(ByAdapter);
        next.Remove(id);
        return this with { ByAdapter = next };
    }
}

/// <summary>
/// The persisted history document.
///
/// Versioned, and decoding is total for the same reason the adapter registry's
/// is: this is read at startup, and a document this build cannot parse must cost
/// the user their history, never their ability to launch the app. One unreadable
/// row is dropped rather than failing its adapter, and one unreadable adapter is
/// dropped rather than failing the document.
/// </summary>
public static class PeerHistoryCodec
{
    public const int Schema = 1;

    private const int MaxText = 64;

    public static string Encode(PeerHistoryBook book)
    {
        var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema", Schema);
            writer.WriteStartArray("adapters");
            foreach (var (id, history) in book.ByAdapter)
            {
                writer.WriteStartObject();
                writer.WriteString("adapter", id.Value);
                writer.WriteStartArray("peers");
                foreach (var record in history.Records)
                {
                    WriteRecord(writer, record);
                }

                writer.WriteEndArray();
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    public static PeerHistoryBook Decode(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return new PeerHistoryBook();
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(text);
        }
        catch (JsonException)
        {
            return new PeerHistoryBook();
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("schema", out var schemaElement) ||
                schemaElement.ValueKind != JsonValueKind.Number ||
                !schemaElement.TryGetInt32(out var schema) ||

                // A future schema is not readable here. The document is left on
                // disk untouched so a later build can still read it.
                schema > Schema)
            {
                return new PeerHistoryBook();
            }

            var byAdapter = new Dictionary<AdapterId, AdapterPeerHistory>();
            if (root.TryGetProperty("adapters", out var adapters) &&
                adapters.ValueKind == JsonValueKind.Array)
            {
                foreach (var row in adapters.EnumerateArray())
                {
                    if (row.ValueKind != JsonValueKind.Object ||
                        AdapterId.FromAddress(Text(row, "adapter")) is not { } id)
                    {
                        continue;
                    }

                    var records = new List<PeerHistoryRecord>();
                    var seen = new HashSet<string>(StringComparer.Ordinal);
                    if (row.TryGetProperty("peers", out var peers) &&
                        peers.ValueKind == JsonValueKind.Array)
                    {
                        foreach (var peerRow in peers.EnumerateArray())
                        {
                            if (peerRow.ValueKind == JsonValueKind.Object &&
                                DecodeRecord(peerRow) is { } record &&
                                seen.Add(record.PeerId))
                            {
                                records.Add(record);
                            }
                        }
                    }

                    byAdapter[id] = new AdapterPeerHistory
                    {
                        Records = new ValueList<PeerHistoryRecord>(records),
                    };
                }
            }

            return new PeerHistoryBook { ByAdapter = byAdapter };
        }
    }

    private static void WriteRecord(Utf8JsonWriter writer, PeerHistoryRecord record)
    {
        writer.WriteStartObject();
        writer.WriteString("id", record.PeerId);
        writer.WriteString("addr", record.Address);
        if (record.LastKnownName is { } name)
        {
            writer.WriteString("name", name);
        }

        if (record.Classification is { } classification)
        {
            writer.WriteString("class", classification);
        }

        if (record.VendorId != 0 || record.ProductId != 0)
        {
            writer.WriteNumber("vid", record.VendorId);
            writer.WriteNumber("pid", record.ProductId);
        }

        writer.WriteString("role", record.ProvenRole.WireName());
        writer.WriteNumber("tr", record.Transports.Mask);
        writer.WriteNumber("firstSeen", record.FirstSeenAtMillis);
        writer.WriteNumber("lastSeen", record.LastSeenAtMillis);
        if (record.LastConnectedAtMillis is { } lastConnected)
        {
            writer.WriteNumber("lastConnected", lastConnected);
        }

        if (record.Bonded)
        {
            writer.WriteBoolean("bonded", true);
        }

        writer.WriteEndObject();
    }

    private static PeerHistoryRecord? DecodeRecord(JsonElement row)
    {
        var peerId = Text(row, "id");
        if (string.IsNullOrWhiteSpace(peerId))
        {
            return null;
        }

        return new PeerHistoryRecord
        {
            PeerId = Truncate(peerId, MaxText)!,
            Address = Truncate(Text(row, "addr"), MaxText) ?? string.Empty,

            // Re-sanitised on read. These strings originated as untrusted remote
            // Bluetooth names; that they entered clean is not proof the bytes on
            // disk still are, and they go straight back into the UI.
            LastKnownName = PeerText.Sanitize(Text(row, "name")),
            Classification = PeerText.Sanitize(Text(row, "class")),
            VendorId = Math.Clamp(Number(row, "vid") ?? 0, 0, 0xFFFF),
            ProductId = Math.Clamp(Number(row, "pid") ?? 0, 0, 0xFFFF),
            ProvenRole = PeerRoles.FromWire(Text(row, "role")),
            Transports = PeerTransportSet.FromMask(Number(row, "tr") ?? 0),
            FirstSeenAtMillis = Number64(row, "firstSeen") ?? 0,
            LastSeenAtMillis = Number64(row, "lastSeen") ?? 0,
            LastConnectedAtMillis = Number64(row, "lastConnected"),
            Bonded = row.TryGetProperty("bonded", out var bonded) &&
                bonded.ValueKind == JsonValueKind.True,
        };
    }

    private static string? Text(JsonElement element, string key) =>
        element.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static int? Number(JsonElement element, string key) =>
        element.TryGetProperty(key, out var value) &&
        value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var parsed)
            ? parsed
            : null;

    private static long? Number64(JsonElement element, string key) =>
        element.TryGetProperty(key, out var value) &&
        value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt64(out var parsed)
            ? parsed
            : null;

    private static string? Truncate(string? value, int max) =>
        value is null ? null : value.Length <= max ? value : value[..max];
}

/// <summary>
/// Cleaning for text that came off the radio.
///
/// The adapter already reduces remote names to printable ASCII before they reach
/// the wire, so this is the second line rather than the first: it exists because
/// this app also reads these strings back from its own storage, where they could
/// have been edited, and because a name reaching a diagnostic line must not be
/// able to split it.
/// </summary>
public static partial class PeerText
{
    public const int MaxLength = 48;

    public static string? Sanitize(string? raw)
    {
        var replaced = new string((raw ?? string.Empty)
            .Select(character => char.IsControl(character) ? ' ' : character)
            .ToArray());
        var cleaned = WhitespaceRun().Replace(replaced.Trim(), " ");
        var truncated = cleaned.Length <= MaxLength ? cleaned : cleaned[..MaxLength];
        truncated = truncated.Trim();
        return truncated.Length == 0 ? null : truncated;
    }

    [GeneratedRegex(@"\s+")]
    private static partial Regex WhitespaceRun();
}
