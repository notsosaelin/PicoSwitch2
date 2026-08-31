using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// THREE SEPARATE IDEAS THAT USED TO BE ONE PROPERTY.
/// </summary>
/// <remarks>
/// <para>
/// Both companions carried a single "selected Amiibo", and it silently meant
/// three different things: which card is highlighted, which one's details are on
/// screen, and — once bulk actions existed — which ones a destructive command
/// would apply to. Conflating them is why the details pane used to chase every
/// highlight, and why there was nowhere to put a multi-selection.
/// </para>
/// <list type="bullet">
/// <item><see cref="FocusedId"/> — highlighted in the browser. Moves on a single
/// click, a tap, or arrow keys. Means nothing more than "this is where you
/// are".</item>
/// <item><see cref="InspectedId"/> — whose details surface is OPEN. Only ever
/// set by an explicit request: a double click, a double tap, or the accessible
/// "Open details" action.</item>
/// <item><see cref="Selection"/> — the set a bulk command would act on.</item>
/// </list>
/// <para>
/// THE INVARIANT THAT KEEPS THE REST OF THE CODE HONEST: when the inspector is
/// open, <see cref="InspectedId"/> equals <see cref="FocusedId"/>. Opening
/// details moves focus there, and moving focus elsewhere closes the inspector,
/// so single-item commands keyed on focus are always aimed at the item being
/// described. Every transition below preserves it, and a test pins it.
/// </para>
/// <para>
/// Selection mode is DERIVED from the set rather than stored beside it. A
/// separate flag can disagree with its own set; this cannot. Removing the last
/// item therefore leaves selection mode, which is also the behaviour a user
/// expects from un-ticking the last box.
/// </para>
/// <para>
/// Mirrored by the Kotlin <c>AmiiboInteractionState</c>. This is a behavioural
/// contract shared by both companions and by any future touch-capable
/// PicoSwitch client, so it is a pure record with a pure reducer and no
/// reference whatsoever to pointers, gestures, views or windows.
/// </para>
/// </remarks>
public sealed record AmiiboInteractionState
{
    /// <summary>The highlighted item. Not a request to describe it.</summary>
    public string? FocusedId { get; init; }

    /// <summary>The item whose details surface is open; null when closed.</summary>
    public string? InspectedId { get; init; }

    /// <summary>
    /// The bulk-action set, in a canonical order so two equal selections compare
    /// equal regardless of the order they were built up in.
    /// </summary>
    public ValueList<string> Selection { get; init; } = ValueList<string>.Empty;

    /// <summary>True while taps toggle membership instead of moving focus.</summary>
    public bool Selecting => Selection.Count > 0;

    public bool InspectorOpen => InspectedId is not null;

    public int SelectedCount => Selection.Count;

    public bool IsSelected(string id) => Selection.Contains(id, StringComparer.Ordinal);

    /// <summary>
    /// How many selected items the current query is not showing.
    /// </summary>
    /// <remarks>
    /// Selection survives a filter change (see <see cref="AmiiboInteraction"/>), so
    /// a user can narrow to one series, select it, narrow to another and select
    /// that too. The count they are about to destroy must therefore be stated
    /// including what they cannot currently see — this is what the confirmation
    /// uses to say so.
    /// </remarks>
    public int HiddenSelectedCount(IEnumerable<string> visibleIds)
    {
        var visible = new HashSet<string>(visibleIds, StringComparer.Ordinal);
        return Selection.Count(id => !visible.Contains(id));
    }
}

/// <summary>
/// Every legal transition of the browser's interaction state.
/// </summary>
/// <remarks>
/// Gesture and pointer handlers translate input into these calls and do nothing
/// else. That is what makes the interaction model testable without rendering
/// anything, and what lets Windows and Android demonstrably share one behaviour
/// rather than two implementations that merely look similar.
/// </remarks>
public static class AmiiboInteraction
{
    /// <summary>
    /// A plain tap or click: browse, never inspect.
    /// </summary>
    /// <remarks>
    /// The single rule the whole model rests on is <c>single = browse, double =
    /// inspect</c>, so this deliberately cannot open anything. During selection
    /// mode a tap means something else entirely, which is why it routes to
    /// <see cref="ToggleSelection"/> rather than quietly doing both.
    /// </remarks>
    public static AmiiboInteractionState Activate(AmiiboInteractionState state, string id) =>
        state.Selecting ? ToggleSelection(state, id) : Focus(state, id);

    /// <summary>
    /// Move the highlight.
    /// </summary>
    /// <remarks>
    /// MOVING FOCUS CLOSES AN OPEN INSPECTOR. The user explicitly asked to see
    /// A; clicking B is a return to browsing, not a request to see B. Silently
    /// swapping the pane's contents would make the details surface follow the
    /// mouse, which is the behaviour this pass exists to remove. B is inspected
    /// by asking for it — double click, double tap, or the explicit command.
    ///
    /// Re-focusing the item already being inspected leaves it open: that is the
    /// first half of a double click, not a request to close.
    /// </remarks>
    public static AmiiboInteractionState Focus(AmiiboInteractionState state, string id) => state with
    {
        FocusedId = id,
        InspectedId = string.Equals(state.InspectedId, id, StringComparison.Ordinal) ? id : null,
    };

    /// <summary>Explicitly open the details surface for one item.</summary>
    /// <remarks>
    /// Refused during selection mode. A multi-selection and a single-item
    /// inspector on screen together give every command in the inspector an
    /// ambiguous scope — does Initialize mean this one or those twelve? — so the
    /// two are mutually exclusive by construction.
    /// </remarks>
    public static AmiiboInteractionState OpenInspector(AmiiboInteractionState state, string id) =>
        state.Selecting ? state : state with { FocusedId = id, InspectedId = id };

    /// <summary>Dismiss the details surface. The browser keeps its place.</summary>
    public static AmiiboInteractionState CloseInspector(AmiiboInteractionState state) =>
        state with { InspectedId = null };

    /// <summary>
    /// Long press, or the accessible "Select" action: begin a multi-selection.
    /// </summary>
    /// <remarks>
    /// Entering selection closes the inspector for the reason given on
    /// <see cref="OpenInspector"/>. Long-pressing while already selecting adds
    /// rather than restarting, so a stray long press cannot discard a set the
    /// user has been building.
    /// </remarks>
    public static AmiiboInteractionState EnterSelection(AmiiboInteractionState state, string id) =>
        state.IsSelected(id)
            ? state with { FocusedId = id, InspectedId = null }
            : Add(state, id);

    /// <summary>Add or remove one item from the bulk set.</summary>
    /// <remarks>
    /// Also the entry point when nothing is selected yet, which is what makes a
    /// desktop Ctrl+click on the first item start a selection.
    /// </remarks>
    public static AmiiboInteractionState ToggleSelection(AmiiboInteractionState state, string id) =>
        state.IsSelected(id)
            ? state with
            {
                FocusedId = id,
                InspectedId = null,
                Selection = Canonical(state.Selection.Where(
                    existing => !string.Equals(existing, id, StringComparison.Ordinal))),
            }
            : Add(state, id);

    /// <summary>Leave selection mode. Browsing is otherwise undisturbed.</summary>
    /// <remarks>
    /// Deliberately touches neither focus nor any query state: cancelling a
    /// selection must not also lose the user's place, their search or their
    /// filters.
    /// </remarks>
    public static AmiiboInteractionState ClearSelection(AmiiboInteractionState state) =>
        state with { Selection = ValueList<string>.Empty };

    /// <summary>
    /// Escape, or Android Back: undo the most specific thing that is open.
    /// </summary>
    /// <remarks>
    /// One key with an unambiguous order, so the user never has to guess which
    /// of two modes it will cancel. Selection is the more disruptive mode and is
    /// dismissed first; the caller can tell nothing happened — and that Back
    /// should navigate away instead — by comparing the result.
    /// </remarks>
    public static AmiiboInteractionState Escape(AmiiboInteractionState state) => state.Selecting
        ? ClearSelection(state)
        : state.InspectorOpen
            ? CloseInspector(state)
            : state;

    /// <summary>
    /// Reconcile with a library that has gained or lost entries.
    /// </summary>
    /// <remarks>
    /// Selection holds STABLE LIBRARY IDS, never positions or row objects, so
    /// sorting, filtering and re-projecting cannot disturb it — only an entry
    /// actually leaving the library can. This is the single place that happens,
    /// and it also closes an inspector describing something that no longer
    /// exists.
    /// </remarks>
    public static AmiiboInteractionState Prune(AmiiboInteractionState state, IEnumerable<string> libraryIds)
    {
        var live = new HashSet<string>(libraryIds, StringComparer.Ordinal);

        return state with
        {
            FocusedId = state.FocusedId is { } focused && live.Contains(focused) ? focused : null,
            InspectedId = state.InspectedId is { } inspected && live.Contains(inspected) ? inspected : null,
            Selection = Canonical(state.Selection.Where(live.Contains)),
        };
    }

    /// <summary>
    /// Where the highlight lands after entries are removed.
    /// </summary>
    /// <remarks>
    /// The neighbour that took the removed item's place, so deleting from the
    /// middle of a thousand-item library leaves the user where they were working
    /// rather than at the top. Falls back to the last survivor when the removal
    /// took everything from the end, and to nothing when it took everything.
    /// </remarks>
    public static string? FocusAfterRemoval(
        IReadOnlyList<string> ordered, IEnumerable<string> removed, string? currentFocus)
    {
        var gone = new HashSet<string>(removed, StringComparer.Ordinal);
        var survivors = ordered.Where(id => !gone.Contains(id)).ToList();

        if (survivors.Count == 0)
        {
            return null;
        }

        if (currentFocus is { } focus && !gone.Contains(focus))
        {
            return focus;
        }

        var firstRemoved = ordered.ToList().FindIndex(gone.Contains);
        if (firstRemoved < 0)
        {
            return survivors[0];
        }

        for (var index = firstRemoved; index < ordered.Count; index++)
        {
            if (!gone.Contains(ordered[index]))
            {
                return ordered[index];
            }
        }

        return survivors[^1];
    }

    /// <summary>
    /// Settle the browser after a bulk removal: prune, re-focus, stop selecting.
    /// </summary>
    public static AmiiboInteractionState AfterRemoval(
        AmiiboInteractionState state, IReadOnlyList<string> orderedBefore, IEnumerable<string> removed)
    {
        var gone = removed.ToList();
        var focus = FocusAfterRemoval(orderedBefore, gone, state.FocusedId);
        var survivors = orderedBefore.Where(id => !gone.Contains(id, StringComparer.Ordinal));

        return Prune(state with { FocusedId = focus, Selection = ValueList<string>.Empty }, survivors);
    }

    private static AmiiboInteractionState Add(AmiiboInteractionState state, string id) => state with
    {
        FocusedId = id,
        InspectedId = null,
        Selection = Canonical(state.Selection.Append(id)),
    };

    /// <summary>
    /// One order for one set, so equality compares content and not history.
    /// </summary>
    private static ValueList<string> Canonical(IEnumerable<string> ids) =>
        new([.. ids.Distinct(StringComparer.Ordinal).OrderBy(id => id, StringComparer.Ordinal)]);
}

/// <summary>
/// What a bulk command actually did, item by item.
/// </summary>
/// <remarks>
/// A batch over a thousand-item library is not all-or-nothing: one dump can fail
/// to decrypt while the rest re-sign perfectly. Returning a bare success flag
/// would force the caller to either roll back work that succeeded or claim work
/// that did not, so the result carries both lists and the summary sentence is
/// derived from them rather than written at each call site.
/// </remarks>
public sealed record AmiiboBulkOutcome(
    ValueList<string> Succeeded,
    ValueList<AmiiboBulkFailure> Failed)
{
    public static AmiiboBulkOutcome Empty { get; } =
        new(ValueList<string>.Empty, ValueList<AmiiboBulkFailure>.Empty);

    public int Total => Succeeded.Count + Failed.Count;

    public bool AnyFailed => Failed.Count > 0;

    /// <summary>
    /// One sentence stating exactly what happened, in the user's terms.
    /// </summary>
    /// <param name="verb">Past tense, e.g. "initialized" or "deleted".</param>
    /// <remarks>
    /// Never rounds a partial result up to a success. When some entries failed
    /// the sentence says how many of each, because "12 Amiibo initialized" after
    /// three of them failed is a lie the user only discovers much later.
    /// </remarks>
    public string Summary(string verb) => Failed.Count switch
    {
        // "Amiibo" is its own plural, so no count-dependent noun is needed.
        0 => $"{Succeeded.Count} Amiibo {verb}",
        _ when Succeeded.Count == 0 => $"No Amiibo {verb}; {Failed.Count} failed",
        _ => $"{Succeeded.Count} of {Total} Amiibo {verb}; {Failed.Count} failed",
    };
}

/// <summary>One entry a bulk command could not process, and why.</summary>
public sealed record AmiiboBulkFailure(string Id, string Name, string Reason);
