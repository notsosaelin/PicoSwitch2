using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// Everything the Keyboard and Mouse feature knows about the adapter.
///
/// Held beside <see cref="AdapterSnapshot"/> rather than inside it, deliberately.
/// <c>ManagementRefresh</c> already returns <c>KbmStatus</c> and
/// <c>KbmMouseConfig</c> outside the snapshot, because they are one feature's
/// state rather than the adapter's general condition — and the mappings are
/// heavier still: two profiles, each paged over the wire. Folding all of that
/// into the snapshot would make every dashboard repaint carry a keyboard map.
///
/// Empty is a real state and means "not read yet", which is why
/// <see cref="Loaded"/> exists rather than inferring readiness from a count. A
/// profile with genuinely zero bindings is indistinguishable from one that was
/// never fetched, and only one of those should send the user to a mapping screen.
/// </summary>
/// <summary>
/// The Keyboard and Mouse page's top-level state. Exactly one is true at a time.
/// </summary>
/// <remarks>
/// This is explicit because the implicit version shipped a bad failure. The page
/// previously inferred readiness from a <c>Loaded</c> flag and quietly degraded
/// to a pre-profile editor whenever the profile contract did not answer. When a
/// protocol defect made the read fail, the user was left looking at the old
/// half-working mapping page with no profile controls and no statement that
/// anything had gone wrong — which is indistinguishable from the feature simply
/// not having been built.
///
/// There is no legacy fallback. This companion targets ONE firmware contract.
/// </remarks>
public enum KeyboardMouseReadiness
{
    /// <summary>Never read this session.</summary>
    NotRead,

    /// <summary>Read in progress.</summary>
    Loading,

    /// <summary>The current contract loaded. The only state with a usable page.</summary>
    Ready,

    /// <summary>
    /// The adapter answered, but does not implement a command the current
    /// contract requires. Its firmware predates the profile system.
    /// </summary>
    FirmwareUpdateRequired,

    /// <summary>
    /// The adapter implements the contract but returned data this build could
    /// not use — malformed, incomplete, or inconsistent. A defect, not a version
    /// gap, and <see cref="KeyboardMouseState.Fault"/> says which.
    /// </summary>
    Error,
}

public sealed record KeyboardMouseState
{
    public KbmStatus Status { get; init; } = new();

    public KbmMouseConfig Mouse { get; init; } = new();

    public ValueList<KbmMapping> Mappings { get; init; } = ValueList<KbmMapping>.Empty;

    /// <summary>
    /// The adapter's named profiles. Required by the current contract: an
    /// adapter that cannot list them is reported as needing a firmware update,
    /// never silently treated as having none.
    /// </summary>
    public KbmProfiles Profiles { get; init; } = KbmProfiles.Empty;

    public KeyboardMouseReadiness Readiness { get; init; } =
        KeyboardMouseReadiness.NotRead;

    /// <summary>
    /// Why the page is not Ready, in developer terms. Shown verbatim in the
    /// diagnostics log and summarized on the page; the generic banner it
    /// replaces cost a hardware round trip to turn into a diagnosis.
    /// </summary>
    public string Fault { get; init; } = string.Empty;

    /// <summary>Whether the current contract loaded completely.</summary>
    public bool Loaded => Readiness == KeyboardMouseReadiness.Ready;

    /// <summary>
    /// Whether the adapter supports the KB/M family at all.
    ///
    /// Distinct from <see cref="Readiness"/>: this drives whether the feature is
    /// offered in navigation, while Readiness drives what the page shows once
    /// opened. Unknown means the probe did not answer; Unsupported means the
    /// firmware said no.
    /// </summary>
    public CapabilityState Capability { get; init; } = CapabilityState.Unknown;

    public KbmMapping Mapping(KbmLayout profile) =>
        Mappings.FirstOrDefault(mapping => mapping.Profile == profile)
        ?? new KbmMapping(profile, ValueList<KbmBinding>.Empty, Loaded: false);

    public KeyboardMouseState With(KbmMapping mapping) => this with
    {
        Mappings = new ValueList<KbmMapping>(
            Mappings
                .Where(existing => existing.Profile != mapping.Profile)
                .Append(mapping)
                .OrderBy(existing => existing.Profile)),
    };
}
