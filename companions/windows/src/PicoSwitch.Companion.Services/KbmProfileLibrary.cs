using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// One profile in the APP'S OWN library.
/// </summary>
/// <remarks>
/// THE DISTINCTION THIS TYPE EXISTS TO MAKE.
///
/// The adapter holds six resident profiles — three positions in each of two
/// layout banks — because those must work with no companion attached. That is
/// the adapter's WORKING SET, not the user's collection. The library here is
/// where a user keeps as many profiles as they like; assigning one to a bank
/// position is a separate, explicit act.
///
/// Treating the adapter's capacity as the user's capacity was the original
/// mistake, and it made "Save" mean "write to the adapter", which in turn made
/// every edit a flash write and every rename a management command.
///
/// <see cref="Id"/> is a stable GUID that survives renames and is independent of
/// any adapter identity. Windows and Android libraries are separate and their ids
/// are NOT shared — the resident copy on the adapter is the only bridge between
/// them, which is why <see cref="Fingerprint"/> is the thing compared across
/// platforms rather than an id.
///
/// No transient UI state is stored: no selection, no dirty flag, no draft.
/// </remarks>
public sealed record KbmLocalProfile
{
    public required string Id { get; init; }

    public required KbmLayout Layout { get; init; }

    public required string Name { get; init; }

    /// <summary>
    /// The mapping, as sparse overrides against the layout's canonical default —
    /// the same representation the adapter stores, so an assignment is a copy
    /// rather than a translation.
    /// </summary>
    public ValueList<KbmBinding> Bindings { get; init; } = ValueList<KbmBinding>.Empty;

    /// <summary>Profile-owned tuning. Switching profiles switches this too.</summary>
    public KbmMouseConfig Mouse { get; init; } = new();

    /// <summary>
    /// Deterministic digest of the canonicalized content, computed by the same
    /// rule as the firmware's. This is what answers "is the adapter's copy of
    /// this profile still the one I have?" across platforms.
    /// </summary>
    public long Fingerprint { get; init; }

    public DateTimeOffset Modified { get; init; }
}

/// <summary>
/// The user's local profiles. Deliberately unbounded.
/// </summary>
/// <remarks>
/// There is no six-profile cap here and there must never be one: six is how many
/// the ADAPTER can hold resident, and conflating the two is the defect this
/// replaces. A user with twenty profiles and an adapter holding six of them is
/// the intended shape.
/// </remarks>
public sealed record KbmProfileLibrary
{
    public static readonly KbmProfileLibrary Empty = new();

    /// <summary>Bumped when the on-disk shape changes, so a migration can be written.</summary>
    public const int CurrentVersion = 1;

    public ValueList<KbmLocalProfile> Profiles { get; init; } =
        ValueList<KbmLocalProfile>.Empty;

    public IReadOnlyList<KbmLocalProfile> For(KbmLayout layout) =>
        [.. Profiles.Where(profile => profile.Layout == layout)
                    .OrderBy(profile => profile.Name, StringComparer.CurrentCultureIgnoreCase)];

    public KbmLocalProfile? Find(string id) =>
        Profiles.FirstOrDefault(profile => profile.Id == id);

    public KbmProfileLibrary With(KbmLocalProfile profile) => this with
    {
        Profiles = new ValueList<KbmLocalProfile>(
            Profiles.Where(existing => existing.Id != profile.Id).Append(profile)),
    };

    public KbmProfileLibrary Without(string id) => this with
    {
        Profiles = new ValueList<KbmLocalProfile>(
            Profiles.Where(existing => existing.Id != id)),
    };

    /// <summary>
    /// A name that is not already taken in this layout, so New and Duplicate
    /// never produce two rows a user cannot tell apart.
    /// </summary>
    public string SuggestName(KbmLayout layout, string basis)
    {
        var taken = For(layout).Select(profile => profile.Name)
                               .ToHashSet(StringComparer.CurrentCultureIgnoreCase);
        if (!taken.Contains(basis))
        {
            return basis;
        }

        for (var n = 2; n < 1000; n++)
        {
            var candidate = $"{basis} {n}";
            if (!taken.Contains(candidate))
            {
                return candidate;
            }
        }

        return basis;
    }
}
