using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

// ValueList<T> lives in the management core because that is where the need for
// structural equality on a record's collection first appeared. Reusing it here
// beats a second copy: two "immutable list with value equality" types that drift
// apart is exactly the duplication this project keeps closing.
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Identity of one physical adapter, as this app knows it.
///
/// ## Why the address and not something new
///
/// The management peripheral advertises with <c>BD_ADDR_TYPE_LE_PUBLIC</c>
/// (<c>config_ble_start_advertising</c> in <c>btstack_host.c</c>), so the adapter
/// already has one stable public identifier that this app can see, that Windows
/// resolves bonds against, and that costs no extra broadcast. Inventing a
/// firmware-issued ID and advertising it would add a permanent radio identifier
/// for no benefit the address does not already provide.
///
/// It is wrapped rather than passed as a bare string for two reasons. It makes
/// "identity" impossible to confuse with the user's alias or with a list
/// position, which the design forbids as identity; and if a firmware-supplied
/// identity is ever added, only the factory methods and the codec change.
///
/// **Strong evidence, not confirmed:** the public address survives a firmware
/// flash. The whole repair path depends on it — a reflashed adapter answers at
/// the same address with no key — but it has never been the subject of its own
/// test.
/// </summary>
public readonly partial record struct AdapterId
{
    private AdapterId(string value) => Value = value;

    public string Value { get; }

    /// <summary>
    /// Four hex characters for disambiguating two adapters the user gave the same
    /// alias. Presentation only; never an identity of its own.
    /// </summary>
    public string ShortLabel
    {
        get
        {
            var compact = Value.Replace(":", string.Empty);
            return compact.Length <= 4 ? compact : compact[^4..];
        }
    }

    /// <summary>Null for anything that is not a Bluetooth address; callers must not invent one.</summary>
    public static AdapterId? FromAddress(string? address) =>
        address is not null && MacPattern().IsMatch(address)
            ? new AdapterId(address.ToUpperInvariant())
            : null;

    /// <summary>
    /// The Windows form of the same identity.
    ///
    /// WinRT hands out a 48-bit address as a <c>ulong</c>
    /// (<c>BluetoothLEDevice.BluetoothAddress</c>,
    /// <c>BluetoothLEAdvertisementReceivedEventArgs.BluetoothAddress</c>), while
    /// the registry document and every diagnostic use the canonical colon form.
    /// Converting in exactly one place is what keeps the two from becoming two
    /// different identities for one adapter.
    /// </summary>
    public static AdapterId? FromBluetoothAddress(ulong address) =>
        FromAddress(BluetoothAddressFormat.ToText(address));

    /// <summary>The <c>ulong</c> WinRT wants back, for connecting to a remembered row.</summary>
    public ulong ToBluetoothAddress() =>
        BluetoothAddressFormat.TryParse(Value, out var address) ? address : 0;

    public override string ToString() => Value;

    [GeneratedRegex("^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$")]
    private static partial Regex MacPattern();
}

/// <summary>
/// How Windows sees the pairing for one adapter.
///
/// The Android port had four states and a Companion Device association alongside
/// them. Windows has no association concept and needs none
/// (WINDOWS_PASS.md §19.2): <c>DeviceInformation.Id</c> plus the pairing state
/// already provide a stable handle and a trust state, so each row has three
/// relationship truths rather than four.
/// </summary>
public enum WindowsPairingState
{
    Unknown,
    NotPaired,
    Paired,
}

/// <summary>
/// One adapter this app remembers.
///
/// Everything except <c>Id</c> and <c>Address</c> is cache or presentation. None
/// of it is authoritative about the adapter's own state:
/// <c>LastFirmwareVersion</c> and <c>LastPersonality</c> are what was true at the
/// last verified connection, and exist so the adapter list can say something
/// honest about an adapter that is not currently connected. Live truth comes from
/// <c>AdapterSnapshot</c> and belongs to whichever adapter is connected now.
/// </summary>
public sealed record AdapterRecord
{
    public const string DefaultProductName = "PicoSwitch2";

    public required AdapterId Id { get; init; }

    public required string Address { get; init; }

    /*
     * There is deliberately NO cached Windows device path here.
     *
     * One used to exist, on the reasoning that DeviceInformationPairing and
     * GattSession.FromDeviceIdAsync both want a path and re-resolving costs a
     * discovery pass. Nothing ever populated it -- the single construction site
     * left it null and no code path ever assigned one -- so every consumer took
     * its null branch forever. Repair's null branch logged a warning, cleared the
     * repair flag and returned WITHOUT unpairing, reporting a repair that had not
     * happened. Observed on hardware 2026-08-29.
     *
     * The Address below is the durable identifier, and Windows re-resolves the
     * device (and its pairing object) from it on demand. Do not reintroduce a
     * cached path: it was never identity anyway -- it can change across a stack
     * reinstall while the adapter stays the same adapter.
     *
     * Documents written before 2026-08-29 may still carry a "device" key. It is
     * ignored on load rather than migrated; there is nothing in it worth keeping.
     */

    public string? UserAlias { get; init; }

    public string LastKnownName { get; init; } = DefaultProductName;

    public long? LastSeenAtMillis { get; init; }

    public long? LastConnectedAtMillis { get; init; }

    public string? LastFirmwareVersion { get; init; }

    public string? LastPersonality { get; init; }

    /// <summary>
    /// Windows believes it is paired but this adapter rejected or lacked the key
    /// — what a reflash looks like from the PC. Per-adapter on purpose:
    /// reflashing one adapter must not push the others into repair.
    /// </summary>
    public bool RepairRequired { get; init; }

    /// <summary>
    /// Display priority: user alias, then the last known Bluetooth/product name,
    /// then product plus a short identity suffix. The alias never becomes
    /// identity and is never written to the adapter.
    /// </summary>
    public string DisplayName =>
        !string.IsNullOrWhiteSpace(UserAlias) ? UserAlias
        : !string.IsNullOrWhiteSpace(LastKnownName) ? LastKnownName
        : $"{DefaultProductName} {Id.ShortLabel}";

    public static AdapterRecord? Of(string address, string? name = null)
    {
        if (AdapterId.FromAddress(address) is not { } id)
        {
            return null;
        }

        return new AdapterRecord
        {
            Id = id,
            Address = id.Value,
            LastKnownName = string.IsNullOrWhiteSpace(name) ? DefaultProductName : name,
        };
    }
}

/// <summary>
/// Every adapter this app knows, plus which one is currently selected.
///
/// Many known adapters, at most one active management session. The registry
/// deliberately holds no live connection state: it is what survives process
/// death, and a live GATT session does not.
/// </summary>
public sealed record AdapterRegistry
{
    public ValueList<AdapterRecord> Records { get; init; } = ValueList<AdapterRecord>.Empty;

    public AdapterId? ActiveId { get; init; }

    public AdapterRecord? Active => Record(ActiveId);

    public AdapterRecord? Record(AdapterId? id) =>
        id is { } wanted ? Records.FirstOrDefault(record => record.Id == wanted) : null;

    public AdapterRecord? Record(string? address) => Record(AdapterId.FromAddress(address));

    /// <summary>Insert or replace by identity, preserving list order for existing rows.</summary>
    public AdapterRegistry With(AdapterRecord record)
    {
        var next = Records.ToList();
        var index = next.FindIndex(existing => existing.Id == record.Id);
        if (index < 0)
        {
            next.Add(record);
        }
        else
        {
            next[index] = record;
        }

        return this with { Records = new ValueList<AdapterRecord>(next) };
    }

    public AdapterRegistry Update(AdapterId id, Func<AdapterRecord, AdapterRecord> transform) =>
        Record(id) is { } record ? With(transform(record)) : this;

    /// <summary>
    /// Remove one adapter from the app. This is not a Bluetooth operation: the
    /// Windows pairing and the adapter's own bonds are untouched. Unpairing is a
    /// separate, explicitly confirmed action (WINDOWS_PASS.md §19.6).
    /// </summary>
    public AdapterRegistry Without(AdapterId id) => this with
    {
        Records = new ValueList<AdapterRecord>(Records.Where(record => record.Id != id)),
        ActiveId = ActiveId == id ? null : ActiveId,
    };

    /// <summary>Selecting an unknown adapter is meaningless; it clears the selection instead.</summary>
    public AdapterRegistry Selecting(AdapterId? id) => this with
    {
        ActiveId = id is { } wanted && Record(wanted) is not null ? wanted : null,
    };

    /// <summary>
    /// Whether this row must show its short identity to stay distinguishable.
    ///
    /// Duplicate aliases are allowed — the design says so — but two rows reading
    /// "Living Room" with no way to tell them apart is not a list, it is a coin
    /// toss.
    /// </summary>
    public bool NeedsShortLabel(AdapterRecord record) =>
        Records.Count(other =>
            string.Equals(other.DisplayName, record.DisplayName, StringComparison.OrdinalIgnoreCase)) > 1;
}

/// <summary>
/// User-supplied adapter names are untrusted text that reaches the UI, the
/// diagnostic log and persisted JSON, so they are cleaned at the one point where
/// they enter the app rather than at each place they are displayed.
/// </summary>
public static partial class AdapterAlias
{
    public const int MaxLength = 40;

    /// <summary>
    /// Returns null for anything that is empty once cleaned, which is how the user
    /// clears an alias and falls back to the adapter's own name.
    /// </summary>
    public static string? Sanitize(string? raw)
    {
        // Control characters cover the newline that would otherwise split a
        // diagnostic line, and the tab/NUL that survive an ordinary trim.
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

/// <summary>
/// The persisted registry document.
///
/// Versioned from the start. Decoding is deliberately total: a document this app
/// cannot read must degrade to "no adapters known", never to a crash on launch,
/// because the registry is read before anything else can be shown. Individual
/// unreadable rows are dropped rather than failing the whole document, so one
/// corrupt entry cannot cost the user their other adapters.
/// </summary>
public static class AdapterRegistryCodec
{
    public const int Schema = 1;

    private const int MaxCachedText = 32;

    public static string Encode(AdapterRegistry registry)
    {
        var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema", Schema);
            if (registry.ActiveId is { } active)
            {
                writer.WriteString("active", active.Value);
            }

            writer.WriteStartArray("adapters");
            foreach (var record in registry.Records)
            {
                writer.WriteStartObject();
                writer.WriteString("id", record.Id.Value);
                writer.WriteString("address", record.Address);
                if (record.UserAlias is { } alias)
                {
                    writer.WriteString("alias", alias);
                }

                writer.WriteString("name", record.LastKnownName);
                if (record.LastSeenAtMillis is { } lastSeen)
                {
                    writer.WriteNumber("lastSeen", lastSeen);
                }

                if (record.LastConnectedAtMillis is { } lastConnected)
                {
                    writer.WriteNumber("lastConnected", lastConnected);
                }

                if (record.LastFirmwareVersion is { } firmware)
                {
                    writer.WriteString("firmware", firmware);
                }

                if (record.LastPersonality is { } personality)
                {
                    writer.WriteString("personality", personality);
                }

                if (record.RepairRequired)
                {
                    writer.WriteBoolean("repair", true);
                }

                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    public static AdapterRegistry Decode(string? text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return new AdapterRegistry();
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(text);
        }
        catch (JsonException)
        {
            return new AdapterRegistry();
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("schema", out var schemaElement) ||
                schemaElement.ValueKind != JsonValueKind.Number ||
                !schemaElement.TryGetInt32(out var schema))
            {
                return new AdapterRegistry();
            }

            // An unknown future schema is not readable by this build. Returning an
            // empty registry is wrong for the user but safe; the document is left
            // on disk untouched so a later build can still read it.
            if (schema > Schema)
            {
                return new AdapterRegistry();
            }

            var records = new List<AdapterRecord>();
            var seen = new HashSet<AdapterId>();
            if (root.TryGetProperty("adapters", out var adapters) &&
                adapters.ValueKind == JsonValueKind.Array)
            {
                foreach (var row in adapters.EnumerateArray())
                {
                    if (row.ValueKind != JsonValueKind.Object || DecodeRecord(row) is not { } record)
                    {
                        continue;
                    }

                    if (seen.Add(record.Id))
                    {
                        records.Add(record);
                    }
                }
            }

            var activeId = AdapterId.FromAddress(Text(root, "active"));
            return new AdapterRegistry { Records = new ValueList<AdapterRecord>(records) }
                .Selecting(activeId);
        }
    }

    private static AdapterRecord? DecodeRecord(JsonElement row)
    {
        var address = Text(row, "address") ?? Text(row, "id");
        if (AdapterId.FromAddress(address) is not { } id)
        {
            return null;
        }

        return new AdapterRecord
        {
            Id = id,
            Address = id.Value,

            // Re-sanitize on read: the document may have been hand-edited, and a
            // value that entered clean is not proof the bytes on disk still are.
            UserAlias = AdapterAlias.Sanitize(Text(row, "alias")),
            LastKnownName = AdapterAlias.Sanitize(Text(row, "name")) ?? AdapterRecord.DefaultProductName,
            LastSeenAtMillis = Number(row, "lastSeen"),
            LastConnectedAtMillis = Number(row, "lastConnected"),
            LastFirmwareVersion = Truncate(Text(row, "firmware"), MaxCachedText),
            LastPersonality = Truncate(Text(row, "personality"), MaxCachedText),
            RepairRequired = row.TryGetProperty("repair", out var repair) &&
                repair.ValueKind == JsonValueKind.True,
        };
    }

    private static string? Text(JsonElement element, string key) =>
        element.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static long? Number(JsonElement element, string key) =>
        element.TryGetProperty(key, out var value) &&
        value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt64(out var parsed)
            ? parsed
            : null;

    private static string? Truncate(string? value, int max) =>
        value is null ? null : value.Length <= max ? value : value[..max];
}
