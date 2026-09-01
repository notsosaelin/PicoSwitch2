using PicoSwitch.Bridge.Touch;

namespace PicoSwitch.Companion.Windows.Storage;

/// <summary>
/// The touch profile library on disk, one document per personality.
///
/// ## Why per personality
///
/// A Pro Controller 2 layout and a GameCube layout describe different catalogs and
/// cannot be merged, so one file per personality means a damaged GameCube document
/// costs the user nothing on Pro Controller 2. The alternative — one document holding
/// all four — makes every read an all-or-nothing bet on the least-used personality.
///
/// ## The rule this store exists to keep
///
/// **A document that cannot be understood is REPORTED, never deleted.** A later build
/// may understand it, the user may have hand-edited it, and a store that tidied it away
/// would destroy the only copy at exactly the moment somebody wanted it back. The
/// runtime is safe regardless, because the factory profile is synthesized from the
/// shipped template and needs nothing from storage.
///
/// That is why <see cref="Load"/> returns a warning alongside a usable library rather
/// than throwing: the caller gets a controller that works AND the sentence explaining
/// what happened to the one that was stored.
/// </summary>
public sealed class WindowsTouchProfileStore(WindowsDocumentStore documents)
    : ITouchProfileLibraryStore
{
    /// <summary>
    /// One document per personality, named by the profile's own wire key.
    ///
    /// The wire key rather than the enum name: it is already the stable identifier the
    /// protocol and both companions use, and a rename of the C# enum must not orphan
    /// every stored layout.
    /// </summary>
    public static string DocumentName(TouchProfileId personality) =>
        $"touch-profiles-{personality.Key()}.json";

    public TouchProfileLibraryLoad Load(TouchProfileId personality)
    {
        var raw = documents.Read(DocumentName(personality));
        if (raw is null)
        {
            // No document is not a fault. A user who has never opened the editor has no
            // profiles, and the factory one is always there.
            return new TouchProfileLibraryLoad(TouchProfileLibrary.Empty(personality));
        }

        return TouchProfileLibraryJsonCodec.Decode(raw, personality) switch
        {
            TouchProfileLibraryDecodeResult.Valid valid =>
                new TouchProfileLibraryLoad(valid.Value),

            TouchProfileLibraryDecodeResult.Invalid invalid =>
                // The stored file stays exactly where it is.
                new TouchProfileLibraryLoad(
                    TouchProfileLibrary.Empty(personality),
                    $"{invalid.Problem}. Your saved layouts were left untouched; " +
                    "the default layout is in use."),

            _ => new TouchProfileLibraryLoad(TouchProfileLibrary.Empty(personality)),
        };
    }

    public void Save(TouchProfileLibrary library) =>
        documents.Write(
            DocumentName(library.Personality),
            TouchProfileLibraryJsonCodec.Encode(library));

    /// <summary>
    /// Whether a stored document exists for this personality.
    ///
    /// Used by the one-time legacy adoption: a library document that already exists means
    /// the upgrade has happened, and re-adopting the old override would add a second copy
    /// of the same layout on every launch.
    /// </summary>
    public bool Exists(TouchProfileId personality) =>
        documents.Read(DocumentName(personality)) is not null;
}

/// <summary>
/// Read-only access to whatever a pre-2.0 build left behind.
///
/// No write side, deliberately: schema 1 is a migration SOURCE, and a store that could
/// still write it would eventually be written to.
///
/// The Android companion kept these in <c>SharedPreferences</c>; here they are ordinary
/// JSON documents beside everything else, which is the whole of the platform difference.
/// </summary>
public sealed class WindowsTouchOverrideStore(WindowsDocumentStore documents)
    : ITouchLayoutOverrideStore
{
    public static string DocumentName(TouchProfileId personality) =>
        $"touch-layout-{personality.Key()}.json";

    /// <summary>
    /// The stored override, or null when there is none.
    ///
    /// An invalid document comes back as
    /// <see cref="TouchOverrideDecodeResult.Invalid"/> rather than as null, so the caller
    /// can tell "nothing was ever saved" from "something was saved and this build cannot
    /// read it" — the second is worth a sentence to the user, the first is not.
    /// </summary>
    public TouchOverrideDecodeResult? Load(TouchProfileId personality)
    {
        var raw = documents.Read(DocumentName(personality));
        return raw is null ? null : TouchLayoutOverrideJsonCodec.Decode(raw);
    }
}
