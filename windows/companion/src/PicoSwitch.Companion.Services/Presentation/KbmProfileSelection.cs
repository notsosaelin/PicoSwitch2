using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// What the profile picker should contain and which row should be selected.
/// </summary>
/// <param name="Rebuild">
/// Whether the item collection actually has to be replaced. False is the common
/// case and is the point of this type: see the remarks on
/// <see cref="KbmProfileSelection"/>.
/// </param>
public sealed record KbmProfileSelectionPlan(
    IReadOnlyList<KbmProfileInfo> Rows,
    int SelectedId,
    int SelectedIndex,
    bool Rebuild);

/// <summary>
/// The profile picker's state machine, extracted from the page.
/// </summary>
/// <remarks>
/// THIS EXISTS BECAUSE THE PAGE CRASHED.
///
/// Windows Error Reporting recorded <c>PicoSwitch.Companion.App.exe</c> faulting
/// inside the XAML framework with exception code <c>0xc000027b</c>
/// (STATUS_STOWED_EXCEPTION — a managed exception that crossed the framework
/// boundary unhandled), in two buckets: <c>0x8000ffff</c> (E_UNEXPECTED) and
/// <c>0x80070490</c> (ERROR_NOT_FOUND).
///
/// The page rebuilt the picker from scratch on EVERY render:
///
///     ProfileSelector.Items.Clear();
///     foreach (row) ProfileSelector.Items.Add(new ComboBoxItem { Tag = row.Id });
///     ProfileSelector.SelectedIndex = selected;
///
/// and read the current selection back out of those objects
/// (<c>SelectedItem.Tag</c>). Two things follow, and both are defects:
///
/// 1. SELECTION IDENTITY LIVED IN THROWAWAY XAML OBJECTS. Every render replaced
///    the object the ComboBox's selection referred to. Renders are not only
///    user-driven — they arrive from a dispatcher-enqueued state-changed
///    callback and from every command's completion — so the collection could be
///    torn down while the control had a live reference into it (a popup open, a
///    selection being committed). That is what ERROR_NOT_FOUND means here.
///
/// 2. SAVE MADE IT LIKELY. Saving changes the library (a revision bumps, or a new
///    profile appears), which fires a state change, which enqueues a render. The
///    user's next act is to open the picker and choose Default — precisely while
///    that render lands.
///
/// The fix is not a null guard. Selection identity is a stable profile ID owned
/// here; the page reconciles the collection instead of replacing it, and rebuilds
/// only when the rows genuinely differ. Everything on this type is pure, so the
/// sequences that used to crash are ordinary unit tests.
/// </remarks>
public static class KbmProfileSelection
{
    /// <summary>
    /// Decide what the picker should show, given what it already shows.
    /// </summary>
    /// <param name="rendered">
    /// The rows currently in the control. Empty on a first render.
    /// </param>
    /// <param name="rows">The rows the view wants shown.</param>
    /// <param name="selectedId">
    /// The profile the view wants selected, or <see cref="KbmProfileIds.None"/>.
    /// </param>
    public static KbmProfileSelectionPlan Plan(
        IReadOnlyList<KbmProfileInfo> rendered,
        IReadOnlyList<KbmProfileInfo> rows,
        int selectedId)
    {
        // Fall back to the first row rather than to nothing. A picker showing an
        // empty box when profiles exist is how "the profile workflow looks
        // absent" gets reported.
        var index = IndexOf(rows, selectedId);
        if (index < 0 && rows.Count > 0)
        {
            index = 0;
            selectedId = rows[0].Id;
        }

        if (rows.Count == 0)
        {
            selectedId = KbmProfileIds.None;
        }

        return new KbmProfileSelectionPlan(rows, selectedId, index,
                                           Rebuild: !SameRows(rendered, rows));
    }

    /// <summary>
    /// Whether the two row sets are interchangeable in the picker.
    /// </summary>
    /// <remarks>
    /// Compares only what the control DISPLAYS — identity and label. A revision
    /// bump or a fingerprint change after a save alters neither, so the common
    /// post-save render leaves the collection alone entirely, and the control
    /// keeps the item its selection already points at.
    /// </remarks>
    public static bool SameRows(IReadOnlyList<KbmProfileInfo> a,
                                IReadOnlyList<KbmProfileInfo> b)
    {
        if (a.Count != b.Count)
        {
            return false;
        }

        for (var i = 0; i < a.Count; i++)
        {
            if (a[i].Id != b[i].Id ||
                !string.Equals(a[i].Name, b[i].Name, StringComparison.Ordinal) ||
                a[i].Builtin != b[i].Builtin)
            {
                return false;
            }
        }

        return true;
    }

    /// <summary>
    /// What a selection change should do, decided before anything is awaited.
    /// </summary>
    /// <remarks>
    /// The old handler read <c>draft?.ProfileId</c>, then awaited a confirmation
    /// dialog, then acted — so its decision was made against state that could
    /// have been replaced by the time it resumed. Deciding up front and returning
    /// a value makes the transition atomic with respect to the await.
    /// </remarks>
    public static KbmSelectionAction Decide(int requestedId, int openDraftId,
                                            bool draftDirty, bool suppressed)
    {
        if (suppressed || requestedId == KbmProfileIds.None)
        {
            return KbmSelectionAction.Ignore;
        }

        // Already open. Re-selecting the same profile must not reload it, or a
        // render that re-asserts the selection would reopen the draft and throw
        // away unsaved edits.
        if (requestedId == openDraftId)
        {
            return KbmSelectionAction.Ignore;
        }

        return draftDirty ? KbmSelectionAction.ConfirmDiscard : KbmSelectionAction.Open;
    }

    private static int IndexOf(IReadOnlyList<KbmProfileInfo> rows, int id)
    {
        for (var i = 0; i < rows.Count; i++)
        {
            if (rows[i].Id == id)
            {
                return i;
            }
        }

        return -1;
    }
}

public enum KbmSelectionAction
{
    /// <summary>Not a real user transition; do nothing at all.</summary>
    Ignore,

    /// <summary>Open the requested profile in a fresh draft.</summary>
    Open,

    /// <summary>Unsaved edits exist; ask before abandoning them.</summary>
    ConfirmDiscard,
}
