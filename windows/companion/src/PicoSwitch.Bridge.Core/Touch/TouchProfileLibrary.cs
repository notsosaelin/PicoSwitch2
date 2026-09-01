using System.Text.RegularExpressions;

namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// A named layout for one console-facing controller.
///
/// <code>
/// TouchProfileId (personality)
///        |
///        v
/// TouchLayoutTemplate           immutable catalog + authored default
///        |
///        v
/// TouchLayoutProfile.Document   the user's own scene of instances
///        |
///        v
/// TouchLayoutComposer -&gt; TouchLayoutResolver -&gt; ResolvedTouchLayout
/// </code>
///
/// A profile is an ENVELOPE around a <see cref="TouchLayoutDocument"/>, not a second
/// layout representation. The personality, template identity and template revision it
/// was authored against are read back off that document rather than stored again
/// beside it: two copies of the same fact drift, and the composer already refuses a
/// document whose template identity does not match the shipped one.
/// </summary>
public sealed record TouchLayoutProfile(
    string Id,
    string Name,
    /// <summary>The user's layout. For the factory profile this is the authored default.</summary>
    TouchLayoutDocument Document)
{
    public TouchProfileMetadata Metadata { get; init; } = new();

    public TouchProfileId Personality => Document.ProfileId;

    public string TemplateId => Document.TemplateId;

    public int TemplateRevision => Document.BasedOnRevision;

    /// <summary>
    /// Factory profiles are the shipped defaults.
    ///
    /// Identified structurally by <see cref="TouchProfileLibrary.FactoryProfileId"/>
    /// rather than by a mutable flag, because the protection this identity carries —
    /// cannot be renamed, overwritten or deleted — must not be something a stored
    /// document can turn off.
    /// </summary>
    public bool IsFactory => Id == TouchProfileLibrary.FactoryProfileId;

    /// <summary>True when the profile still describes exactly the shipped arrangement.</summary>
    public bool IsPristine => Document.Controls.SequenceEqual(
        TouchLayoutDocument.AuthoredDefault(TouchProfileCatalog.Require(Personality)).Controls);
}

/// <summary>
/// Bookkeeping that is not layout.
///
/// <see cref="GameKey"/> is reserved for the per-game profiles named as future work in
/// the editor design. Nothing in this build writes or reads it for selection; it exists
/// so that adding automatic selection later does not require a schema migration of
/// every stored profile.
/// </summary>
public sealed record TouchProfileMetadata(
    long CreatedAtEpochMs = 0L,
    long UpdatedAtEpochMs = 0L,
    string? GameKey = null);

/// <summary>
/// Every profile available for one personality, plus which one is active.
///
/// The factory profile is NOT a member of <see cref="UserProfiles"/> and is never
/// persisted. It is synthesized from the shipped template on every read, which is what
/// makes "cannot be overwritten, cannot be deleted, always available" a property of the
/// type instead of a rule some call site has to remember. A corrupt or truncated stored
/// document therefore degrades to the shipped controller rather than to a controller
/// with no layout at all.
/// </summary>
public sealed record TouchProfileLibrary
{
    /// <summary>Reserved id; a stored user profile may never claim it.</summary>
    public const string FactoryProfileId = "factory-default";

    public const string FactoryProfileName = "Default";

    /// <summary>
    /// Enough for the per-game sets this architecture anticipates, small enough that the
    /// profile picker never becomes a scrolling list on a phone in landscape.
    /// </summary>
    public const int MaxUserProfiles = 12;

    public const int MaxNameLength = 32;

    public TouchProfileLibrary(TouchProfileId personality)
    {
        Personality = personality;
    }

    public TouchProfileId Personality { get; init; }

    public IReadOnlyList<TouchLayoutProfile> UserProfiles { get; init; } = [];

    public string SelectedProfileId { get; init; } = FactoryProfileId;

    public TouchLayoutProfile FactoryProfile => new(
        FactoryProfileId,
        FactoryProfileName,
        TouchLayoutDocument.AuthoredDefault(TouchProfileCatalog.Require(Personality)));

    /// <summary>Factory first, then user profiles in creation order.</summary>
    public IReadOnlyList<TouchLayoutProfile> Profiles => [FactoryProfile, .. UserProfiles];

    public TouchLayoutProfile Selected =>
        Profiles.FirstOrDefault(profile => profile.Id == SelectedProfileId) ?? FactoryProfile;

    /// <summary>The layout the runtime should compose. Always a real document.</summary>
    public TouchLayoutDocument ActiveDocument => Selected.Document;

    public TouchLayoutProfile? Profile(string id) =>
        Profiles.FirstOrDefault(profile => profile.Id == id);

    public static TouchProfileLibrary Empty(TouchProfileId personality) => new(personality);

    public bool Equals(TouchProfileLibrary? other) =>
        other is not null &&
        Personality == other.Personality &&
        string.Equals(SelectedProfileId, other.SelectedProfileId, StringComparison.Ordinal) &&
        UserProfiles.SequenceEqual(other.UserProfiles);

    public override int GetHashCode()
    {
        var hash = new HashCode();
        hash.Add(Personality);
        hash.Add(SelectedProfileId, StringComparer.Ordinal);
        foreach (var profile in UserProfiles)
        {
            hash.Add(profile);
        }

        return hash.ToHashCode();
    }
}

/// <summary>Outcome of a library edit; a rejection explains itself rather than silently no-op'ing.</summary>
public abstract record TouchProfileEdit
{
    private TouchProfileEdit()
    {
    }

    public sealed record Applied(TouchProfileLibrary Library, string ProfileId) : TouchProfileEdit;

    public sealed record Rejected(string Reason) : TouchProfileEdit;
}

/// <summary>
/// Pure profile-library operations, shared by every host that has an editor.
///
/// Kept beside <see cref="TouchLayoutEditor"/> and in the same platform-neutral module
/// for the same reason: profile protection, naming and identity are rules about the
/// user's data, and rules that live in a UI layer are rules that hold only on the
/// platform where somebody happened to write them.
/// </summary>
public static partial class TouchProfileLibraryEditor
{
    public const string DefaultNewProfileName = "Custom";

    public const string LegacyProfileName = "My layout";

    private const string FactoryProtected = "The default layout cannot be changed";

    public static TouchProfileEdit Select(TouchProfileLibrary library, string profileId) =>
        library.Profile(profileId) is null
            ? new TouchProfileEdit.Rejected("That layout profile no longer exists")
            : new TouchProfileEdit.Applied(
                library with { SelectedProfileId = profileId }, profileId);

    /// <summary>A new profile: identical to the shipped default until it is edited.</summary>
    public static TouchProfileEdit Create(
        TouchProfileLibrary library, string name, long nowEpochMs) =>
        Insert(
            library,
            name,
            TouchLayoutDocument.AuthoredDefault(TouchProfileCatalog.Require(library.Personality)),
            nowEpochMs);

    /// <summary>
    /// Copy an existing profile, including the factory one.
    ///
    /// Duplicating the factory profile is how a user starts from the official layout
    /// without endangering it, so it is explicitly allowed even though the source itself
    /// can never be written.
    /// </summary>
    public static TouchProfileEdit Duplicate(
        TouchProfileLibrary library, string sourceId, long nowEpochMs, string? name = null)
    {
        var source = library.Profile(sourceId);
        return source is null
            ? new TouchProfileEdit.Rejected("That layout profile no longer exists")
            : Insert(library, name ?? source.Name, source.Document, nowEpochMs);
    }

    public static TouchProfileEdit Rename(
        TouchProfileLibrary library, string profileId, string name)
    {
        var target = library.UserProfiles.FirstOrDefault(profile => profile.Id == profileId);
        if (target is null)
        {
            return new TouchProfileEdit.Rejected(
                profileId == TouchProfileLibrary.FactoryProfileId
                    ? FactoryProtected
                    : "That layout profile no longer exists");
        }

        var clean = UniqueName(SanitizeName(name), library.UserProfiles, exceptId: profileId);
        return new TouchProfileEdit.Applied(
            library with
            {
                UserProfiles = library.UserProfiles
                    .Select(profile => profile.Id == profileId
                        ? profile with { Name = clean }
                        : profile)
                    .ToList(),
            },
            target.Id);
    }

    public static TouchProfileEdit Delete(TouchProfileLibrary library, string profileId)
    {
        if (profileId == TouchProfileLibrary.FactoryProfileId)
        {
            return new TouchProfileEdit.Rejected(FactoryProtected);
        }

        if (!library.UserProfiles.Any(profile => profile.Id == profileId))
        {
            return new TouchProfileEdit.Rejected("That layout profile no longer exists");
        }

        var remaining = library.UserProfiles.Where(profile => profile.Id != profileId).ToList();

        // Deleting the active profile must land somewhere that certainly exists. The
        // factory profile is the only such place.
        var selected = library.SelectedProfileId == profileId
            ? TouchProfileLibrary.FactoryProfileId
            : library.SelectedProfileId;

        return new TouchProfileEdit.Applied(
            library with { UserProfiles = remaining, SelectedProfileId = selected },
            selected);
    }

    /// <summary>
    /// Store an edited layout into a profile.
    ///
    /// Saving onto the factory profile does not fail and does not overwrite it: it
    /// creates a new user profile carrying the edit and selects that. Refusing outright
    /// would mean discarding work the user just did, and overwriting would destroy the
    /// one layout that is always supposed to be recoverable.
    /// </summary>
    public static TouchProfileEdit Save(
        TouchProfileLibrary library,
        string profileId,
        TouchLayoutDocument document,
        long nowEpochMs,
        string newProfileName = DefaultNewProfileName)
    {
        var template = TouchProfileCatalog.Require(library.Personality).DefaultTemplate;
        if (document.ProfileId != library.Personality || document.TemplateId != template.Id)
        {
            return new TouchProfileEdit.Rejected("That layout belongs to another controller");
        }

        if (profileId == TouchProfileLibrary.FactoryProfileId)
        {
            return Insert(library, newProfileName, document, nowEpochMs);
        }

        var target = library.UserProfiles.FirstOrDefault(profile => profile.Id == profileId);
        if (target is null)
        {
            return new TouchProfileEdit.Rejected("That layout profile no longer exists");
        }

        return new TouchProfileEdit.Applied(
            library with
            {
                UserProfiles = library.UserProfiles
                    .Select(profile => profile.Id != profileId
                        ? profile
                        : profile with
                        {
                            Document = document,
                            Metadata = profile.Metadata with { UpdatedAtEpochMs = nowEpochMs },
                        })
                    .ToList(),
                SelectedProfileId = profileId,
            },
            target.Id);
    }

    /// <summary>
    /// Put a user profile's layout back to the shipped arrangement.
    ///
    /// On the factory profile this is already true, so it succeeds and changes nothing —
    /// "Reset to default" should never report an error.
    /// </summary>
    public static TouchProfileEdit ResetToDefault(
        TouchProfileLibrary library, string profileId, long nowEpochMs)
    {
        if (profileId == TouchProfileLibrary.FactoryProfileId)
        {
            return new TouchProfileEdit.Applied(library, profileId);
        }

        var authored = TouchLayoutDocument.AuthoredDefault(
            TouchProfileCatalog.Require(library.Personality));
        return Save(library, profileId, authored, nowEpochMs);
    }

    /// <summary>Adopt an imported document as a new profile of this personality.</summary>
    public static TouchProfileEdit Import(
        TouchProfileLibrary library, TouchLayoutProfile profile, long nowEpochMs) =>
        profile.Personality != library.Personality
            ? new TouchProfileEdit.Rejected("That layout was exported for another controller")
            : Insert(library, profile.Name, profile.Document, nowEpochMs);

    /// <summary>
    /// Adopt the single pre-profile override document as a user profile.
    ///
    /// The first release stored exactly one sparse override per personality with no name
    /// and no identity. Discarding it on upgrade would silently throw away every layout
    /// anybody had already tuned, so it is migrated to an instance document, becomes a
    /// normal profile and is selected — which is what the user last saw.
    /// </summary>
    public static TouchProfileLibrary AdoptLegacyOverride(
        TouchProfileId personality,
        TouchLayoutOverride @override,
        long nowEpochMs,
        string name = LegacyProfileName)
    {
        var library = TouchProfileLibrary.Empty(personality);
        if (@override.Controls.Count == 0 || @override.ProfileId != personality)
        {
            return library;
        }

        var profile = TouchProfileCatalog.Require(personality);
        var document = TouchLayoutMigration.FromOverride(profile, @override);

        // A migration that produced exactly the shipped layout is not worth a profile of
        // its own: the override said nothing the default does not.
        if (document.Controls.SequenceEqual(
                TouchLayoutDocument.AuthoredDefault(profile).Controls))
        {
            return library;
        }

        return Insert(library, name, document, nowEpochMs) is TouchProfileEdit.Applied applied
            ? applied.Library
            : library;
    }

    private static TouchProfileEdit Insert(
        TouchProfileLibrary library,
        string name,
        TouchLayoutDocument document,
        long nowEpochMs)
    {
        if (library.UserProfiles.Count >= TouchProfileLibrary.MaxUserProfiles)
        {
            return new TouchProfileEdit.Rejected(
                $"This controller already has {TouchProfileLibrary.MaxUserProfiles} layout profiles");
        }

        var id = AllocateId(library, nowEpochMs);
        var profile = new TouchLayoutProfile(
            id,
            UniqueName(SanitizeName(name), library.UserProfiles, exceptId: null),
            document)
        {
            Metadata = new TouchProfileMetadata(nowEpochMs, nowEpochMs),
        };

        return new TouchProfileEdit.Applied(
            library with
            {
                UserProfiles = [.. library.UserProfiles, profile],
                SelectedProfileId = id,
            },
            id);
    }

    /// <summary>
    /// Ids are derived, not random.
    ///
    /// A pure function of the library and the clock keeps every operation here testable
    /// without injecting a generator, and the collision suffix means two profiles created
    /// inside the same millisecond still get distinct identities.
    /// </summary>
    internal static string AllocateId(TouchProfileLibrary library, long nowEpochMs)
    {
        var @base = "p" + Base36(Math.Max(nowEpochMs, 0L));
        var taken = library.Profiles.Select(p => p.Id).ToHashSet(StringComparer.Ordinal);
        if (!taken.Contains(@base))
        {
            return @base;
        }

        var suffix = 2;
        while (taken.Contains($"{@base}-{suffix}"))
        {
            suffix++;
        }

        return $"{@base}-{suffix}";
    }

    /// <summary>
    /// Base 36, lower case — the same rendering Kotlin's <c>Long.toString(36)</c>
    /// produces, so an id generated on either client reads the same way.
    /// </summary>
    private static string Base36(long value)
    {
        if (value == 0L)
        {
            return "0";
        }

        const string Digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        var buffer = new Stack<char>();
        while (value > 0L)
        {
            buffer.Push(Digits[(int)(value % 36)]);
            value /= 36;
        }

        return new string([.. buffer]);
    }

    internal static string SanitizeName(string raw)
    {
        var collapsed = Whitespace().Replace(raw.Trim(), " ");
        var filtered = new string(collapsed
            .Where(character => char.IsLetterOrDigit(character) || character == ' ' ||
                                "-_()+.".Contains(character, StringComparison.Ordinal))
            .Take(TouchProfileLibrary.MaxNameLength)
            .ToArray())
            .Trim();

        return string.IsNullOrWhiteSpace(filtered) ? DefaultNewProfileName : filtered;
    }

    /// <summary>
    /// Names are for the user's benefit, so two identical ones are a defect even though
    /// ids stay unique. The factory name is reserved as well: a second "Default" in the
    /// picker would make the protected profile unidentifiable.
    /// </summary>
    internal static string UniqueName(
        string candidate, IReadOnlyList<TouchLayoutProfile> existing, string? exceptId)
    {
        var taken = existing
            .Where(profile => profile.Id != exceptId)
            .Select(profile => profile.Name)
            .Append(TouchProfileLibrary.FactoryProfileName)
            .ToHashSet(StringComparer.Ordinal);

        if (!taken.Contains(candidate))
        {
            return candidate;
        }

        var suffix = 2;
        while (true)
        {
            var room = TouchProfileLibrary.MaxNameLength - $" {suffix}".Length;
            var next = candidate[..Math.Min(room, candidate.Length)].Trim() + $" {suffix}";
            if (!taken.Contains(next))
            {
                return next;
            }

            suffix++;
        }
    }

    [GeneratedRegex(@"\s+")]
    private static partial Regex Whitespace();
}

/// <summary>
/// Storage boundary implemented by each host.
///
/// A document that cannot be understood is reported, never deleted: a later build may
/// understand it, and the runtime is safe in the meantime because the factory profile
/// needs nothing from storage.
/// </summary>
public interface ITouchProfileLibraryStore
{
    TouchProfileLibraryLoad Load(TouchProfileId personality);

    void Save(TouchProfileLibrary library);
}

/// <summary>What a host's storage produced, and what to tell the user if it was not usable.</summary>
public sealed record TouchProfileLibraryLoad(
    TouchProfileLibrary Library,
    string? Warning = null);
