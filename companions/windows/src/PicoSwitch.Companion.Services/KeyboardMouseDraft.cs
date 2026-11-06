using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// What the editor is doing, from the user's point of view.
///
/// The three that matter and are easy to conflate:
///
/// <list type="bullet">
/// <item><b>Dirty</b> — edited locally. NOTHING has been sent to the adapter.</item>
/// <item><b>SavedNotApplied</b> — the adapter stored it, and the console is
/// still running the old mapping. This state exists because Save and Apply are
/// different acts, and a UI that cannot show it will lie.</item>
/// <item><b>Active</b> — the realized mapping matches this profile's saved
/// content.</item>
/// </list>
/// </summary>
public enum KbmDraftState
{
    /// <summary>Draft equals the adapter's saved profile, which is not applied.</summary>
    Clean,

    /// <summary>Draft equals the saved profile, and that is what is running.</summary>
    Active,

    /// <summary>Edited locally. Zero adapter writes have happened.</summary>
    Dirty,

    /// <summary>Saved to the library; the console still runs something else.</summary>
    SavedNotApplied,

    /// <summary>The adapter's profile moved on since this draft was based on it.</summary>
    Conflict,

    /// <summary>No live session. Nothing here may be presented as live truth.</summary>
    Disconnected,
}

/// <summary>
/// A local, editable copy of one profile.
///
/// The reason this type exists: the previous editor sent <c>kbm bind</c> on every
/// keystroke, which erased flash once per changed key and made Save and Discard
/// meaningless — there was nothing to discard, because it had already happened.
/// Every edit here is a pure record transformation. Nothing in this file talks to
/// an adapter.
/// </summary>
public sealed record KeyboardMouseDraft
{
    /// <summary>
    /// The profile being edited. <see cref="KbmProfileIds.Default"/> means the
    /// user is looking at the built-in template, which cannot be saved into —
    /// they must create a profile from it first.
    /// </summary>
    public required int ProfileId { get; init; }

    public required KbmLayout Layout { get; init; }

    /// <summary>
    /// The revision this draft was built from. Sent with a save so the adapter
    /// can refuse rather than overwrite work done from another companion.
    /// </summary>
    public required int BaseRevision { get; init; }

    public required string Name { get; init; }

    public required ValueList<KbmBinding> Bindings { get; init; }

    public required KbmMouseConfig Mouse { get; init; }

    /// <summary>The saved content this draft started from, for Discard and dirty checks.</summary>
    public required ValueList<KbmBinding> BaseBindings { get; init; }

    public required KbmMouseConfig BaseMouse { get; init; }

    public required string BaseName { get; init; }

    /// <summary>
    /// True when the draft differs from what the adapter has stored.
    ///
    /// Compared on CONTENT rather than tracked with a flag, so a user who edits
    /// a key and then puts it back is correctly clean again and Save stays
    /// disabled.
    /// </summary>
    public bool Dirty =>
        Name != BaseName ||
        !Mouse.Equals(BaseMouse) ||
        !Canonical(Bindings).SequenceEqual(Canonical(BaseBindings));

    /// <summary>Editing the built-in template. Save must offer "create" instead.</summary>
    public bool IsBuiltin => ProfileId == KbmProfileIds.Default;

    public static KeyboardMouseDraft From(KbmProfileInfo profile,
                                          IReadOnlyList<KbmBinding> bindings,
                                          KbmMouseConfig mouse) =>
        new()
        {
            ProfileId = profile.Id,
            Layout = profile.Layout,
            BaseRevision = profile.Revision,
            Name = profile.Name,
            Bindings = new ValueList<KbmBinding>(bindings),
            Mouse = mouse,
            BaseBindings = new ValueList<KbmBinding>(bindings),
            BaseMouse = mouse,
            BaseName = profile.Name,
        };

    /// <summary>Rebind one input. Zero adapter writes.</summary>
    public KeyboardMouseDraft With(KbmSource source, KbmDestination destination)
    {
        var kept = Bindings.Where(b => !b.Source.Equals(source)).ToList();
        // NONE is a real, storable answer — "this key does nothing" — and is
        // kept rather than dropped, because dropping it would restore the
        // adapter's canonical default instead.
        kept.Add(new KbmBinding(source, destination, Custom: true));
        return this with { Bindings = new ValueList<KbmBinding>(kept) };
    }

    public KeyboardMouseDraft WithName(string name) => this with { Name = name };

    public KeyboardMouseDraft WithMouse(KbmMouseConfig mouse) =>
        this with { Mouse = mouse };

    /// <summary>Throw the local edits away. Zero adapter writes.</summary>
    public KeyboardMouseDraft Discard() => this with
    {
        Name = BaseName,
        Bindings = BaseBindings,
        Mouse = BaseMouse,
    };

    /// <summary>
    /// Adopt what the adapter now reports, after a successful save or a reload.
    /// The draft becomes clean against the new revision.
    /// </summary>
    public KeyboardMouseDraft Rebased(int profileId, int revision, string name) =>
        this with
        {
            ProfileId = profileId,
            BaseRevision = revision,
            Name = name,
            BaseName = name,
            BaseBindings = Bindings,
            BaseMouse = Mouse,
        };

    /// <summary>
    /// Where this draft stands, given what the adapter currently reports.
    ///
    /// Deliberately computed from adapter truth on every call rather than
    /// latched: a cached "Active" flag is exactly the lie this model exists to
    /// prevent, and it goes stale the moment another companion applies
    /// something.
    /// </summary>
    public KbmDraftState StateAgainst(KbmProfiles adapter, bool connected)
    {
        if (!connected)
        {
            return KbmDraftState.Disconnected;
        }

        if (Dirty)
        {
            return KbmDraftState.Dirty;
        }

        var saved = adapter.Find(ProfileId);
        if (!IsBuiltin && saved is not null && saved.Revision != BaseRevision)
        {
            // Someone else saved this profile while this draft was open.
            return KbmDraftState.Conflict;
        }

        var active = adapter.ActiveFor(Layout);
        if (active is null)
        {
            return KbmDraftState.Clean;
        }

        // Both halves matter. The id says which profile produced the realized
        // mapping; MatchesSaved says whether that profile has been edited since.
        // An id match alone is what would let the UI claim "Active" for a
        // profile that was saved and never applied.
        return active.SourceId == ProfileId && active.MatchesSaved
            ? KbmDraftState.Active
            : KbmDraftState.SavedNotApplied;
    }

    private static IEnumerable<KbmBinding> Canonical(
        IEnumerable<KbmBinding> bindings) =>
        bindings
            .OrderBy(b => b.Source.Kind)
            .ThenBy(b => b.Source.Code)
            .Select(b => b with { Custom = true });
}
