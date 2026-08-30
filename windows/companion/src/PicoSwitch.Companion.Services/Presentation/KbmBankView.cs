using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// Where a local profile stands relative to the adapter.
/// </summary>
/// <remarks>
/// THREE INDEPENDENT COMPARISONS, not one overloaded boolean:
///
///   local vs resident    — is the adapter's copy the one I have?
///   resident vs runtime  — is the adapter RUNNING what it has stored?
///   local vs runtime     — derived from the two above.
///
/// A single <c>matchesSaved</c> flag could not express "I edited this locally,
/// the adapter still has the old copy, and it is running that old copy" — which
/// is the ordinary state after a local Save and the exact thing the user needs
/// told.
/// </remarks>
public enum KbmLocalState
{
    /// <summary>In the library only. Never sent to this adapter.</summary>
    LocalOnly,

    /// <summary>Assigned to a bank position, and the copies agree.</summary>
    OnAdapter,

    /// <summary>
    /// Assigned, but edited locally since. The adapter still holds the older
    /// content and is unaware of the edit.
    /// </summary>
    AdapterCopyOutOfDate,

    /// <summary>Assigned, in agreement, and what the console is running now.</summary>
    Active,

    /// <summary>
    /// The resident copy was updated but the console has not picked it up.
    ///
    /// Deliberately reachable: assigning into an active position must not mutate
    /// gameplay mid-session, so the realized snapshot is kept until the user
    /// activates. This state is how the page says so.
    /// </summary>
    ResidentUpdatedNotActivated,
}

/// <summary>One row of the "on adapter" bank list.</summary>
public sealed record KbmBankSlot(
    KbmLayout Layout,
    int Position,
    KbmProfileInfo? Resident,
    bool IsRuntime,
    bool IsBoot,
    KbmSource? SwitchKey)
{
    public bool Empty => Resident is null;

    public string PositionLabel => KbmPositions.Label(Position);

    public string ResidentLabel => Resident?.Name ?? "Empty";
}

/// <summary>One row of the library list, with its adapter relationship.</summary>
public sealed record KbmLibraryRow(
    KbmLocalProfile Profile,
    KbmLocalState State,
    int? AssignedPosition)
{
    public string StateLabel => State switch
    {
        KbmLocalState.LocalOnly => "Local only",
        KbmLocalState.OnAdapter =>
            $"On adapter · {KbmPositions.Label(AssignedPosition ?? 0)}",
        KbmLocalState.AdapterCopyOutOfDate =>
            $"{KbmPositions.Label(AssignedPosition ?? 0)} · adapter copy out of date",
        KbmLocalState.Active =>
            $"{KbmPositions.Label(AssignedPosition ?? 0)} · active",
        KbmLocalState.ResidentUpdatedNotActivated =>
            $"{KbmPositions.Label(AssignedPosition ?? 0)} · activate to use changes",
        _ => string.Empty,
    };

    /// <summary>Offered only when there is something to send.</summary>
    public bool CanUpdateAdapterCopy =>
        State == KbmLocalState.AdapterCopyOutOfDate;
}

/// <summary>
/// The bank and library projection.
/// </summary>
public static class KbmBankView
{
    /// <summary>
    /// Every position of a layout's bank, empty ones included.
    /// </summary>
    /// <remarks>
    /// Empty positions are rows rather than omissions: "Profile 3 — Empty" is
    /// what tells a user they have somewhere to assign to, and a list that only
    /// showed occupied positions would hide the capacity entirely.
    /// </remarks>
    public static IReadOnlyList<KbmBankSlot> Bank(
        KeyboardMouseState state, KbmLayout layout)
    {
        var active = state.Profiles.ActiveFor(layout);
        var rows = new List<KbmBankSlot>();

        for (var position = KbmPositions.Default;
             position <= KbmLimits.PositionsPerLayout;
             position++)
        {
            rows.Add(new KbmBankSlot(
                Layout: layout,
                Position: position,
                Resident: position == KbmPositions.Default
                    ? state.Profiles.For(layout).FirstOrDefault(p => p.Builtin)
                    : state.Profiles.At(layout, position),
                IsRuntime: active?.RuntimePosition == position,
                IsBoot: active?.BootPosition == position,
                // The switch key is the same in both layouts by design, so the
                // row shows the key that selects THIS position anywhere.
                SwitchKey: state.Switches
                    .FirstOrDefault(binding => binding.Position == position)?.Source));
        }

        return rows;
    }

    /// <summary>
    /// The library, each row carrying its relationship to the adapter.
    /// </summary>
    public static IReadOnlyList<KbmLibraryRow> Library(
        KbmProfileLibrary library, KeyboardMouseState state, KbmLayout layout)
    {
        var active = state.Profiles.ActiveFor(layout);
        var rows = new List<KbmLibraryRow>();

        // A resident copy belongs to AT MOST ONE library row.
        //
        // Two local profiles can hold identical content — two untouched copies of
        // Default do — and without this both would claim the same resident and
        // each would be reported as "on adapter". Claiming consumes, and the
        // strongest evidence wins: name AND content, then content, then name.
        var unclaimed = state.Profiles.Profiles
            .Where(resident => resident.Layout == layout)
            .ToList();

        var profiles = library.For(layout).ToList();
        var matched = new Dictionary<string, KbmProfileInfo>(StringComparer.Ordinal);

        foreach (var strength in Enumerable.Range(0, 3))
        {
            foreach (var profile in profiles.Where(p => !matched.ContainsKey(p.Id)))
            {
                var resident = unclaimed.FirstOrDefault(candidate => strength switch
                {
                    0 => candidate.Fingerprint == profile.Fingerprint &&
                         Same(candidate.Name, profile.Name),
                    1 => candidate.Fingerprint == profile.Fingerprint,
                    _ => Same(candidate.Name, profile.Name),
                });

                if (resident is not null)
                {
                    matched[profile.Id] = resident;
                    unclaimed.Remove(resident);
                }
            }
        }

        foreach (var profile in profiles)
        {
            if (!matched.TryGetValue(profile.Id, out var resident))
            {
                rows.Add(new KbmLibraryRow(profile, KbmLocalState.LocalOnly, null));
                continue;
            }

            var agrees = resident.Fingerprint == profile.Fingerprint;
            var isRuntime = active?.RuntimePosition == resident.Position;

            var kbmState = !agrees
                ? KbmLocalState.AdapterCopyOutOfDate
                : isRuntime && active?.MatchesSaved == false
                    ? KbmLocalState.ResidentUpdatedNotActivated
                    : isRuntime
                        ? KbmLocalState.Active
                        : KbmLocalState.OnAdapter;

            rows.Add(new KbmLibraryRow(profile, kbmState, resident.Position));
        }

        return rows;
    }

    private static bool Same(string a, string b) =>
        string.Equals(a, b, StringComparison.CurrentCultureIgnoreCase);

    /// <summary>
    /// The four semantic switch actions and whatever key is bound to each.
    /// </summary>
    /// <remarks>
    /// Rendered from the ACTIONS rather than from the bindings, so an unassigned
    /// action still appears as a row the user can bind — a list built from the
    /// bindings alone would hide the three actions they have not set up yet.
    /// </remarks>
    public static IReadOnlyList<(int Position, KbmSource? Key)> SwitchActions(
        KeyboardMouseState state) =>
        [.. KbmPositions.All.Select(position =>
            (position,
             state.Switches.FirstOrDefault(b => b.Position == position)?.Source))];
}
