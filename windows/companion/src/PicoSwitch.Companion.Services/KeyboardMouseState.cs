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
public sealed record KeyboardMouseState
{
    public KbmStatus Status { get; init; } = new();

    public KbmMouseConfig Mouse { get; init; } = new();

    public ValueList<KbmMapping> Mappings { get; init; } = ValueList<KbmMapping>.Empty;

    /// <summary>Whether status has been read at all this session.</summary>
    public bool Loaded { get; init; }

    /// <summary>
    /// Whether the adapter supports the KB/M family at all.
    ///
    /// Separate from <see cref="Loaded"/>: Unknown means the probe did not answer,
    /// Unsupported means the firmware said no. Only the second may hide the
    /// feature.
    /// </summary>
    public CapabilityState Capability { get; init; } = CapabilityState.Unknown;

    public KbmMapping Mapping(KbmProfile profile) =>
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
