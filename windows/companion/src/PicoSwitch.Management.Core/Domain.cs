namespace PicoSwitch.Management;

/*
 * Portable management domain types.
 *
 * Level 1 reimplementation of
 * `android/companion/management-core/.../management/Domain.kt`
 * (WINDOWS_PASS.md §9.4). These are DOMAIN types: no screen titles, no colour
 * packing, no carrier mechanics. `PeerNaming` stays here because a display-name
 * precedence rule is domain logic, not UI copy — the Kotlin architecture guard
 * makes exactly that distinction and so does its C# twin.
 *
 * Records that carry a collection use `ValueList<T>` so they keep the
 * structural equality the Kotlin data classes have.
 */

public enum Personality
{
    Pro2,
    GameCube,
    JoyConLeft,
    JoyConRight,
    Config,
    Unknown,
}

public static class Personalities
{
    public static string WireName(this Personality value) => value switch
    {
        Personality.Pro2 => "pro2",
        Personality.GameCube => "gc",
        Personality.JoyConLeft => "jcl",
        Personality.JoyConRight => "jcr",
        Personality.Config => "config",
        _ => "unknown",
    };

    public static Personality FromWire(string? value) => value switch
    {
        "pro2" => Personality.Pro2,
        "gc" => Personality.GameCube,
        "jcl" => Personality.JoyConLeft,
        "jcr" => Personality.JoyConRight,
        "config" => Personality.Config,
        _ => Personality.Unknown,
    };
}

public sealed record FirmwareInfo(
    string Id = "",
    string Product = "",
    string Version = "",
    int BridgeContract = 0,
    string Build = "");

public sealed record ControllerInfo(
    string Name = "No controller",
    int Vid = 0,
    int Pid = 0,
    bool BatteryValid = false,
    int BatteryPercent = 0,
    bool Charging = false)
{
    public bool Attached =>
        Vid != 0 || Pid != 0 ||
        (!string.IsNullOrWhiteSpace(Name) && Name != "No controller");
}

public sealed record RgbColor
{
    public RgbColor(int red, int green, int blue)
    {
        if (red is < 0 or > 255 || green is < 0 or > 255 || blue is < 0 or > 255)
        {
            throw new ArgumentOutOfRangeException(nameof(red), "RGB components must be 0..255");
        }

        Red = red;
        Green = green;
        Blue = blue;
    }

    public int Red { get; }

    public int Green { get; }

    public int Blue { get; }

    public string Wire() => $"{Red} {Green} {Blue}";
}

public sealed record AdapterConfig
{
    public static readonly RgbColor Black = new(0, 0, 0);

    public RgbColor BodyColor { get; init; } = Black;

    public RgbColor LeftAccent { get; init; } = Black;

    public RgbColor RightAccent { get; init; } = Black;
}

public sealed record PersonalityState
{
    public Personality Current { get; init; } = Personality.Unknown;

    public ValueList<Personality> Available { get; init; } = ValueList<Personality>.Empty;
}

public sealed record AmiiboUpload(bool Active = false, int Received = 0, int Size = 0);

public sealed record AmiiboStatus
{
    public bool Loaded { get; init; }

    public bool Dirty { get; init; }

    public bool Presented { get; init; }

    public bool V3Loaded { get; init; }

    public bool Persisted { get; init; }

    public bool PersistPending { get; init; }

    public int Size { get; init; }

    public bool Signature { get; init; }

    public bool HasSave2 { get; init; }

    public bool UsingSave2 { get; init; }

    public long Generation { get; init; }

    public string PayloadCrc { get; init; } = "00000000";

    public string Uid { get; init; } = "";

    public string FigureId { get; init; } = "";

    public AmiiboUpload Upload { get; init; } = new();
}

public sealed record BondInfo(int Index, string Address, string? Name = null, int? Type = null);

public sealed record BondPage(ValueList<BondInfo> Entries, int Total, int? Next);

public sealed record BondEnumeration(ValueList<BondInfo> Entries, bool Complete, int? Total = null);

/// <summary>
/// What a stored peer is to the user.
///
/// <c>Unknown</c> is a real answer, not a parse failure. The adapter has no
/// persistent role metadata, so a bond whose owner has not been seen since the
/// adapter booted genuinely cannot be classified — and a controller list that
/// guessed would eventually offer to forget the user's own PC.
///
/// Unrecognised wire values also land here, which is the same statement: this
/// build does not know what that is.
/// </summary>
public enum PeerRole
{
    ManagementCompanion,
    ControllerLink,
    PhysicalController,
    Unknown,
}

public static class PeerRoles
{
    public static string WireName(this PeerRole value) => value switch
    {
        PeerRole.ManagementCompanion => "management",
        PeerRole.ControllerLink => "controller_link",
        PeerRole.PhysicalController => "controller",
        _ => "unknown",
    };

    public static PeerRole FromWire(string? value) => value switch
    {
        "management" => PeerRole.ManagementCompanion,
        "controller_link" => PeerRole.ControllerLink,
        "controller" => PeerRole.PhysicalController,
        _ => PeerRole.Unknown,
    };
}

public enum PeerTransport
{
    Classic = 0x01,
    Le = 0x02,
}

/// <summary>
/// The set of transports one logical peer holds a credential on.
///
/// A value type over the wire mask rather than a <c>HashSet</c> so records that
/// carry it keep VALUE equality. The Kotlin side gets that for free from
/// <c>Set</c> inside a <c>data class</c>; C# reference equality on a mutable set
/// would quietly break both record comparison and anything built on it.
/// </summary>
public readonly record struct PeerTransportSet
{
    private const int KnownMask = (int)PeerTransport.Classic | (int)PeerTransport.Le;

    private PeerTransportSet(int mask) => Mask = mask;

    public int Mask { get; }

    public static PeerTransportSet Empty => default;

    /// <summary>
    /// Unknown transport bits are dropped rather than rejected: a newer adapter
    /// is allowed to know about a transport this build does not, and hiding the
    /// whole peer over an unrecognised bit is worse than describing the bits we
    /// do understand.
    /// </summary>
    public static PeerTransportSet FromMask(int mask) => new(mask & KnownMask);

    public static PeerTransportSet Of(params PeerTransport[] transports)
    {
        var mask = 0;
        foreach (var transport in transports)
        {
            mask |= (int)transport;
        }

        return new PeerTransportSet(mask & KnownMask);
    }

    public bool Contains(PeerTransport transport) => (Mask & (int)transport) != 0;

    public int Count => System.Numerics.BitOperations.PopCount((uint)Mask);

    public bool IsEmpty => Mask == 0;

    public IEnumerable<PeerTransport> Values
    {
        get
        {
            if (Contains(PeerTransport.Classic))
            {
                yield return PeerTransport.Classic;
            }

            if (Contains(PeerTransport.Le))
            {
                yield return PeerTransport.Le;
            }
        }
    }

    public override string ToString() => Mask switch
    {
        0 => "none",
        (int)PeerTransport.Classic => "classic",
        (int)PeerTransport.Le => "le",
        _ => "classic+le",
    };
}

/// <summary>
/// One logical remote device the adapter knows.
///
/// Not a bond row. One peer may hold a Classic link key, an LE bond, or both —
/// the management companion routinely holds both — and <c>Transports</c> is how
/// that is expressed without showing one device twice.
///
/// <c>Id</c> is opaque, stable and firmware-assigned; it is not a database index,
/// because those get reused. <c>Name</c> is whatever the device calls itself.
/// <c>Classification</c> is what the adapter's own driver stack decided the
/// device IS (e.g. <c>Sony DualSense</c>) — derived identity rather than a claim
/// by the device, which is why it outranks <c>Name</c> when labelling a
/// controller. Null classification means the adapter cannot say: a bonded peer
/// that is not connected has no driver bound and therefore never carries one,
/// and that gap is what the app-side history exists to cover.
///
/// Contains no key material and never will: the firmware's peer record has
/// nowhere to put any.
/// </summary>
public sealed record PeerInfo(
    string Id,
    string Address = "",
    PeerRole Role = PeerRole.Unknown,
    PeerTransportSet Transports = default,
    bool Bonded = false,
    bool Connected = false,
    string? Name = null,
    string? Classification = null,
    int VendorId = 0,
    int ProductId = 0)
{
    /// <summary>A peer with entries on both transports, which selective forget must treat as one device.</summary>
    public bool MultiTransport => Transports.Count > 1;

    /// <summary>0/0 means the adapter has no identity for this peer, not that it is device 0000:0000.</summary>
    public bool HasUsbIdentity => VendorId != 0 || ProductId != 0;
}

/// <summary>
/// The display-name hierarchy for one remote device.
///
/// Ordered by how much anyone can actually vouch for the answer:
///
///  1. a user alias, which is the user's own decision and outranks everything;
///  2. the adapter's classification, derived from VID/PID and the HID descriptor
///     rather than supplied by the device;
///  3. the remote-supplied name, which is only ever a claim by the device;
///  4. the USB identity, when the adapter has one but no driver name for it;
///  5. a short identity suffix, so two unnamed devices stay distinguishable.
///
/// The final fallback is deliberately not the bare address. An address rendered
/// where a name belongs reads as a name, and this one is not one.
/// </summary>
public static class PeerNaming
{
    public static string Label(
        string address,
        string? alias = null,
        string? classification = null,
        string? name = null,
        int vendorId = 0,
        int productId = 0)
    {
        if (!string.IsNullOrWhiteSpace(alias))
        {
            return alias;
        }

        if (!string.IsNullOrWhiteSpace(classification))
        {
            return classification;
        }

        if (!string.IsNullOrWhiteSpace(name))
        {
            return name;
        }

        return UsbIdentity(vendorId, productId) ?? $"Controller • {ShortLabel(address)}";
    }

    /// <summary>Four hex characters of the identity address. Presentation only, never identity.</summary>
    public static string ShortLabel(string address)
    {
        var alphanumeric = new string(address.Where(char.IsLetterOrDigit).ToArray());
        var tail = alphanumeric.Length <= 4 ? alphanumeric : alphanumeric[^4..];
        return tail.Length == 0 ? "????" : tail.ToUpperInvariant();
    }

    private static string? UsbIdentity(int vendorId, int productId) =>
        vendorId == 0 && productId == 0 ? null : $"Device {vendorId:X4}:{productId:X4}";
}

/// <summary>
/// What a forget attempt did, as the adapter verified it.
///
/// Three of these are outcomes rather than errors, because "forget" asks for an
/// end state, not for an event.
/// </summary>
public enum PeerForgetResult
{
    /// <summary>A record existed and is gone. The adapter re-enumerated to confirm it.</summary>
    Removed,

    /// <summary>
    /// Nothing to do, and a SUCCESS. A management reply can be lost after the
    /// command already ran, so a retry must not report failure for completed work.
    /// </summary>
    AlreadyAbsent,

    /// <summary>The adapter refused: this peer is its management companion.</summary>
    ManagementPeer,

    /// <summary>The delete ran and the peer still holds a credential. Never smoothed over.</summary>
    Incomplete,

    /// <summary>A result this build does not recognise. Treated as "refresh and look".</summary>
    Unknown,
}

public static class PeerForgetResults
{
    public static string WireName(this PeerForgetResult value) => value switch
    {
        PeerForgetResult.Removed => "removed",
        PeerForgetResult.AlreadyAbsent => "already_absent",
        PeerForgetResult.ManagementPeer => "management_peer",
        PeerForgetResult.Incomplete => "incomplete",
        _ => "unknown",
    };

    public static PeerForgetResult FromWire(string? value) => value switch
    {
        "removed" => PeerForgetResult.Removed,
        "already_absent" => PeerForgetResult.AlreadyAbsent,
        "management_peer" => PeerForgetResult.ManagementPeer,
        "incomplete" => PeerForgetResult.Incomplete,
        _ => PeerForgetResult.Unknown,
    };

    public static bool Succeeded(this PeerForgetResult value) =>
        value is PeerForgetResult.Removed or PeerForgetResult.AlreadyAbsent;
}

/// <summary>
/// The adapter's verified answer to one forget.
///
/// <c>StillBonded</c> is the state the adapter observed AFTER deleting, not what
/// it intended. A client must trust this over its own optimism.
/// </summary>
public sealed record PeerForgetOutcome(
    string PeerId,
    PeerForgetResult Result,
    bool StillBonded,
    PeerTransportSet Transports = default);

/// <summary>
/// Where a remote controller-pairing operation has got to.
///
/// The adapter runs ONE pairing state machine — the same one its own pairing
/// button drives — so these states describe that machine, not a second flow the
/// app owns.
/// </summary>
public enum PairingState
{
    Idle,
    Discovering,
    Connecting,
    Paired,
    TimedOut,
    Cancelled,

    /// <summary>The adapter refused to start; <see cref="PairingStatus.Reason"/> says why.</summary>
    Blocked,
    Unknown,
}

public static class PairingStates
{
    public static string WireName(this PairingState value) => value switch
    {
        PairingState.Idle => "idle",
        PairingState.Discovering => "discovering",
        PairingState.Connecting => "connecting",
        PairingState.Paired => "paired",
        PairingState.TimedOut => "timed_out",
        PairingState.Cancelled => "cancelled",
        PairingState.Blocked => "blocked",
        _ => "unknown",
    };

    public static PairingState FromWire(string? value) => value switch
    {
        "idle" => PairingState.Idle,
        "discovering" => PairingState.Discovering,
        "connecting" => PairingState.Connecting,
        "paired" => PairingState.Paired,
        "timed_out" => PairingState.TimedOut,
        "cancelled" => PairingState.Cancelled,
        "blocked" => PairingState.Blocked,
        _ => PairingState.Unknown,
    };

    /// <summary>Still running, so the app should keep polling.</summary>
    public static bool IsActive(this PairingState value) =>
        value is PairingState.Discovering or PairingState.Connecting;
}

/// <summary>Machine-readable failure causes. The adapter names them; the app words them.</summary>
public enum PairingReason
{
    None,
    NoController,
    ManagementDisabled,
    Busy,
    LockedOut,

    /// <summary>Both security stores are full; the user must forget a controller first.</summary>
    StorageFull,
    Unknown,
}

public static class PairingReasons
{
    public static string WireName(this PairingReason value) => value switch
    {
        PairingReason.None => "none",
        PairingReason.NoController => "no_controller",
        PairingReason.ManagementDisabled => "management_disabled",
        PairingReason.Busy => "busy",
        PairingReason.LockedOut => "locked_out",
        PairingReason.StorageFull => "storage_full",
        _ => "unknown",
    };

    public static PairingReason FromWire(string? value) => value switch
    {
        "none" => PairingReason.None,
        "no_controller" => PairingReason.NoController,
        "management_disabled" => PairingReason.ManagementDisabled,
        "busy" => PairingReason.Busy,
        "locked_out" => PairingReason.LockedOut,
        "storage_full" => PairingReason.StorageFull,
        _ => PairingReason.Unknown,
    };
}

/// <summary>
/// One pairing operation as the adapter reports it.
///
/// <c>Operation</c> is a generation, not a handle. A status arriving for an older
/// operation — after an adapter switch, or a reply the app missed — must never be
/// allowed to describe the current one.
/// </summary>
public sealed record PairingStatus(
    long Operation = 0,
    PairingState State = PairingState.Idle,
    PairingReason Reason = PairingReason.None,
    long RemainingMillis = 0,
    int Candidates = 0)
{
    public bool Active => State.IsActive();
}

public sealed record PeerPage(ValueList<PeerInfo> Entries, int Total, int? Next);

public sealed record PeerInventory
{
    public ValueList<PeerInfo> Peers { get; init; } = ValueList<PeerInfo>.Empty;

    public bool Complete { get; init; }

    public int Total { get; init; }

    /// <summary>What the Controllers page shows. Deliberately excludes this PC in either of its roles.</summary>
    public IReadOnlyList<PeerInfo> Controllers =>
        Peers.Where(peer => peer.Role == PeerRole.PhysicalController).ToList();

    /// <summary>Companion/advanced rows: the management PC, Controller Link, and anything unclassified.</summary>
    public IReadOnlyList<PeerInfo> CompanionsAndUnknown =>
        Peers.Where(peer => peer.Role != PeerRole.PhysicalController).ToList();
}

public sealed record AdapterInputSource(
    long Id,
    int Connection,
    int Transport,
    long Generation,
    string Name);

public sealed record AdapterInputState
{
    public long ActiveId { get; init; }

    public long PendingId { get; init; }

    public bool Explicit { get; init; }

    public bool AwaitingFresh { get; init; }

    public long Transitions { get; init; }

    public ValueList<AdapterInputSource> Sources { get; init; } =
        ValueList<AdapterInputSource>.Empty;

    public bool Truncated { get; init; }

    public AdapterInputSource? ActiveSource =>
        Sources.FirstOrDefault(source => source.Id == ActiveId);
}

public enum CapabilityState
{
    Available,
    Unsupported,
    Unknown,
}

public sealed record AdapterCapabilities(
    CapabilityState Core = CapabilityState.Unknown,
    CapabilityState Personality = CapabilityState.Unknown,
    CapabilityState Colors = CapabilityState.Unknown,
    CapabilityState Amiibo = CapabilityState.Unknown,
    CapabilityState ManagementGate = CapabilityState.Unknown,
    CapabilityState Bonds = CapabilityState.Unknown,
    CapabilityState Peers = CapabilityState.Unknown,
    CapabilityState PeerForget = CapabilityState.Unknown,
    CapabilityState RemotePairing = CapabilityState.Unknown,
    CapabilityState Wake = CapabilityState.Unknown,
    CapabilityState ActiveInput = CapabilityState.Unknown,
    CapabilityState Kbm = CapabilityState.Unknown);

/*
 * `PeerForget` and `RemotePairing` are probed separately from `Peers` on
 * purpose. An adapter can list peers without being able to forget one:
 * `peers list` shipped a phase before `peers forget`. Treating them as one
 * capability would either hide a working list or offer a Forget button that
 * answers `unknown command`, and one missing capability must never hide the
 * whole adapter.
 */

public sealed record AdapterSnapshot
{
    public FirmwareInfo Firmware { get; init; } = new();

    public ControllerInfo Controller { get; init; } = new();

    public PersonalityState Personality { get; init; } = new();

    public AdapterConfig Config { get; init; } = new();

    public AmiiboStatus Amiibo { get; init; } = new();

    public bool? ManagementEnabled { get; init; }

    public ValueList<BondInfo> Bonds { get; init; } = ValueList<BondInfo>.Empty;

    public bool? BondsComplete { get; init; }

    public int? BondsTotal { get; init; }

    /// <summary>Logical peers, as the adapter reports them. Never inferred by the app.</summary>
    public PeerInventory Peers { get; init; } = new();

    public AdapterInputState Input { get; init; } = new();

    public AdapterCapabilities Capabilities { get; init; } = new();

    public long RefreshedAtMillis { get; init; }
}

public sealed record ManagementRefresh(
    AdapterSnapshot Snapshot,
    KbmStatus? KbmStatus = null,
    KbmMouseConfig? KbmMouse = null);

public enum KbmMode
{
    Automatic,
    Controller,
    Keyboard,
    KeyboardMouse,
}

public static class KbmModes
{
    public static string Wire(this KbmMode value) => value switch
    {
        KbmMode.Automatic => "auto",
        KbmMode.Controller => "controller",
        KbmMode.Keyboard => "keyboard",
        _ => "kbmouse",
    };

    /// <summary>Null for an unrecognised value; the parser fails the reply closed.</summary>
    public static KbmMode? FromWire(string? value) => value switch
    {
        "auto" => KbmMode.Automatic,
        "controller" => KbmMode.Controller,
        "keyboard" => KbmMode.Keyboard,
        "kbmouse" => KbmMode.KeyboardMouse,
        _ => null,
    };
}

public enum KbmLayout
{
    Keyboard,
    KeyboardMouse,
}

public static class KbmLayouts
{
    public static readonly ValueList<KbmLayout> All =
        new([KbmLayout.Keyboard, KbmLayout.KeyboardMouse]);

    public static string Wire(this KbmLayout value) =>
        value == KbmLayout.Keyboard ? "kb" : "kbm";

    public static KbmLayout? FromWire(string? value) => value switch
    {
        "kb" => KbmLayout.Keyboard,
        "kbm" => KbmLayout.KeyboardMouse,
        _ => null,
    };
}

/// <summary>
/// One named mapping the user can select, within one layout.
///
/// A profile is NOT a layout. The layout is the shape of the mapping — keyboard,
/// or keyboard and mouse — and is derived from which peer roles are filled. The
/// profile is which mapping of that shape is in use, and it is the user's
/// choice. Collapsing the two is what let a binding be saved into a mapping the
/// adapter was not resolving, report success, and do nothing at the console.
/// </summary>
/// <param name="Builtin">
/// One of the two reserved Default profiles. Editable and resettable, never
/// renameable or deletable — which is what guarantees every layout always has an
/// active profile to fall back to.
/// </param>
public sealed record KbmProfileInfo(
    int Id,
    KbmLayout Layout,
    string Name,
    bool Active,
    bool Builtin,
    int Overrides);

/// <summary>The adapter's profile table, and how many slots it has.</summary>
public sealed record KbmProfiles(
    ValueList<KbmProfileInfo> Profiles,
    int Max)
{
    public static readonly KbmProfiles Empty =
        new(ValueList<KbmProfileInfo>.Empty, 0);

    public bool Full => Profiles.Count >= Max && Max > 0;

    public KbmProfileInfo? ActiveFor(KbmLayout layout) =>
        Profiles.FirstOrDefault(p => p.Layout == layout && p.Active);

    public IEnumerable<KbmProfileInfo> For(KbmLayout layout) =>
        Profiles.Where(p => p.Layout == layout);
}

public sealed record KbmStatus(
    KbmMode Mode = KbmMode.Automatic,
    KbmMode ModeOverride = KbmMode.Automatic,
    KbmLayout Profile = KbmLayout.Keyboard,
    bool KeyboardConnected = false,
    bool MouseConnected = false,
    bool NativeMouseOutput = false,
    int KeyboardConn = 0,
    int MouseConn = 0,

    // The composite's identity as the arbiter sees it. Both are already on the
    // wire; neither was read until the runtime counters needed them.
    //
    // GroupId is what makes a separately paired keyboard and mouse ONE logical
    // owner. SourceId is the handle the adapter resolved for the console slot --
    // zero means the composite does not currently own the console, which is the
    // state RejectedNotOwner counts arriving reports against.
    long GroupId = 0,
    long SourceId = 0,
    long KeyboardReports = 0,
    long MouseReports = 0,
    long RejectedMode = 0,
    long RejectedDuplicate = 0,
    long RejectedNotOwner = 0,

    // Admission outcomes that used to increment nothing at all. A report dropped
    // by one of these left every visible counter unchanged, so a keyboard
    // producing no console input looked identical to a keyboard sending nothing.
    long RejectedNoPeerKey = 0,
    long RejectedUnclassified = 0,
    long RejectedNoRole = 0,

    // Reports that reached the keyboard driver and decoded as neither a keyboard
    // nor a pointer report -- what a mis-parsed report descriptor looks like.
    long UndecodedReports = 0,
    long Rollover = 0,
    long RoleLosses = 0,
    long MapGeneration = 0,
    long Publishes = 0,
    long Recenters = 0,

    // Which named profile the live layout is resolving against. A client that
    // shows a mapping without this cannot say whether what it is showing is the
    // mapping actually in use.
    int ActiveProfile = 0,
    string ActiveProfileName = "")
{
    public bool AnyDeviceConnected => KeyboardConnected || MouseConnected;
}

public enum KbmSourceKind
{
    Key,
    MouseButton,
}

public sealed record KbmSource
{
    public const int KeyUsageMin = 0x04;
    public const int KeyUsageMax = 0xE7;
    public const int MouseButtonMin = 1;
    public const int MouseButtonMax = 5;

    public KbmSource(KbmSourceKind kind, int code)
    {
        var valid = kind == KbmSourceKind.Key
            ? code is >= KeyUsageMin and <= KeyUsageMax
            : code is >= MouseButtonMin and <= MouseButtonMax;
        if (!valid)
        {
            throw new ArgumentOutOfRangeException(nameof(code), $"Not a valid {kind} code: {code}");
        }

        Kind = kind;
        Code = code;
    }

    public KbmSourceKind Kind { get; }

    public int Code { get; }

    public string Wire => Kind == KbmSourceKind.Key ? $"key:{Code:X2}" : $"mouse:{Code}";

    public static KbmSource? Parse(string text)
    {
        var separator = text.IndexOf(':');
        var prefix = (separator < 0 ? text : text[..separator]).ToLowerInvariant();
        var value = separator < 0 ? string.Empty : text[(separator + 1)..];
        switch (prefix)
        {
            case "key":
                return int.TryParse(
                        value,
                        System.Globalization.NumberStyles.HexNumber,
                        System.Globalization.CultureInfo.InvariantCulture,
                        out var usage) &&
                    usage is >= KeyUsageMin and <= KeyUsageMax
                    ? new KbmSource(KbmSourceKind.Key, usage)
                    : null;
            case "mouse":
                return int.TryParse(
                        value,
                        System.Globalization.NumberStyles.Integer,
                        System.Globalization.CultureInfo.InvariantCulture,
                        out var button) &&
                    button is >= MouseButtonMin and <= MouseButtonMax
                    ? new KbmSource(KbmSourceKind.MouseButton, button)
                    : null;
            default:
                return null;
        }
    }
}

public enum KbmDestination
{
    None,
    A, B, X, Y,
    L, R, Zl, Zr, Gl, Gr,
    L3, R3,
    DUp, DDown, DLeft, DRight,
    Minus, Plus, Home, Capture, C,
    LStickUp, LStickDown,
    LStickLeft, LStickRight,
    RStickUp, RStickDown,
    RStickLeft, RStickRight,
}

public static class KbmDestinations
{
    private static readonly (KbmDestination Destination, string Wire)[] Table =
    [
        (KbmDestination.None, "none"),
        (KbmDestination.A, "a"), (KbmDestination.B, "b"),
        (KbmDestination.X, "x"), (KbmDestination.Y, "y"),
        (KbmDestination.L, "l"), (KbmDestination.R, "r"),
        (KbmDestination.Zl, "zl"), (KbmDestination.Zr, "zr"),
        (KbmDestination.Gl, "gl"), (KbmDestination.Gr, "gr"),
        (KbmDestination.L3, "l3"), (KbmDestination.R3, "r3"),
        (KbmDestination.DUp, "dup"), (KbmDestination.DDown, "ddown"),
        (KbmDestination.DLeft, "dleft"), (KbmDestination.DRight, "dright"),
        (KbmDestination.Minus, "minus"), (KbmDestination.Plus, "plus"),
        (KbmDestination.Home, "home"), (KbmDestination.Capture, "capture"),
        (KbmDestination.C, "c"),
        (KbmDestination.LStickUp, "lstick_up"), (KbmDestination.LStickDown, "lstick_down"),
        (KbmDestination.LStickLeft, "lstick_left"), (KbmDestination.LStickRight, "lstick_right"),
        (KbmDestination.RStickUp, "rstick_up"), (KbmDestination.RStickDown, "rstick_down"),
        (KbmDestination.RStickLeft, "rstick_left"), (KbmDestination.RStickRight, "rstick_right"),
    ];

    public static string Wire(this KbmDestination value)
    {
        foreach (var (destination, wire) in Table)
        {
            if (destination == value)
            {
                return wire;
            }
        }

        throw new ArgumentOutOfRangeException(nameof(value));
    }

    public static KbmDestination? FromWire(string? value)
    {
        foreach (var (destination, wire) in Table)
        {
            if (wire == value)
            {
                return destination;
            }
        }

        return null;
    }
}

public sealed record KbmBinding(KbmSource Source, KbmDestination Destination, bool Custom);

public sealed record KbmMapping(
    KbmLayout Profile,
    ValueList<KbmBinding> Bindings,
    bool Loaded = false)
{
    public static KbmMapping Empty(KbmLayout profile) =>
        new(profile, ValueList<KbmBinding>.Empty);

    public IReadOnlyList<KbmBinding> KeyBindings =>
        Bindings.Where(binding => binding.Source.Kind == KbmSourceKind.Key).ToList();

    public IReadOnlyList<KbmBinding> MouseBindings =>
        Bindings.Where(binding => binding.Source.Kind == KbmSourceKind.MouseButton).ToList();

    public int CustomCount => Bindings.Count(binding => binding.Custom);
}

public sealed record KbmMapPage(
    KbmLayout Profile,
    int Page,
    int PageSize,
    int Total,
    ValueList<KbmBinding> Bindings,
    bool More);

public sealed record KbmMouseConfig(
    int SensitivityX = 0,
    int SensitivityY = 0,
    int VelocityWindowMs = 0,
    bool InvertX = false,
    bool InvertY = false,
    int AntiDeadzone = 0,
    int SensitivityMin = 0,
    int SensitivityMax = 0,
    int VelocityWindowMinMs = 0,
    int VelocityWindowMaxMs = 0,
    int AntiDeadzoneMax = 0)
{
    public bool AxesLinked => SensitivityX == SensitivityY;

    public double Multiplier(int raw) => raw / 256.0;

    public bool Ranged => SensitivityMax > SensitivityMin;
}

public enum KbmMouseField
{
    Sensitivity,
    SensitivityX,
    SensitivityY,
    VelocityWindow,
    InvertX,
    InvertY,
    AntiDeadzone,
}

public static class KbmMouseFields
{
    public static string Wire(this KbmMouseField value) => value switch
    {
        KbmMouseField.Sensitivity => "sensitivity",
        KbmMouseField.SensitivityX => "sensitivityx",
        KbmMouseField.SensitivityY => "sensitivityy",
        KbmMouseField.VelocityWindow => "recenter",
        KbmMouseField.InvertX => "invertx",
        KbmMouseField.InvertY => "inverty",
        _ => "antideadzone",
    };
}

public enum WakeResult
{
    Pending,
    Advertised,
    ConsoleAwake,
    NoIdentity,
    RadioBusy,
    Unknown,
}

public sealed record WakeStatus(
    WakeResult Result,
    bool ConsoleAsleep,
    bool IdentityValid,
    long Attempts,
    long LastAttemptMs = 0);

public sealed record CommandAcknowledgement(
    bool Queued = false,
    bool Switching = false,
    bool Unchanged = false,
    bool Reenumerating = false,
    bool? Enabled = null,
    long? Requested = null);

public enum PersistenceState
{
    Accepted,
    Queued,
}

public sealed record PersistenceAcknowledgement(PersistenceState State, long? RequestId = null);

public sealed record PersistenceStatus(bool Pending, long Requested, long Completed);

public sealed record AmiiboDownload(byte[] Bytes, long Generation, string? PayloadCrc);

public enum ColorTarget
{
    Body,
    LeftAccent,
    RightAccent,
}

public static class ColorTargets
{
    public static string Command(this ColorTarget value) => value switch
    {
        ColorTarget.Body => "body",
        ColorTarget.LeftAccent => "jcl",
        _ => "jcr",
    };
}
