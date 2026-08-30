using System.Globalization;
using System.Text;
using System.Text.Json;

namespace PicoSwitch.Management;

/// <summary>
/// Logical newline/JSON contract implemented by firmware <c>src/config.c</c>.
///
/// Level 1 reimplementation (WINDOWS_PASS.md §9.4) verified against
/// <c>tools/fixtures/management/protocol-v1.json</c>, which is the
/// language-neutral authority. This file must contain no BLE carrier mechanics:
/// service UUIDs, chunking and the reply limit belong to
/// <see cref="BleManagementContract"/> because the same logical contract is also
/// spoken over UART.
///
/// The parser is strict in the specific ways WINDOWS_PASS.md §13.2 enumerates. A
/// lenient port would silently accept malformed adapter replies, so every helper
/// checks the JSON value KIND before reading it, and <c>boolInt</c> — the one
/// place a boolean or an integer are both legal — exists only for
/// <c>batteryValid</c> and <c>charging</c>, where the firmware emits both forms.
/// </summary>
public static class ManagementProtocol
{
    public const int MaxCommandBytes = 127;
    public const int AmiiboChunkBytes = 32;
    public const int BondsProtocolVersion = 2;

    /// <summary>
    /// Peer inventory version 1. Independent of the bond envelope's version on
    /// purpose: they describe different models and will not move together.
    /// </summary>
    public const int PeersProtocolVersion = 1;

    private const long Uint32Max = 0xFFFF_FFFFL;

    public static byte[] Frame(string command)
    {
        if (string.IsNullOrWhiteSpace(command))
        {
            throw new ArgumentException("Command cannot be blank", nameof(command));
        }

        if (command.Contains('\n') || command.Contains('\r'))
        {
            throw new ArgumentException("Command must be one line", nameof(command));
        }

        var commandBytes = Encoding.UTF8.GetBytes(command);
        if (commandBytes.Length > MaxCommandBytes)
        {
            throw new ArgumentException($"Command exceeds {MaxCommandBytes} bytes", nameof(command));
        }

        var framed = new byte[commandBytes.Length + 1];
        commandBytes.CopyTo(framed, 0);
        framed[^1] = (byte)'\n';
        return framed;
    }

    public static FirmwareInfo Firmware(string command, string response) =>
        Decode(command, response, value =>
        {
            var info = new FirmwareInfo(
                Id: value.String("id"),
                Product: value.String("product"),
                Version: value.String("version"),
                BridgeContract: value.Int("bridge_contract"),
                Build: value.String("build"));
            RequireShape(
                !string.IsNullOrWhiteSpace(info.Id) && !string.IsNullOrWhiteSpace(info.Version),
                command);
            return info;
        });

    public static ControllerInfo Controller(string command, string response) =>
        Decode(command, response, value =>
        {
            var name = value.String("name", "No controller");
            return new ControllerInfo(
                Name: string.IsNullOrWhiteSpace(name) ? "No controller" : name,
                Vid: value.Int("vid"),
                Pid: value.Int("pid"),
                BatteryValid: value.BoolInt("batteryValid"),
                BatteryPercent: value.Int("battery"),
                Charging: value.BoolInt("charging"));
        });

    public static PersonalityState PersonalityState(string command, string response) =>
        Decode(command, response, value =>
        {
            var available = new List<Personality>();
            if (value.TryGetProperty("available", out var entries))
            {
                if (entries.ValueKind != JsonValueKind.Array)
                {
                    throw new ArgumentException("'available' must be an array");
                }

                foreach (var entry in entries.EnumerateArray())
                {
                    if (entry.ValueKind != JsonValueKind.String)
                    {
                        throw new ArgumentException("'available' entries must be strings");
                    }

                    available.Add(Personalities.FromWire(entry.GetString()));
                }
            }

            var state = new Management.PersonalityState
            {
                Current = Personalities.FromWire(value.String("current")),
                Available = new ValueList<Personality>(available),
            };
            RequireShape(
                state.Current != Management.Personality.Unknown && state.Available.Count > 0,
                command);
            return state;
        });

    public static AdapterConfig Config(string command, string response) =>
        Decode(command, response, value =>
        {
            RequireShape(
                value.TryGetProperty("body_color", out _) &&
                value.TryGetProperty("joycon2_left_accent", out _) &&
                value.TryGetProperty("joycon2_right_accent", out _),
                command);

            return new AdapterConfig
            {
                BodyColor = Color(value, "body_color"),
                LeftAccent = Color(value, "joycon2_left_accent"),
                RightAccent = Color(value, "joycon2_right_accent"),
            };
        });

    public static AmiiboStatus Amiibo(string command, string response) =>
        Decode(command, response, value =>
        {
            RequireShape(
                value.TryGetProperty("loaded", out _) &&
                value.TryGetProperty("v3loaded", out _) &&
                value.TryGetProperty("upload", out _),
                command);

            var upload = new AmiiboUpload();
            if (value.TryGetProperty("upload", out var uploadElement))
            {
                if (uploadElement.ValueKind != JsonValueKind.Object)
                {
                    throw new ArgumentException("'upload' must be an object");
                }

                upload = new AmiiboUpload(
                    uploadElement.Bool("active"),
                    uploadElement.Int("received"),
                    uploadElement.Int("size"));
            }

            return new AmiiboStatus
            {
                Loaded = value.Bool("loaded"),
                Dirty = value.Bool("dirty"),
                Presented = value.Bool("presented"),
                V3Loaded = value.Bool("v3loaded"),
                Persisted = value.Bool("persisted"),
                PersistPending = value.Bool("persistPending"),
                Size = value.Int("size"),
                Signature = value.Bool("signature"),
                HasSave2 = value.Bool("hasSave2"),
                UsingSave2 = value.Bool("usingSave2"),
                Generation = value.Long("generation"),
                PayloadCrc = value.String("payloadCrc", "00000000"),
                Uid = value.String("uid"),
                FigureId = value.String("figureId"),
                Upload = upload,
            };
        });

    public static WakeStatus WakeStatus(string command, string response) =>
        Decode(command, response, value => new Management.WakeStatus(
            Result: value.String("result") switch
            {
                "pending" => WakeResult.Pending,
                "advertised" => WakeResult.Advertised,
                "console_awake" => WakeResult.ConsoleAwake,
                "no_identity" => WakeResult.NoIdentity,
                "radio_busy" => WakeResult.RadioBusy,
                _ => WakeResult.Unknown,
            },
            ConsoleAsleep: value.Bool("consoleAsleep"),
            IdentityValid: value.Bool("identityValid"),
            Attempts: value.Long("attempts"),
            LastAttemptMs: value.Long("lastAttemptMs")));

    public static PersistenceStatus PersistenceStatus(string command, string response) =>
        Decode(command, response, value =>
        {
            RequireShape(
                value.TryGetProperty("pending", out _) &&
                value.TryGetProperty("requested", out _) &&
                value.TryGetProperty("completed", out _),
                command);

            var requested = value.Long("requested");
            var completed = value.Long("completed");
            var pending = value.Bool("pending");
            RequireShape(
                requested is >= 0 and <= Uint32Max &&
                completed is >= 0 and <= Uint32Max &&
                pending == (requested != completed),
                command);
            return new Management.PersistenceStatus(pending, requested, completed);
        });

    public static bool? ManagementEnabled(string command, string response) =>
        Decode<bool?>(command, response, value =>
        {
            var primitive = value.Primitive("enabled");
            if (primitive is null)
            {
                return (bool?)null;
            }

            return primitive.Value.ValueKind switch
            {
                JsonValueKind.True => true,
                JsonValueKind.False => false,
                _ => throw new ArgumentException("'enabled' must be a boolean"),
            };
        });

    public static KbmStatus KbmStatus(string command, string response) =>
        Decode(command, response, value =>
        {
            var mode = KbmModes.FromWire(value.String("mode"));
            var modeOverride = KbmModes.FromWire(value.String("override"));
            var profile = KbmLayouts.FromWire(value.String("profile"));
            RequireShape(
                mode is not null && modeOverride is not null && profile is not null &&
                value.TryGetProperty("keyboard", out _) &&
                value.TryGetProperty("mouse", out _) &&
                value.TryGetProperty("nativeMouse", out _),
                command);

            return new Management.KbmStatus(
                Mode: mode!.Value,
                ModeOverride: modeOverride!.Value,
                Profile: profile!.Value,
                KeyboardConnected: value.Bool("keyboard"),
                MouseConnected: value.Bool("mouse"),
                NativeMouseOutput: value.Bool("nativeMouse"),
                KeyboardConn: value.Int("keyboardConn"),
                MouseConn: value.Int("mouseConn"),
                GroupId: value.Long("group"),
                SourceId: value.Long("source"),
                KeyboardReports: value.Long("keyboardReports"),
                MouseReports: value.Long("mouseReports"),
                RejectedMode: value.Long("rejectedMode"),
                RejectedDuplicate: value.Long("rejectedDuplicate"),
                RejectedNotOwner: value.Long("rejectedNotOwner"),
                RejectedNoPeerKey: value.Long("rejectedNoPeerKey"),
                RejectedUnclassified: value.Long("rejectedUnclassified"),
                RejectedNoRole: value.Long("rejectedNoRole"),
                UndecodedReports: value.Long("undecodedReports"),
                Rollover: value.Long("rollover"),
                RoleLosses: value.Long("roleLosses"),
                MapGeneration: value.Long("mapGeneration"),
                Publishes: value.Long("publishes"),
                Recenters: value.Long("recenters"),
                ActiveProfile: value.Int("activeProfile"),
                ActiveProfileName: value.OptionalString("activeProfileName") ?? string.Empty);
        });

    public static KbmProfiles KbmProfiles(string command, string response) =>
        Decode(command, response, value =>
        {
            var hasList =
                value.TryGetProperty("profiles", out var entries) &&
                entries.ValueKind == JsonValueKind.Array;
            RequireShape(hasList && value.TryGetProperty("max", out _), command);

            var profiles = new List<KbmProfileInfo>();
            foreach (var entry in entries.EnumerateArray())
            {
                var layout = KbmLayouts.FromWire(entry.String("layout"));
                var name = entry.OptionalString("name");
                // A profile the adapter cannot name, or names for a layout this
                // build does not know, is skipped rather than shown as an
                // unusable row the user cannot select or remove.
                if (layout is null || string.IsNullOrWhiteSpace(name))
                {
                    continue;
                }

                profiles.Add(new KbmProfileInfo(
                    Id: entry.Int("id"),
                    Layout: layout.Value,
                    Name: name!,
                    Active: entry.Bool("active"),
                    Builtin: entry.Bool("builtin"),
                    Overrides: entry.Int("overrides")));
            }

            return new KbmProfiles(new ValueList<KbmProfileInfo>(profiles),
                                   value.Int("max"));
        });

    public static KbmMapPage KbmMapPage(string command, string response) =>
        Decode(command, response, value =>
        {
            var profile = KbmLayouts.FromWire(value.String("profile"));
            var hasEntries =
                value.TryGetProperty("bindings", out var entries) &&
                entries.ValueKind == JsonValueKind.Array;
            RequireShape(
                profile is not null && hasEntries &&
                value.TryGetProperty("total", out _) &&
                value.TryGetProperty("more", out _),
                command);

            var page = value.Int("page");
            var pageSize = value.Int("pageSize");
            var total = value.Int("total");
            RequireShape(page >= 0 && pageSize > 0 && total >= 0, command);

            var bindings = new List<KbmBinding>();
            foreach (var element in entries.EnumerateArray())
            {
                if (element.ValueKind != JsonValueKind.Object)
                {
                    throw new ArgumentException("'bindings' entries must be objects");
                }

                var source = KbmSource.Parse(element.String("src"));
                var destination = KbmDestinations.FromWire(element.String("dst"));
                RequireShape(source is not null && destination is not null, command);
                bindings.Add(new KbmBinding(source!, destination!.Value, element.Bool("custom")));
            }

            RequireShape(bindings.Count <= pageSize && bindings.Count <= total, command);
            return new Management.KbmMapPage(
                profile!.Value,
                page,
                pageSize,
                total,
                new ValueList<KbmBinding>(bindings),
                value.Bool("more"));
        });

    public static KbmMouseConfig KbmMouse(string command, string response) =>
        Decode(command, response, value =>
        {
            RequireShape(
                value.TryGetProperty("sensitivityX", out _) &&
                value.TryGetProperty("sensitivityY", out _) &&
                value.TryGetProperty("sensitivityMin", out _) &&
                value.TryGetProperty("sensitivityMax", out _) &&
                value.TryGetProperty("antiDeadzoneMax", out _),
                command);

            var config = new KbmMouseConfig(
                SensitivityX: value.Int("sensitivityX"),
                SensitivityY: value.Int("sensitivityY"),
                VelocityWindowMs: value.Int("recenterMs"),
                InvertX: value.Bool("invertX"),
                InvertY: value.Bool("invertY"),
                AntiDeadzone: value.Int("antiDeadzone"),
                SensitivityMin: value.Int("sensitivityMin"),
                SensitivityMax: value.Int("sensitivityMax"),
                VelocityWindowMinMs: value.Int("recenterMinMs"),
                VelocityWindowMaxMs: value.Int("recenterMaxMs"),
                AntiDeadzoneMax: value.Int("antiDeadzoneMax"));

            RequireShape(
                config.SensitivityMax > config.SensitivityMin &&
                config.VelocityWindowMaxMs >= config.VelocityWindowMinMs &&
                config.AntiDeadzoneMax >= 0,
                command);
            return config;
        });

    public static AdapterInputState InputSources(string command, string response) =>
        Decode(command, response, value =>
        {
            if (!value.TryGetProperty("sources", out var array))
            {
                throw Incomplete(command);
            }

            if (array.ValueKind != JsonValueKind.Array)
            {
                throw new ArgumentException("'sources' must be an array");
            }

            var sources = new List<AdapterInputSource>();
            foreach (var element in array.EnumerateArray())
            {
                if (element.ValueKind != JsonValueKind.Object)
                {
                    throw new ArgumentException("'sources' entries must be objects");
                }

                var id = element.RequiredLong("id");
                var connection = element.RequiredInt("conn");
                var transport = element.RequiredInt("transport");
                var generation = element.RequiredLong("generation");
                var name = element.String("name");
                RequireShape(
                    id is >= 1 and <= Uint32Max &&
                    connection is >= 0 and <= 255 &&
                    transport is >= 0 and <= 255 &&
                    generation is >= 0 and <= Uint32Max,
                    command);
                sources.Add(new AdapterInputSource(
                    id,
                    connection,
                    transport,
                    generation,
                    string.IsNullOrWhiteSpace(name) ? "Controller" : name));
            }

            var active = value.RequiredLong("active");
            var pending = value.RequiredLong("pending");
            var transitions = value.RequiredLong("transitions");
            RequireShape(
                active is >= 0 and <= Uint32Max &&
                pending is >= 0 and <= Uint32Max &&
                transitions is >= 0 and <= Uint32Max &&
                sources.Select(source => source.Id).Distinct().Count() == sources.Count,
                command);

            return new AdapterInputState
            {
                ActiveId = active,
                PendingId = pending,
                Explicit = value.RequiredBool("explicit"),
                AwaitingFresh = value.RequiredBool("fresh"),
                Transitions = transitions,
                Sources = new ValueList<AdapterInputSource>(sources),
                Truncated = value.RequiredBool("more"),
            };
        });

    public static BondPage BondsPage(string command, string response) =>
        Decode(command, response, value =>
        {
            RequireShape(value.RequiredInt("v") == BondsProtocolVersion, command);
            var total = value.RequiredInt("total");
            if (!value.TryGetProperty("bonds", out var array) || array.ValueKind != JsonValueKind.Array)
            {
                throw Incomplete(command);
            }

            var next = Cursor(value, command);
            RequireShape(total >= 0 && (next is null || next >= 0), command);

            var entries = ReadBondEntries(array);
            RequireShape(entries.Count <= total && (next is null || entries.Count > 0), command);
            return new BondPage(new ValueList<BondInfo>(entries), total, next);
        });

    /// <summary>
    /// One page of the logical peer inventory.
    ///
    /// The envelope stays at version 1 across the Phase 4 additions. <c>class</c>,
    /// <c>vid</c> and <c>pid</c> are optional fields on an existing shape, so an
    /// older app ignores them and a newer app reads an older adapter's pages
    /// unchanged; bumping the version would break both directions to describe a
    /// change that breaks neither.
    ///
    /// Shape validation is deliberately as strict as the bond pager's, and for
    /// the same reason: a client that follows a cursor it cannot trust either
    /// loops forever or silently drops a peer, and a dropped peer is one the user
    /// cannot see or act on.
    ///
    /// Unknown roles and unknown transport bits are tolerated rather than
    /// rejected. A newer adapter is allowed to know about things this build does
    /// not, and the honest rendering of that is "unknown", not a parse error that
    /// hides every peer on the page.
    /// </summary>
    public static PeerPage PeersPage(string command, string response) =>
        Decode(command, response, value =>
        {
            RequireShape(value.RequiredInt("v") == PeersProtocolVersion, command);
            var total = value.RequiredInt("total");
            if (!value.TryGetProperty("peers", out var array) || array.ValueKind != JsonValueKind.Array)
            {
                throw Incomplete(command);
            }

            var next = Cursor(value, command);
            RequireShape(total >= 0 && (next is null || next >= 0), command);

            var entries = new List<PeerInfo>();
            foreach (var element in array.EnumerateArray())
            {
                if (element.ValueKind != JsonValueKind.Object)
                {
                    throw new ArgumentException("'peers' entries must be objects");
                }

                var id = element.OptionalString("id");
                if (string.IsNullOrWhiteSpace(id))
                {
                    throw Incomplete(command);
                }

                entries.Add(new PeerInfo(
                    Id: id,
                    Address: element.OptionalString("addr") ?? string.Empty,
                    Role: PeerRoles.FromWire(element.OptionalString("role")),
                    Transports: PeerTransportSet.FromMask(element.OptionalInt("tr") ?? 0),
                    Bonded: element.OptionalBool("bonded") ?? false,
                    Connected: element.OptionalBool("conn") ?? false,
                    Name: Blank(element.OptionalString("name")),

                    // Optional on the wire and absent rather than empty when the
                    // adapter has no driver bound, which is every bonded peer that
                    // is not currently connected. Absent must stay distinguishable
                    // from blank: one means "cannot say", the other would be a name.
                    Classification: Blank(element.OptionalString("class")),
                    VendorId: element.OptionalInt("vid") ?? 0,
                    ProductId: element.OptionalInt("pid") ?? 0));
            }

            RequireShape(entries.Count <= total && (next is null || entries.Count > 0), command);

            // A repeated id inside one page means the adapter's own merge failed;
            // accepting it would show one device twice.
            RequireShape(
                entries.Select(entry => entry.Id).Distinct().Count() == entries.Count,
                command);
            return new PeerPage(new ValueList<PeerInfo>(entries), total, next);
        });

    /// <summary>
    /// One forget result.
    ///
    /// Deliberately strict about the verified fields and tolerant about the
    /// outcome vocabulary: a newer adapter may name an outcome this build does
    /// not know, and refusing the whole reply would leave the client unable to
    /// tell whether the delete happened. An unknown outcome still carries
    /// <c>bonded</c>, which is the part that decides what the user is shown.
    /// </summary>
    public static PeerForgetOutcome PeersForget(string command, string response) =>
        Decode(command, response, value =>
        {
            var id = value.OptionalString("id");
            if (string.IsNullOrWhiteSpace(id))
            {
                throw Incomplete(command);
            }

            var stillBonded = value.OptionalBool("bonded") ?? throw Incomplete(command);
            return new PeerForgetOutcome(
                PeerId: id,
                Result: PeerForgetResults.FromWire(value.OptionalString("result")),
                StillBonded: stillBonded,
                Transports: PeerTransportSet.FromMask(value.OptionalInt("tr") ?? 0));
        });

    /// <summary>
    /// One controller-pairing status.
    ///
    /// Tolerant about vocabulary and strict about the operation generation: a
    /// newer adapter may name a state or reason this build does not know, and
    /// rendering that as <c>Unknown</c> is better than refusing a reply that
    /// still carries the generation and the countdown.
    /// </summary>
    public static PairingStatus PairingStatus(string command, string response) =>
        Decode(command, response, value =>
        {
            var operation = value.OptionalLong("op") ?? throw Incomplete(command);
            return new Management.PairingStatus(
                Operation: operation,
                State: PairingStates.FromWire(value.OptionalString("state")),
                Reason: PairingReasons.FromWire(value.OptionalString("reason")),
                RemainingMillis: value.OptionalLong("remaining_ms") ?? 0,
                Candidates: value.OptionalInt("candidates") ?? 0);
        });

    public static BondEnumeration LegacyBonds(string command, string response) =>
        Decode(command, response, value =>
        {
            if (!value.TryGetProperty("bonds", out var array) || array.ValueKind != JsonValueKind.Array)
            {
                throw Incomplete(command);
            }

            return new BondEnumeration(
                new ValueList<BondInfo>(ReadBondEntries(array)),
                Complete: false,
                Total: null);
        });

    public static bool IsVersionedBondResponse(string command, string response) =>
        Decode(command, response, value =>
        {
            var primitive = value.Primitive("v");
            if (primitive is null)
            {
                return false;
            }

            if (primitive.Value.ValueKind != JsonValueKind.Number ||
                !primitive.Value.TryGetInt32(out var version))
            {
                throw new ArgumentException("'v' must be an integer");
            }

            return version == BondsProtocolVersion;
        });

    public static byte[] ReadData(string command, string response) =>
        Decode(command, response, value =>
        {
            var hex = value.String("data");
            if (hex.Length % 2 != 0)
            {
                throw new ManagementProtocolException("Adapter returned odd-length Amiibo data");
            }

            var bytes = new byte[hex.Length / 2];
            for (var index = 0; index < bytes.Length; index++)
            {
                if (!byte.TryParse(
                        hex.AsSpan(index * 2, 2),
                        NumberStyles.HexNumber,
                        CultureInfo.InvariantCulture,
                        out bytes[index]))
                {
                    throw new ManagementProtocolException("Adapter returned non-hex Amiibo data");
                }
            }

            return bytes;
        });

    public static CommandAcknowledgement Acknowledgement(string command, string response) =>
        Decode(command, response, value =>
        {
            if (!value.Bool("ok"))
            {
                throw new ManagementProtocolException(
                    $"Adapter returned an unexpected response for '{command}'");
            }

            var requested = value.OptionalLong("requested");
            RequireShape(requested is null or (>= 0 and <= Uint32Max), command);
            return new CommandAcknowledgement(
                Queued: value.Bool("queued"),
                Switching: value.Bool("switching"),
                Unchanged: value.Bool("unchanged"),
                Reenumerating: value.Bool("reenumerating"),
                Enabled: value.OptionalBool("enabled"),
                Requested: requested);
        });

    private static string? Blank(string? value) => string.IsNullOrWhiteSpace(value) ? null : value;

    private static List<BondInfo> ReadBondEntries(JsonElement array)
    {
        var entries = new List<BondInfo>();
        var position = 0;
        foreach (var element in array.EnumerateArray())
        {
            if (element.ValueKind != JsonValueKind.Object)
            {
                throw new ArgumentException("'bonds' entries must be objects");
            }

            var address = element.TryGetProperty("addr", out _)
                ? element.String("addr")
                : element.TryGetProperty("address", out _)
                    ? element.String("address")
                    : string.Empty;

            entries.Add(new BondInfo(
                Index: element.OptionalInt("i") ?? element.OptionalInt("index") ?? position,
                Address: address,
                Name: element.OptionalString("name"),
                Type: element.OptionalInt("type")));
            position++;
        }

        return entries;
    }

    /// <summary>
    /// The paginated <c>next</c> field, which must be PRESENT and is either an
    /// integer or explicit null. An absent cursor is an incomplete envelope, not
    /// "no more pages" — collapsing the two lets a truncated reply masquerade as
    /// a finished inventory.
    /// </summary>
    private static int? Cursor(JsonElement value, string command)
    {
        if (!value.TryGetProperty("next", out var element))
        {
            throw Incomplete(command);
        }

        return element.ValueKind switch
        {
            JsonValueKind.Null => null,
            JsonValueKind.Number when element.TryGetInt32(out var cursor) => cursor,
            _ => throw Incomplete(command),
        };
    }

    private static RgbColor Color(JsonElement value, string key)
    {
        if (!value.TryGetProperty(key, out var array) || array.ValueKind != JsonValueKind.Array)
        {
            throw new ArgumentException($"'{key}' must be an RGB array");
        }

        if (array.GetArrayLength() != 3)
        {
            throw new ArgumentException($"'{key}' must contain three RGB components");
        }

        var components = new int[3];
        var index = 0;
        foreach (var element in array.EnumerateArray())
        {
            if (element.ValueKind != JsonValueKind.Number || !element.TryGetInt32(out components[index]))
            {
                throw new ArgumentException($"'{key}' components must be integers");
            }

            index++;
        }

        return new RgbColor(components[0], components[1], components[2]);
    }

    private static T Decode<T>(string command, string response, Func<JsonElement, T> block)
    {
        using var document = ObjectOrThrow(command, response);
        try
        {
            return block(document.RootElement);
        }
        catch (ManagementException)
        {
            throw;
        }
        catch (Exception error)
        {
            throw new ManagementProtocolException(
                $"Adapter returned an incomplete response for '{command}'",
                error);
        }
    }

    private static JsonDocument ObjectOrThrow(string command, string response)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(response.Trim());
        }
        catch (Exception error)
        {
            throw new ManagementProtocolException(
                $"Adapter returned malformed JSON for '{command}'",
                error);
        }

        if (document.RootElement.ValueKind != JsonValueKind.Object)
        {
            document.Dispose();
            throw new ManagementProtocolException($"Adapter returned malformed JSON for '{command}'");
        }

        // `error` is checked BEFORE any operation-specific field, and a malformed
        // error or code is itself a protocol error (WINDOWS_PASS.md §13.2 rule 4).
        if (document.RootElement.TryGetProperty("error", out var errorElement))
        {
            if (errorElement.ValueKind != JsonValueKind.String)
            {
                document.Dispose();
                throw new ManagementProtocolException(
                    $"Adapter returned a malformed error for '{command}'");
            }

            var message = errorElement.GetString()!;
            int? code = null;
            if (document.RootElement.TryGetProperty("code", out var codeElement))
            {
                if (codeElement.ValueKind != JsonValueKind.Number ||
                    !codeElement.TryGetInt32(out var parsed))
                {
                    document.Dispose();
                    throw new ManagementProtocolException(
                        $"Adapter returned a malformed error code for '{command}'");
                }

                code = parsed;
            }

            document.Dispose();
            throw new AdapterCommandException(command, code, message);
        }

        return document;
    }

    private static void RequireShape(bool valid, string command)
    {
        if (!valid)
        {
            throw Incomplete(command);
        }
    }

    private static ManagementProtocolException Incomplete(string command) =>
        new($"Adapter returned an incomplete response for '{command}'");
}

/// <summary>
/// Strict JSON field readers.
///
/// Every one checks the value KIND before reading. A JSON string where an
/// integer is expected is an error, never a coercion — WINDOWS_PASS.md §13.2
/// rule 1, and the behaviour the Kotlin private helpers already have.
/// </summary>
internal static class ManagementJsonReader
{
    internal static JsonElement? Primitive(this JsonElement value, string key)
    {
        if (!value.TryGetProperty(key, out var element))
        {
            return null;
        }

        if (element.ValueKind is JsonValueKind.Object or JsonValueKind.Array)
        {
            throw new ArgumentException($"'{key}' must be a JSON primitive");
        }

        return element;
    }

    internal static string String(this JsonElement value, string key, string fallback = "")
    {
        var primitive = value.Primitive(key);
        if (primitive is null)
        {
            return fallback;
        }

        return primitive.Value.ValueKind == JsonValueKind.String
            ? primitive.Value.GetString()!
            : throw new ArgumentException($"'{key}' must be a string");
    }

    internal static int Int(this JsonElement value, string key)
    {
        var primitive = value.Primitive(key);
        if (primitive is null)
        {
            return 0;
        }

        return primitive.Value.ValueKind == JsonValueKind.Number &&
               primitive.Value.TryGetInt32(out var parsed)
            ? parsed
            : throw new ArgumentException($"'{key}' must be an integer");
    }

    internal static long Long(this JsonElement value, string key)
    {
        var primitive = value.Primitive(key);
        if (primitive is null)
        {
            return 0L;
        }

        return primitive.Value.ValueKind == JsonValueKind.Number &&
               primitive.Value.TryGetInt64(out var parsed)
            ? parsed
            : throw new ArgumentException($"'{key}' must be an integer");
    }

    internal static bool Bool(this JsonElement value, string key)
    {
        var primitive = value.Primitive(key);
        if (primitive is null)
        {
            return false;
        }

        return primitive.Value.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => throw new ArgumentException($"'{key}' must be a boolean"),
        };
    }

    /// <summary>
    /// A boolean OR an integer. Used only for <c>batteryValid</c> and
    /// <c>charging</c>, because the firmware emits both forms there. Nowhere
    /// else — WINDOWS_PASS.md §13.2 rule 2.
    /// </summary>
    internal static bool BoolInt(this JsonElement value, string key)
    {
        var primitive = value.Primitive(key);
        if (primitive is null)
        {
            return false;
        }

        return primitive.Value.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            JsonValueKind.Number when primitive.Value.TryGetInt32(out var parsed) => parsed != 0,
            _ => throw new ArgumentException($"'{key}' must be a boolean or integer"),
        };
    }

    internal static int RequiredInt(this JsonElement value, string key) =>
        value.TryGetProperty(key, out _)
            ? value.Int(key)
            : throw new ArgumentException($"Missing '{key}'");

    internal static long RequiredLong(this JsonElement value, string key) =>
        value.TryGetProperty(key, out _)
            ? value.Long(key)
            : throw new ArgumentException($"Missing '{key}'");

    internal static bool RequiredBool(this JsonElement value, string key) =>
        value.TryGetProperty(key, out _)
            ? value.Bool(key)
            : throw new ArgumentException($"Missing '{key}'");

    internal static int? OptionalInt(this JsonElement value, string key)
    {
        if (!value.TryGetProperty(key, out var element) || element.ValueKind == JsonValueKind.Null)
        {
            return null;
        }

        return element.ValueKind == JsonValueKind.Number && element.TryGetInt32(out var parsed)
            ? parsed
            : throw new ArgumentException($"'{key}' must be an integer or null");
    }

    internal static long? OptionalLong(this JsonElement value, string key)
    {
        if (!value.TryGetProperty(key, out var element) || element.ValueKind == JsonValueKind.Null)
        {
            return null;
        }

        return element.ValueKind == JsonValueKind.Number && element.TryGetInt64(out var parsed)
            ? parsed
            : throw new ArgumentException($"'{key}' must be an integer or null");
    }

    internal static string? OptionalString(this JsonElement value, string key)
    {
        if (!value.TryGetProperty(key, out var element) || element.ValueKind == JsonValueKind.Null)
        {
            return null;
        }

        return element.ValueKind == JsonValueKind.String
            ? element.GetString()
            : throw new ArgumentException($"'{key}' must be a string or null");
    }

    internal static bool? OptionalBool(this JsonElement value, string key)
    {
        if (!value.TryGetProperty(key, out var element) || element.ValueKind == JsonValueKind.Null)
        {
            return null;
        }

        return element.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => throw new ArgumentException($"'{key}' must be a boolean or null"),
        };
    }
}
