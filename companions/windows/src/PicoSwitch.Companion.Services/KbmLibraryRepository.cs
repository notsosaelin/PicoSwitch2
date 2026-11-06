using PicoSwitch.Bridge.Core;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The local profile library: the ONLY thing Save writes to.
/// </summary>
/// <remarks>
/// THE RULE THIS TYPE ENFORCES: nothing here talks to the adapter. Not New, not
/// Duplicate, not Rename, not Delete, not Save, not Discard. It has no
/// <c>ManagementClient</c> and cannot acquire one, which is what makes
/// "zero adapter writes while editing" a structural property rather than a
/// convention someone has to remember.
///
/// Sending a local profile to the adapter is a separate, explicit act that lives
/// on <see cref="AdapterRepository"/>, where the management session is.
/// </remarks>
public sealed class KbmLibraryRepository
{
    private readonly KbmProfileLibraryStore store;
    private readonly StateValue<KbmProfileLibrary> library;

    public KbmLibraryRepository(KbmProfileLibraryStore store)
    {
        this.store = store;
        library = new StateValue<KbmProfileLibrary>(store.Load());
    }

    public IReadOnlyStateValue<KbmProfileLibrary> Library => library;

    public KbmProfileLibrary Value => library.Value;

    /// <summary>Create a profile from a layout's canonical Default.</summary>
    public KbmLocalProfile Create(KbmLayout layout, string name,
                                  IReadOnlyList<KbmBinding>? bindings = null,
                                  KbmMouseConfig? mouse = null)
    {
        var overrides = KbmFingerprint.Canonical(bindings ?? []);
        var tuning = mouse ?? new KbmMouseConfig();
        var profile = new KbmLocalProfile
        {
            // A fresh GUID, never derived from the name or from any adapter
            // identity: renaming must not change identity, and deleting then
            // recreating must not alias the old one.
            Id = Guid.NewGuid().ToString("N"),
            Layout = layout,
            Name = name,
            Bindings = new ValueList<KbmBinding>(overrides),
            Mouse = tuning,
            Fingerprint = KbmFingerprint.Compute(layout, overrides, tuning),
            Modified = DateTimeOffset.UtcNow,
        };

        Commit(library.Value.With(profile));
        return profile;
    }

    /// <summary>
    /// SAVE. Local persistence only, and the whole point of the draft model.
    /// </summary>
    /// <remarks>
    /// This runs while an older copy of the same profile may be resident on the
    /// adapter, and it deliberately leaves that copy alone. The UI reports the
    /// divergence; only an explicit adapter action resolves it. Conflating the
    /// two is what made every keystroke a flash erase.
    /// </remarks>
    public KbmLocalProfile Save(string id, string name,
                                IReadOnlyList<KbmBinding> bindings,
                                KbmMouseConfig mouse)
    {
        var existing = library.Value.Find(id);
        var overrides = KbmFingerprint.Canonical(bindings);
        var layout = existing?.Layout ?? KbmLayout.Keyboard;
        var profile = new KbmLocalProfile
        {
            Id = id,
            Layout = layout,
            Name = name,
            Bindings = new ValueList<KbmBinding>(overrides),
            Mouse = mouse,
            Fingerprint = KbmFingerprint.Compute(layout, overrides, mouse),
            Modified = DateTimeOffset.UtcNow,
        };

        Commit(library.Value.With(profile));
        return profile;
    }

    /// <summary>A copy under a new identity, so edits to it cannot reach the original.</summary>
    public KbmLocalProfile Duplicate(string id, string name)
    {
        var source = library.Value.Find(id)
            ?? throw new InvalidOperationException($"No local profile '{id}'");
        return Create(source.Layout, name, source.Bindings, source.Mouse);
    }

    public KbmLocalProfile? Rename(string id, string name)
    {
        var existing = library.Value.Find(id);
        if (existing is null)
        {
            return null;
        }

        // Identity and fingerprint are untouched: a rename changes no behaviour,
        // so an adapter copy that matched before still matches.
        var renamed = existing with { Name = name, Modified = DateTimeOffset.UtcNow };
        Commit(library.Value.With(renamed));
        return renamed;
    }

    /// <summary>
    /// Remove from the LIBRARY. Any copy resident on the adapter survives.
    /// </summary>
    /// <remarks>
    /// The resident copy is a separate snapshot the adapter owns and may be
    /// running right now. Deleting it here would change console behaviour from a
    /// library operation, which is exactly the coupling this model removes.
    /// </remarks>
    public bool Delete(string id)
    {
        if (library.Value.Find(id) is null)
        {
            return false;
        }

        Commit(library.Value.Without(id));
        return true;
    }

    /// <summary>
    /// Import a profile that is resident on the adapter into this library.
    /// </summary>
    /// <remarks>
    /// The cross-platform bridge: an Android-created profile reaches Windows only
    /// as an adapter resident copy, and vice versa. Local ids are NOT shared
    /// between platforms, so the match is by CONTENT — same layout, same
    /// fingerprint — with the name as a tiebreak. Without that, every reconnect
    /// would add another duplicate.
    /// </remarks>
    public KbmLocalProfile Import(KbmLayout layout, string name,
                                  IReadOnlyList<KbmBinding> bindings,
                                  KbmMouseConfig mouse)
    {
        var overrides = KbmFingerprint.Canonical(bindings);
        var fingerprint = KbmFingerprint.Compute(layout, overrides, mouse);

        var existing = library.Value.For(layout)
            .FirstOrDefault(profile => profile.Fingerprint == fingerprint);
        if (existing is not null)
        {
            return existing;
        }

        return Create(layout, library.Value.SuggestName(layout, name), overrides,
                      mouse);
    }

    private void Commit(KbmProfileLibrary updated)
    {
        library.Set(updated);
        store.Save(updated);
    }
}
