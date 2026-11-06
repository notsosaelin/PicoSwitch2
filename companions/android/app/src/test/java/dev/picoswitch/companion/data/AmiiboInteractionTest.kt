package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The interaction contract both companions implement.
 *
 * THE PRODUCT RULE THESE PIN, in one line: `single = browse, double = inspect,
 * long press = select`. Every assertion below is a consequence of that rule, and
 * the C# `AmiiboInteractionTests` asserts the same things against the mirrored
 * implementation, so the two clients cannot drift into behaving differently
 * while both look correct on their own.
 *
 * Deliberately free of pixels, coordinates, gesture timings and composables. A
 * test that tapped at (120, 340) would be pinning a layout, not a behaviour, and
 * would have to be rewritten every time the browser was restyled.
 */
class AmiiboInteractionTest {

    private val fresh = AmiiboInteractionState()

    private fun selecting(vararg ids: String): AmiiboInteractionState =
        ids.fold(fresh) { state, id -> AmiiboInteraction.toggleSelection(state, id) }

    // ------------------------------------------------------------ the invariant

    /**
     * An open inspector always describes the focused item.
     *
     * The property every single-item command depends on. Send to adapter,
     * Rename, Initialize and Delete are all keyed on focus, and they live in the
     * details surface; if the two could disagree, a button in a sheet headed
     * "Link" could act on Zelda. Asserted over every transition rather than at
     * one call site so a new transition cannot quietly break it.
     */
    @Test fun `the inspected item is always the focused item`() {
        val open = AmiiboInteraction.openInspector(fresh, "a")
        listOf(
            fresh,
            AmiiboInteraction.focus(fresh, "a"),
            open,
            AmiiboInteraction.focus(open, "b"),
            AmiiboInteraction.closeInspector(open),
            AmiiboInteraction.enterSelection(open, "b"),
            AmiiboInteraction.toggleSelection(open, "b"),
            AmiiboInteraction.activate(open, "b"),
            AmiiboInteraction.escape(open),
            AmiiboInteraction.prune(open, listOf("b")),
        ).forEach { state ->
            if (state.inspectorOpen) assertEquals(state.focusedId, state.inspectedId)
        }
    }

    @Test fun `nothing is focused, inspected or selected to begin with`() {
        assertNull(fresh.focusedId)
        assertNull(fresh.inspectedId)
        assertFalse(fresh.inspectorOpen)
        assertFalse(fresh.selecting)
        assertEquals(0, fresh.selectedCount)
    }

    // ------------------------------------------------------------ normal mode

    /** The whole point: a tap browses, it does not inspect. */
    @Test fun `a single tap moves focus and opens nothing`() {
        val state = AmiiboInteraction.activate(fresh, "a")

        assertEquals("a", state.focusedId)
        assertFalse(state.inspectorOpen)
        assertFalse(state.selecting)
    }

    @Test fun `opening details requires an explicit request`() {
        val state = AmiiboInteraction.openInspector(fresh, "a")

        assertEquals("a", state.inspectedId)
        assertEquals("a", state.focusedId)
        assertTrue(state.inspectorOpen)
    }

    /**
     * The behaviour this pass was asked for: the sheet does not chase the
     * highlight. Focusing B while A is open returns the user to browsing.
     */
    @Test fun `tapping another item closes the inspector rather than replacing it`() {
        val open = AmiiboInteraction.openInspector(fresh, "a")
        val moved = AmiiboInteraction.activate(open, "b")

        assertEquals("b", moved.focusedId)
        assertFalse(moved.inspectorOpen)
        assertNull(moved.inspectedId)
    }

    /** The first half of a double tap must not close the sheet. */
    @Test fun `re-tapping the inspected item leaves it open`() {
        val open = AmiiboInteraction.openInspector(fresh, "a")
        val again = AmiiboInteraction.activate(open, "a")

        assertEquals("a", again.focusedId)
        assertTrue(again.inspectorOpen)
    }

    /** A double tap is a focus followed by an open request. */
    @Test fun `a double tap ends with the inspector open`() {
        val first = AmiiboInteraction.activate(fresh, "a")
        val second = AmiiboInteraction.openInspector(first, "a")

        assertTrue(second.inspectorOpen)
        assertEquals("a", second.inspectedId)
    }

    @Test fun `closing the inspector keeps the focus it was showing`() {
        val open = AmiiboInteraction.openInspector(fresh, "a")
        val closed = AmiiboInteraction.closeInspector(open)

        assertFalse(closed.inspectorOpen)
        assertEquals("a", closed.focusedId)
    }

    // ------------------------------------------------------------- selection

    @Test fun `entering selection selects the item it started from`() {
        val state = AmiiboInteraction.enterSelection(fresh, "a")

        assertTrue(state.selecting)
        assertTrue(state.isSelected("a"))
        assertEquals(1, state.selectedCount)
        assertEquals("a", state.focusedId)
    }

    /** Long-pressing every item is exactly what this avoids. */
    @Test fun `subsequent taps toggle membership`() {
        var state = AmiiboInteraction.enterSelection(fresh, "a")
        state = AmiiboInteraction.activate(state, "b")
        state = AmiiboInteraction.activate(state, "c")

        assertEquals(3, state.selectedCount)

        state = AmiiboInteraction.activate(state, "b")

        assertEquals(2, state.selectedCount)
        assertFalse(state.isSelected("b"))
        assertTrue(state.isSelected("a"))
        assertTrue(state.isSelected("c"))
    }

    /** Ambiguous command scope is designed out, not warned about. */
    @Test fun `entering selection closes an open inspector`() {
        val open = AmiiboInteraction.openInspector(fresh, "a")
        val selecting = AmiiboInteraction.enterSelection(open, "b")

        assertFalse(selecting.inspectorOpen)
        assertTrue(selecting.selecting)
    }

    @Test fun `selection mode refuses to open the inspector`() {
        val selecting = AmiiboInteraction.enterSelection(fresh, "a")
        val attempted = AmiiboInteraction.openInspector(selecting, "b")

        assertEquals(selecting, attempted)
        assertFalse(attempted.inspectorOpen)
    }

    /**
     * A double tap during selection mode must not toggle AND inspect. The second
     * half is refused, so the gesture degrades to a plain toggle.
     */
    @Test fun `a double tap during selection only toggles`() {
        val selecting = AmiiboInteraction.enterSelection(fresh, "a")
        val once = AmiiboInteraction.activate(selecting, "b")
        val twice = AmiiboInteraction.openInspector(once, "b")

        assertFalse(twice.inspectorOpen)
        assertTrue(twice.isSelected("b"))
        assertEquals(once, twice)
    }

    /**
     * THE HIGHLIGHT AND THE TICK ARE INDEPENDENT, and tapping must not care
     * which item is highlighted.
     *
     * A real defect, found on Windows by using the built page rather than
     * reading it: selection was driven from a host event that only fires when
     * the highlight MOVES, so the already-highlighted tile could be ticked and
     * never un-ticked. The domain was always right — this pins the property the
     * plumbing broke, on both platforms, so no future gesture handler can
     * quietly reintroduce it.
     */
    @Test fun `activating the focused item toggles it like any other`() {
        // Two, so the set outlives the un-tick: emptying it would leave
        // selection mode, which is a different rule and is pinned separately.
        // Selecting leaves the focus on the last item touched, so "b" is both
        // focused AND selected — precisely the state that could not be undone.
        val state = selecting("a", "b")
        assertEquals("b", state.focusedId)
        assertTrue(state.isSelected("b"))

        val untick = AmiiboInteraction.activate(state, "b")
        assertFalse(untick.isSelected("b"))
        assertEquals("b", untick.focusedId)
        assertTrue(untick.selecting)

        val retick = AmiiboInteraction.activate(untick, "b")
        assertTrue(retick.isSelected("b"))
        assertEquals("b", retick.focusedId)
    }

    /** Toggling the highlighted item must be able to START a selection. */
    @Test fun `toggling the focused item starts a selection from nothing`() {
        val browsing = AmiiboInteraction.activate(fresh, "a")
        assertFalse(browsing.selecting)

        val selecting = AmiiboInteraction.toggleSelection(browsing, "a")

        assertTrue(selecting.selecting)
        assertTrue(selecting.isSelected("a"))
    }

    /** Un-ticking the last box leaves the mode, as a box would. */
    @Test fun `removing the final item leaves selection mode`() {
        var state = AmiiboInteraction.enterSelection(fresh, "a")
        state = AmiiboInteraction.activate(state, "a")

        assertFalse(state.selecting)
        assertEquals(0, state.selectedCount)
        assertEquals("a", state.focusedId)
    }

    /** A stray long press must not discard a set being built. */
    @Test fun `long-pressing an already selected item does not restart the selection`() {
        val state = selecting("a", "b", "c")
        val pressed = AmiiboInteraction.enterSelection(state, "b")

        assertEquals(3, pressed.selectedCount)
        assertEquals("b", pressed.focusedId)
    }

    @Test fun `cancelling selection keeps focus and opens nothing`() {
        val state = selecting("a", "b")
        val cleared = AmiiboInteraction.clearSelection(state)

        assertFalse(cleared.selecting)
        assertEquals(state.focusedId, cleared.focusedId)
        assertFalse(cleared.inspectorOpen)
    }

    // ------------------------------------------------------------------ back

    /** One gesture, one unambiguous order: the more disruptive mode goes first. */
    @Test fun `back dismisses selection before the inspector`() {
        val selecting = AmiiboInteraction.enterSelection(fresh, "a")
        assertFalse(AmiiboInteraction.escape(selecting).selecting)

        val open = AmiiboInteraction.openInspector(fresh, "a")
        assertFalse(AmiiboInteraction.escape(open).inspectorOpen)
    }

    /**
     * An unchanged result is how the caller knows Back should navigate away
     * instead of being swallowed.
     */
    @Test fun `back with nothing open changes nothing`() {
        val browsing = AmiiboInteraction.activate(fresh, "a")

        assertEquals(browsing, AmiiboInteraction.escape(browsing))
    }

    // ------------------------------------------------------ stable identities

    /**
     * THE POLICY, chosen and pinned: selection is held by library id and
     * SURVIVES a query change. Re-sorting, searching or filtering cannot disturb
     * it, because none of them change what an item's id is.
     */
    @Test fun `selection survives reordering and filtering`() {
        val state = selecting("ccc", "a")

        assertTrue(state.isSelected("a"))
        assertTrue(state.isSelected("ccc"))
        assertEquals(2, state.selectedCount)
        assertEquals(0, state.hiddenSelectedCount(listOf("a", "bb", "ccc")))
    }

    /**
     * The count a destructive confirmation must state. Two of the three selected
     * entries are filtered out of view, and the user is about to destroy all
     * three.
     */
    @Test fun `selected items hidden by a filter are still counted`() {
        val state = selecting("a", "bb", "ccc")

        assertEquals(3, state.selectedCount)
        assertEquals(2, state.hiddenSelectedCount(listOf("a")))
        assertEquals(0, state.hiddenSelectedCount(listOf("a", "bb", "ccc", "d")))
        assertEquals(3, state.hiddenSelectedCount(emptyList()))
    }

    /**
     * Switching Grid to List to Carousel is a change of presentation only, so it
     * goes nowhere near this state and the set is preserved by construction:
     * there is no transition that takes a view mode at all.
     */
    @Test fun `the selected set is independent of how the library is being shown`() {
        val state = selecting("a", "bb")

        assertEquals(listOf("a", "bb"), state.selection)
        assertEquals(2, state.selectedCount)
    }

    /**
     * Order of selection must not change what the set IS.
     *
     * The SET compares equal; the states do not, and should not — focus follows
     * the last item touched, which genuinely differs between the two orders.
     */
    @Test fun `two identical selections built in different orders hold the same set`() {
        val first = selecting("a", "bb", "ccc")
        val second = selecting("ccc", "a", "bb")

        assertEquals(first.selection, second.selection)
        assertEquals("ccc", first.focusedId)
        assertEquals("bb", second.focusedId)
    }

    @Test fun `selecting the same item twice does not duplicate it`() {
        var state = AmiiboInteraction.enterSelection(fresh, "a")
        state = AmiiboInteraction.enterSelection(state, "a")

        assertEquals(1, state.selectedCount)
    }

    // --------------------------------------------------------------- removal

    @Test fun `pruning drops entries the library no longer has`() {
        val state = selecting("a", "bb", "ccc")
        val pruned = AmiiboInteraction.prune(state, listOf("a", "ccc"))

        assertEquals(2, pruned.selectedCount)
        assertFalse(pruned.isSelected("bb"))
    }

    @Test fun `pruning closes an inspector describing something that is gone`() {
        val open = AmiiboInteraction.openInspector(fresh, "a")
        val pruned = AmiiboInteraction.prune(open, listOf("b"))

        assertFalse(pruned.inspectorOpen)
        assertNull(pruned.focusedId)
    }

    /**
     * Deleting from the middle leaves the user where they were working. The
     * neighbour that took the deleted item's place gets the highlight.
     */
    @Test fun `focus lands on the neighbour that took the deleted item's place`() {
        val ordered = listOf("a", "b", "c", "d", "e")

        assertEquals("d", AmiiboInteraction.focusAfterRemoval(ordered, listOf("b", "c"), "b"))
        assertEquals("b", AmiiboInteraction.focusAfterRemoval(ordered, listOf("a"), "a"))
    }

    /** Removing the tail falls back to the last survivor. */
    @Test fun `deleting the end of the library focuses the last survivor`() {
        val ordered = listOf("a", "b", "c")

        assertEquals("a", AmiiboInteraction.focusAfterRemoval(ordered, listOf("b", "c"), "c"))
    }

    @Test fun `deleting everything leaves nothing focused`() {
        assertNull(AmiiboInteraction.focusAfterRemoval(listOf("a", "b"), listOf("a", "b"), "a"))
    }

    /** A deletion elsewhere must not move the user. */
    @Test fun `an unaffected focus is left where it was`() {
        val ordered = listOf("a", "b", "c")

        assertEquals("c", AmiiboInteraction.focusAfterRemoval(ordered, listOf("a"), "c"))
    }

    /** Nobody is left in selection mode staring at a set that no longer exists. */
    @Test fun `settling after a removal leaves selection mode with a sensible focus`() {
        val state = selecting("b", "c")
        val settled = AmiiboInteraction.afterRemoval(state, listOf("a", "b", "c", "d"), listOf("b", "c"))

        assertFalse(settled.selecting)
        assertEquals("d", settled.focusedId)
        assertFalse(settled.inspectorOpen)
    }

    // ---------------------------------------------------------- bulk outcomes

    @Test fun `an all-successful batch says so plainly`() {
        val outcome = AmiiboBulkOutcome(succeeded = listOf("a", "b"))

        assertFalse(outcome.anyFailed)
        assertEquals(2, outcome.total)
        assertEquals("2 Amiibo initialized", outcome.summary("initialized"))
    }

    /**
     * The lie this exists to prevent: reporting twelve successes when three
     * failed. The user finds out much later, from a tag that did not change.
     */
    @Test fun `a partial batch never rounds up to success`() {
        val outcome = AmiiboBulkOutcome(
            succeeded = listOf("a", "b"),
            failed = listOf(AmiiboBulkFailure("c", "Kirby", "key did not verify")),
        )

        assertTrue(outcome.anyFailed)
        assertEquals(3, outcome.total)
        assertEquals("2 of 3 Amiibo initialized; 1 failed", outcome.summary("initialized"))
    }

    @Test fun `a total failure is not described as partial success`() {
        val outcome = AmiiboBulkOutcome(
            failed = listOf(AmiiboBulkFailure("c", "Kirby", "key did not verify")),
        )

        assertEquals("No Amiibo initialized; 1 failed", outcome.summary("initialized"))
    }

    @Test fun `a failure carries enough to tell the user which tag and why`() {
        val failure = AmiiboBulkFailure("c", "Kirby", "key did not verify")

        assertEquals("Kirby", failure.name)
        assertEquals("key did not verify", failure.reason)
    }
}
