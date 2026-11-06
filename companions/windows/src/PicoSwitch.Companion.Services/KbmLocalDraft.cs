using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The editor's working copy of a LOCAL library profile.
/// </summary>
/// <remarks>
/// REPLACES <see cref="KeyboardMouseDraft"/> ON THE EDITING PATH, and the
/// difference is the whole correction: that type is keyed on an ADAPTER profile
/// id and its Save is a staged management transaction. Editing through it made
/// "the profile I have open" and "the profile resident on the adapter" the same
/// object, so creating a profile wrote to flash and saving one changed what the
/// console might run.
///
/// This is keyed on the LOCAL id — a GUID that no adapter has ever seen. It can
/// be created, edited, saved and discarded with nothing connected, and it holds
/// no adapter state at all. Getting content onto the adapter is a separate,
/// explicit assignment.
///
/// <see cref="KeyboardMouseDraft"/> survives for the staged upload itself, which
/// is still exactly the right mechanism for that job.
/// </remarks>
public sealed record KbmLocalDraft
{
    public required string ProfileId { get; init; }

    public required KbmLayout Layout { get; init; }

    public required string Name { get; init; }

    /// <summary>
    /// Sparse overrides, as stored. The full mapping is composed on demand from
    /// <see cref="KbmDefaults"/>, so the draft never has to carry a copy of the
    /// canonical table.
    /// </summary>
    public ValueList<KbmBinding> Overrides { get; init; } = ValueList<KbmBinding>.Empty;

    public KbmMouseConfig Mouse { get; init; } = new();

    // What was saved, so Dirty is a comparison rather than a flag.
    public required string BaseName { get; init; }

    public ValueList<KbmBinding> BaseOverrides { get; init; } =
        ValueList<KbmBinding>.Empty;

    public KbmMouseConfig BaseMouse { get; init; } = new();

    /// <summary>
    /// Editing the built-in template, which is not a library profile.
    /// </summary>
    /// <remarks>
    /// Default is read-only: saving it creates a NEW local profile instead, which
    /// is what keeps the template always available as a starting point.
    /// </remarks>
    public bool IsBuiltin => ProfileId.Length == 0;

    /// <summary>
    /// Compared on CONTENT rather than tracked with a flag, so a user who edits a
    /// key and puts it back is correctly clean again.
    /// </summary>
    public bool Dirty =>
        !string.Equals(Name, BaseName, StringComparison.Ordinal) ||
        !Mouse.Equals(BaseMouse) ||
        Fingerprint != BaseFingerprint;

    public long Fingerprint =>
        KbmFingerprint.Compute(Layout, KbmFingerprint.Canonical(Overrides), Mouse);

    public long BaseFingerprint =>
        KbmFingerprint.Compute(Layout, KbmFingerprint.Canonical(BaseOverrides),
                               BaseMouse);

    /// <summary>The full mapping the grid draws, composed from the defaults.</summary>
    public IReadOnlyList<KbmBinding> Effective =>
        KbmDefaults.Effective(Layout, Overrides);

    /// <summary>A draft on the built-in template of a layout. No adapter needed.</summary>
    public static KbmLocalDraft FromDefault(KbmLayout layout) => new()
    {
        ProfileId = string.Empty,
        Layout = layout,
        Name = "Default",
        BaseName = "Default",
        Mouse = KbmDefaults.For(layout).Mouse,
        BaseMouse = KbmDefaults.For(layout).Mouse,
    };

    public static KbmLocalDraft From(KbmLocalProfile profile) => new()
    {
        ProfileId = profile.Id,
        Layout = profile.Layout,
        Name = profile.Name,
        Overrides = profile.Bindings,
        Mouse = profile.Mouse,
        BaseName = profile.Name,
        BaseOverrides = profile.Bindings,
        BaseMouse = profile.Mouse,
    };

    /// <summary>Rebind one input. Local only; zero adapter traffic.</summary>
    public KbmLocalDraft With(KbmSource source, KbmDestination destination)
    {
        var kept = Overrides.Where(b => !b.Source.Equals(source)).ToList();

        // An override equal to the layout's canonical default is DROPPED rather
        // than stored, so putting a key back really does return the profile to
        // clean. NONE is never dropped: "does nothing" is a real answer that
        // differs from the default.
        var canonical = KbmDefaults.For(Layout).Bindings
            .FirstOrDefault(b => b.Source.Equals(source));
        if (canonical is null || canonical.Destination != destination)
        {
            kept.Add(new KbmBinding(source, destination, Custom: true));
        }

        return this with { Overrides = new ValueList<KbmBinding>(kept) };
    }

    /// <summary>Restore one input to the layout's canonical default.</summary>
    public KbmLocalDraft Restore(KbmSource source) => this with
    {
        Overrides = new ValueList<KbmBinding>(
            Overrides.Where(b => !b.Source.Equals(source))),
    };

    public KbmLocalDraft WithName(string name) => this with { Name = name };

    public KbmLocalDraft WithMouse(KbmMouseConfig mouse) => this with { Mouse = mouse };

    /// <summary>Throw the local edits away. Zero adapter traffic.</summary>
    public KbmLocalDraft Discard() => this with
    {
        Name = BaseName,
        Overrides = BaseOverrides,
        Mouse = BaseMouse,
    };

    /// <summary>Rebase after a local save.</summary>
    public KbmLocalDraft Rebased(KbmLocalProfile saved) => this with
    {
        ProfileId = saved.Id,
        Name = saved.Name,
        Overrides = saved.Bindings,
        Mouse = saved.Mouse,
        BaseName = saved.Name,
        BaseOverrides = saved.Bindings,
        BaseMouse = saved.Mouse,
    };
}
