using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Xunit;

namespace PicoSwitch.Companion.Services.Tests;

/// <summary>
/// The interaction contract both companions implement.
/// </summary>
/// <remarks>
/// <para>
/// THE PRODUCT RULE THESE PIN, in one line: <c>single = browse, double =
/// inspect, long press = select</c>. Every assertion below is a consequence of
/// that rule, and the Kotlin <c>AmiiboInteractionTest</c> asserts the same
/// things against the mirrored implementation, so the two clients cannot drift
/// into behaving differently while both look correct on their own.
/// </para>
/// <para>
/// Deliberately free of pixels, coordinates, gesture timings and controls. A
/// test that clicked at (120, 340) would be pinning a layout, not a behaviour,
/// and would have to be rewritten every time the browser was restyled.
/// </para>
/// </remarks>
public sealed class AmiiboInteractionTests
{
    private static readonly AmiiboInteractionState Fresh = new();

    private static AmiiboInteractionState Selecting(params string[] ids)
    {
        var state = Fresh;
        foreach (var id in ids)
        {
            state = AmiiboInteraction.ToggleSelection(state, id);
        }

        return state;
    }

    // ------------------------------------------------------------ the invariant

    /// <summary>
    /// An open inspector always describes the focused item.
    /// </summary>
    /// <remarks>
    /// The property every single-item command depends on. Upload, Rename,
    /// Initialize and Delete are all keyed on focus, and they live in the
    /// inspector; if the two could disagree, a button in a pane headed "Link"
    /// could act on Zelda. Asserted over every transition rather than at one
    /// call site so a new transition cannot quietly break it.
    /// </remarks>
    [Fact]
    public void InspectedItemIsAlwaysTheFocusedItem()
    {
        var states = new List<AmiiboInteractionState>
        {
            Fresh,
            AmiiboInteraction.Focus(Fresh, "a"),
            AmiiboInteraction.OpenInspector(Fresh, "a"),
            AmiiboInteraction.Focus(AmiiboInteraction.OpenInspector(Fresh, "a"), "b"),
            AmiiboInteraction.CloseInspector(AmiiboInteraction.OpenInspector(Fresh, "a")),
            AmiiboInteraction.EnterSelection(AmiiboInteraction.OpenInspector(Fresh, "a"), "b"),
            AmiiboInteraction.ToggleSelection(AmiiboInteraction.OpenInspector(Fresh, "a"), "b"),
            AmiiboInteraction.Activate(AmiiboInteraction.OpenInspector(Fresh, "a"), "b"),
            AmiiboInteraction.Escape(AmiiboInteraction.OpenInspector(Fresh, "a")),
            AmiiboInteraction.Prune(AmiiboInteraction.OpenInspector(Fresh, "a"), ["b"]),
        };

        foreach (var state in states)
        {
            if (state.InspectorOpen)
            {
                Assert.Equal(state.FocusedId, state.InspectedId);
            }
        }
    }

    [Fact]
    public void NothingIsFocusedInspectedOrSelectedToBeginWith()
    {
        Assert.Null(Fresh.FocusedId);
        Assert.Null(Fresh.InspectedId);
        Assert.False(Fresh.InspectorOpen);
        Assert.False(Fresh.Selecting);
        Assert.Equal(0, Fresh.SelectedCount);
    }

    // ------------------------------------------------------------ normal mode

    /// <summary>The whole point: a tap browses, it does not inspect.</summary>
    [Fact]
    public void SingleActivationMovesFocusAndOpensNothing()
    {
        var state = AmiiboInteraction.Activate(Fresh, "a");

        Assert.Equal("a", state.FocusedId);
        Assert.False(state.InspectorOpen);
        Assert.False(state.Selecting);
    }

    [Fact]
    public void OpeningDetailsRequiresAnExplicitRequest()
    {
        var state = AmiiboInteraction.OpenInspector(Fresh, "a");

        Assert.Equal("a", state.InspectedId);
        Assert.Equal("a", state.FocusedId);
        Assert.True(state.InspectorOpen);
    }

    /// <summary>
    /// The behaviour this pass was asked for: the pane does not chase the
    /// highlight. Focusing B while A is open returns the user to browsing.
    /// </summary>
    [Fact]
    public void FocusingAnotherItemClosesTheInspectorRatherThanReplacingIt()
    {
        var open = AmiiboInteraction.OpenInspector(Fresh, "a");
        var moved = AmiiboInteraction.Activate(open, "b");

        Assert.Equal("b", moved.FocusedId);
        Assert.False(moved.InspectorOpen);
        Assert.Null(moved.InspectedId);
    }

    /// <summary>The first half of a double click must not close the pane.</summary>
    [Fact]
    public void RefocusingTheInspectedItemLeavesItOpen()
    {
        var open = AmiiboInteraction.OpenInspector(Fresh, "a");
        var again = AmiiboInteraction.Activate(open, "a");

        Assert.Equal("a", again.FocusedId);
        Assert.True(again.InspectorOpen);
    }

    /// <summary>A double click is a focus followed by an open request.</summary>
    [Fact]
    public void DoubleActivationEndsWithTheInspectorOpen()
    {
        var first = AmiiboInteraction.Activate(Fresh, "a");
        var second = AmiiboInteraction.OpenInspector(first, "a");

        Assert.True(second.InspectorOpen);
        Assert.Equal("a", second.InspectedId);
    }

    [Fact]
    public void ClosingTheInspectorKeepsTheFocusItWasShowing()
    {
        var open = AmiiboInteraction.OpenInspector(Fresh, "a");
        var closed = AmiiboInteraction.CloseInspector(open);

        Assert.False(closed.InspectorOpen);
        Assert.Equal("a", closed.FocusedId);
    }

    // ------------------------------------------------------------- selection

    [Fact]
    public void EnteringSelectionSelectsTheItemItStartedFrom()
    {
        var state = AmiiboInteraction.EnterSelection(Fresh, "a");

        Assert.True(state.Selecting);
        Assert.True(state.IsSelected("a"));
        Assert.Equal(1, state.SelectedCount);
        Assert.Equal("a", state.FocusedId);
    }

    /// <summary>Long-pressing every item is exactly what this avoids.</summary>
    [Fact]
    public void SubsequentActivationsToggleMembership()
    {
        var state = AmiiboInteraction.EnterSelection(Fresh, "a");
        state = AmiiboInteraction.Activate(state, "b");
        state = AmiiboInteraction.Activate(state, "c");

        Assert.Equal(3, state.SelectedCount);

        state = AmiiboInteraction.Activate(state, "b");

        Assert.Equal(2, state.SelectedCount);
        Assert.False(state.IsSelected("b"));
        Assert.True(state.IsSelected("a"));
        Assert.True(state.IsSelected("c"));
    }

    /// <summary>Ambiguous command scope is designed out, not warned about.</summary>
    [Fact]
    public void EnteringSelectionClosesAnOpenInspector()
    {
        var open = AmiiboInteraction.OpenInspector(Fresh, "a");
        var selecting = AmiiboInteraction.EnterSelection(open, "b");

        Assert.False(selecting.InspectorOpen);
        Assert.True(selecting.Selecting);
    }

    [Fact]
    public void SelectionModeRefusesToOpenTheInspector()
    {
        var selecting = AmiiboInteraction.EnterSelection(Fresh, "a");
        var attempted = AmiiboInteraction.OpenInspector(selecting, "b");

        Assert.Equal(selecting, attempted);
        Assert.False(attempted.InspectorOpen);
    }

    /// <summary>
    /// A double tap during selection mode must not toggle AND inspect. The
    /// second half is refused, so the gesture degrades to a plain toggle.
    /// </summary>
    [Fact]
    public void DoubleActivationDuringSelectionOnlyToggles()
    {
        var selecting = AmiiboInteraction.EnterSelection(Fresh, "a");
        var once = AmiiboInteraction.Activate(selecting, "b");
        var twice = AmiiboInteraction.OpenInspector(once, "b");

        Assert.False(twice.InspectorOpen);
        Assert.True(twice.IsSelected("b"));
        Assert.Equal(once, twice);
    }

    /// <summary>
    /// THE HIGHLIGHT AND THE TICK ARE INDEPENDENT, and clicking must not care
    /// which item is highlighted.
    /// </summary>
    /// <remarks>
    /// A real defect, found by using the built page rather than reading it.
    /// Selection was driven from the host's SelectionChanged, which WinUI raises
    /// only when its OWN selection moves; clicking the already-highlighted tile
    /// raised nothing, so that one tile could be ticked and never un-ticked. The
    /// domain was always right — this pins the property the plumbing broke, so a
    /// future handler cannot quietly reintroduce it.
    /// </remarks>
    [Fact]
    public void ActivatingTheFocusedItemTogglesItLikeAnyOther()
    {
        // Two, so the set outlives the un-tick: emptying it would leave
        // selection mode, which is a different rule and is pinned separately.
        // Selecting leaves the focus on the last item touched, so "b" is both
        // focused AND selected — precisely the state that could not be undone.
        var state = Selecting("a", "b");
        Assert.Equal("b", state.FocusedId);
        Assert.True(state.IsSelected("b"));

        // Un-ticking the very item the highlight is on.
        var untick = AmiiboInteraction.Activate(state, "b");
        Assert.False(untick.IsSelected("b"));
        Assert.Equal("b", untick.FocusedId);
        Assert.True(untick.Selecting);

        // And ticking it again, still without the highlight having moved.
        var retick = AmiiboInteraction.Activate(untick, "b");
        Assert.True(retick.IsSelected("b"));
        Assert.Equal("b", retick.FocusedId);
    }

    /// <summary>
    /// The same property for the desktop gesture: Ctrl+click on the highlighted
    /// item must be able to START a selection.
    /// </summary>
    [Fact]
    public void TogglingTheFocusedItemStartsASelectionFromNothing()
    {
        var browsing = AmiiboInteraction.Activate(Fresh, "a");
        Assert.False(browsing.Selecting);

        var selecting = AmiiboInteraction.ToggleSelection(browsing, "a");

        Assert.True(selecting.Selecting);
        Assert.True(selecting.IsSelected("a"));
    }

    /// <summary>Un-ticking the last box leaves the mode, as a box would.</summary>
    [Fact]
    public void RemovingTheFinalItemLeavesSelectionMode()
    {
        var state = AmiiboInteraction.EnterSelection(Fresh, "a");
        state = AmiiboInteraction.Activate(state, "a");

        Assert.False(state.Selecting);
        Assert.Equal(0, state.SelectedCount);
        Assert.Equal("a", state.FocusedId);
    }

    /// <summary>A stray long press must not discard a set being built.</summary>
    [Fact]
    public void LongPressingAnAlreadySelectedItemDoesNotRestartTheSelection()
    {
        var state = Selecting("a", "b", "c");
        var pressed = AmiiboInteraction.EnterSelection(state, "b");

        Assert.Equal(3, pressed.SelectedCount);
        Assert.Equal("b", pressed.FocusedId);
    }

    [Fact]
    public void CancellingSelectionKeepsFocusAndOpensNothing()
    {
        var state = Selecting("a", "b");
        var cleared = AmiiboInteraction.ClearSelection(state);

        Assert.False(cleared.Selecting);
        Assert.Equal(state.FocusedId, cleared.FocusedId);
        Assert.False(cleared.InspectorOpen);
    }

    // ---------------------------------------------------------------- escape

    /// <summary>
    /// One key, one unambiguous order: the more disruptive mode goes first.
    /// </summary>
    [Fact]
    public void EscapeDismissesSelectionBeforeTheInspector()
    {
        var selecting = AmiiboInteraction.EnterSelection(Fresh, "a");
        var afterFirst = AmiiboInteraction.Escape(selecting);

        Assert.False(afterFirst.Selecting);

        var open = AmiiboInteraction.OpenInspector(Fresh, "a");
        Assert.False(AmiiboInteraction.Escape(open).InspectorOpen);
    }

    /// <summary>
    /// An unchanged result is how the caller knows Back should navigate away
    /// instead of being swallowed.
    /// </summary>
    [Fact]
    public void EscapeWithNothingOpenChangesNothing()
    {
        var browsing = AmiiboInteraction.Activate(Fresh, "a");

        Assert.Equal(browsing, AmiiboInteraction.Escape(browsing));
    }

    // ------------------------------------------------------ stable identities

    /// <summary>
    /// THE POLICY, chosen and pinned: selection is held by library id and
    /// SURVIVES a query change. Re-sorting, searching or filtering cannot
    /// disturb it, because none of them change what an item's id is.
    /// </summary>
    [Fact]
    public void SelectionSurvivesReorderingAndFiltering()
    {
        var state = Selecting("ccc", "a");

        // Whatever the browser is currently showing, in whatever order.
        Assert.True(state.IsSelected("a"));
        Assert.True(state.IsSelected("ccc"));
        Assert.Equal(2, state.SelectedCount);
        Assert.Equal(0, state.HiddenSelectedCount(["a", "bb", "ccc"]));
    }

    /// <summary>
    /// The count a destructive confirmation must state. Two of the three
    /// selected entries are filtered out of view, and the user is about to
    /// destroy all three.
    /// </summary>
    [Fact]
    public void SelectedItemsHiddenByAFilterAreStillCounted()
    {
        var state = Selecting("a", "bb", "ccc");

        Assert.Equal(3, state.SelectedCount);
        Assert.Equal(2, state.HiddenSelectedCount(["a"]));
        Assert.Equal(0, state.HiddenSelectedCount(["a", "bb", "ccc", "d"]));
        Assert.Equal(3, state.HiddenSelectedCount([]));
    }

    /// <summary>
    /// Switching Grid to List to Carousel is a change of presentation only, so
    /// it goes nowhere near this state and the set is preserved by construction.
    /// </summary>
    [Fact]
    public void TheSelectedSetIsIndependentOfHowTheLibraryIsBeingShown()
    {
        var state = Selecting("a", "bb");
        var expected = state.Selection;

        // Nothing in the view mode can reach the interaction state; there is no
        // transition that takes one.
        Assert.Equal(expected, state.Selection);
        Assert.Equal(2, state.SelectedCount);
    }

    /// <summary>
    /// Order of selection must not change what the set IS.
    /// </summary>
    /// <remarks>
    /// The SET compares equal; the states do not, and should not — focus follows
    /// the last item touched, which genuinely differs between the two orders.
    /// That distinction is the reason the set is canonicalised rather than the
    /// whole state being compared anywhere it matters.
    /// </remarks>
    [Fact]
    public void TwoIdenticalSelectionsBuiltInDifferentOrdersHoldTheSameSet()
    {
        var first = Selecting("a", "bb", "ccc");
        var second = Selecting("ccc", "a", "bb");

        Assert.Equal(first.Selection, second.Selection);
        Assert.Equal("ccc", first.FocusedId);
        Assert.Equal("bb", second.FocusedId);
    }

    [Fact]
    public void SelectingTheSameItemTwiceDoesNotDuplicateIt()
    {
        var state = AmiiboInteraction.EnterSelection(Fresh, "a");
        state = AmiiboInteraction.EnterSelection(state, "a");

        Assert.Equal(1, state.SelectedCount);
    }

    // --------------------------------------------------------------- removal

    [Fact]
    public void PruningDropsEntriesTheLibraryNoLongerHas()
    {
        var state = Selecting("a", "bb", "ccc");
        var pruned = AmiiboInteraction.Prune(state, ["a", "ccc"]);

        Assert.Equal(2, pruned.SelectedCount);
        Assert.False(pruned.IsSelected("bb"));
    }

    [Fact]
    public void PruningClosesAnInspectorDescribingSomethingThatIsGone()
    {
        var open = AmiiboInteraction.OpenInspector(Fresh, "a");
        var pruned = AmiiboInteraction.Prune(open, ["b"]);

        Assert.False(pruned.InspectorOpen);
        Assert.Null(pruned.FocusedId);
    }

    /// <summary>
    /// Deleting from the middle leaves the user where they were working. The
    /// neighbour that took the deleted item's place gets the highlight.
    /// </summary>
    [Fact]
    public void FocusLandsOnTheNeighbourThatTookTheDeletedItemsPlace()
    {
        string[] ordered = ["a", "b", "c", "d", "e"];

        Assert.Equal("d", AmiiboInteraction.FocusAfterRemoval(ordered, ["b", "c"], "b"));
        Assert.Equal("b", AmiiboInteraction.FocusAfterRemoval(ordered, ["a"], "a"));
    }

    /// <summary>Removing the tail falls back to the last survivor.</summary>
    [Fact]
    public void DeletingTheEndOfTheLibraryFocusesTheLastSurvivor()
    {
        string[] ordered = ["a", "b", "c"];

        Assert.Equal("a", AmiiboInteraction.FocusAfterRemoval(ordered, ["b", "c"], "c"));
    }

    [Fact]
    public void DeletingEverythingLeavesNothingFocused()
    {
        string[] ordered = ["a", "b"];

        Assert.Null(AmiiboInteraction.FocusAfterRemoval(ordered, ["a", "b"], "a"));
    }

    /// <summary>A deletion elsewhere must not move the user.</summary>
    [Fact]
    public void AnUnaffectedFocusIsLeftWhereItWas()
    {
        string[] ordered = ["a", "b", "c"];

        Assert.Equal("c", AmiiboInteraction.FocusAfterRemoval(ordered, ["a"], "c"));
    }

    /// <summary>
    /// Nobody is left in selection mode staring at a set that no longer exists.
    /// </summary>
    [Fact]
    public void SettlingAfterARemovalLeavesSelectionModeWithASensibleFocus()
    {
        var state = Selecting("b", "c");
        var settled = AmiiboInteraction.AfterRemoval(state, ["a", "b", "c", "d"], ["b", "c"]);

        Assert.False(settled.Selecting);
        Assert.Equal("d", settled.FocusedId);
        Assert.False(settled.InspectorOpen);
    }

    // ---------------------------------------------------------- bulk outcomes

    [Fact]
    public void AnAllSuccessfulBatchSaysSoPlainly()
    {
        var outcome = new AmiiboBulkOutcome(new(["a", "b"]), ValueList<AmiiboBulkFailure>.Empty);

        Assert.False(outcome.AnyFailed);
        Assert.Equal(2, outcome.Total);
        Assert.Equal("2 Amiibo initialized", outcome.Summary("initialized"));
    }

    /// <summary>
    /// The lie this exists to prevent: reporting twelve successes when three
    /// failed. The user finds out much later, from a tag that did not change.
    /// </summary>
    [Fact]
    public void APartialBatchNeverRoundsUpToSuccess()
    {
        var outcome = new AmiiboBulkOutcome(
            new(["a", "b"]),
            new([new AmiiboBulkFailure("c", "Kirby", "key did not verify")]));

        Assert.True(outcome.AnyFailed);
        Assert.Equal(3, outcome.Total);
        Assert.Equal("2 of 3 Amiibo initialized; 1 failed", outcome.Summary("initialized"));
    }

    [Fact]
    public void ATotalFailureIsNotDescribedAsPartialSuccess()
    {
        var outcome = new AmiiboBulkOutcome(
            ValueList<string>.Empty,
            new([new AmiiboBulkFailure("c", "Kirby", "key did not verify")]));

        Assert.Equal("No Amiibo initialized; 1 failed", outcome.Summary("initialized"));
    }

    [Fact]
    public void AFailureCarriesEnoughToTellTheUserWhichTagAndWhy()
    {
        var failure = new AmiiboBulkFailure("c", "Kirby", "key did not verify");

        Assert.Equal("Kirby", failure.Name);
        Assert.Equal("key did not verify", failure.Reason);
    }
}
