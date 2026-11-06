package dev.picoswitch.companion.data

/**
 * THREE SEPARATE IDEAS THAT USED TO BE ONE PROPERTY.
 *
 * Both companions carried a single "selected Amiibo", and it silently meant
 * three different things: which card is highlighted, which one's details are on
 * screen, and — once bulk actions existed — which ones a destructive command
 * would apply to. Conflating them is why the details pane used to chase every
 * highlight, and why there was nowhere to put a multi-selection.
 *
 * - [focusedId] — highlighted in the browser. Moves on a single tap, a click, or
 *   arrow keys. Means nothing more than "this is where you are".
 * - [inspectedId] — whose details surface is OPEN. Only ever set by an explicit
 *   request: a double tap, a double click, or the accessible "Open details"
 *   action.
 * - [selection] — the set a bulk command would act on.
 *
 * THE INVARIANT THAT KEEPS THE REST OF THE CODE HONEST: when the inspector is
 * open, [inspectedId] equals [focusedId]. Opening details moves focus there, and
 * moving focus elsewhere closes the inspector, so single-item commands keyed on
 * focus are always aimed at the item being described. Every transition below
 * preserves it, and a test pins it.
 *
 * Selection mode is DERIVED from the set rather than stored beside it. A
 * separate flag can disagree with its own set; this cannot. Removing the last
 * item therefore leaves selection mode, which is also the behaviour a user
 * expects from un-ticking the last box.
 *
 * A DELIBERATE MIRROR of the C# `AmiiboInteractionState`. This is a behavioural
 * contract shared by both companions and by any future touch-capable PicoSwitch
 * client, so it is a pure data class with a pure reducer and no reference
 * whatsoever to gestures, composables, pointers or windows.
 */
data class AmiiboInteractionState(
    /** The highlighted item. Not a request to describe it. */
    val focusedId: String? = null,

    /** The item whose details surface is open; null when closed. */
    val inspectedId: String? = null,

    /**
     * The bulk-action set, in a canonical order so two equal selections compare
     * equal regardless of the order they were built up in.
     */
    val selection: List<String> = emptyList(),
) {
    /** True while taps toggle membership instead of moving focus. */
    val selecting: Boolean get() = selection.isNotEmpty()

    val inspectorOpen: Boolean get() = inspectedId != null

    val selectedCount: Int get() = selection.size

    fun isSelected(id: String): Boolean = id in selection

    /**
     * How many selected items the current query is not showing.
     *
     * Selection survives a filter change (see [AmiiboInteraction]), so a user can
     * narrow to one series, select it, narrow to another and select that too.
     * The count they are about to destroy must therefore be stated including
     * what they cannot currently see — this is what the confirmation uses to
     * say so.
     */
    fun hiddenSelectedCount(visibleIds: Collection<String>): Int {
        val visible = visibleIds.toHashSet()
        return selection.count { it !in visible }
    }
}

/**
 * Every legal transition of the browser's interaction state.
 *
 * Gesture callbacks translate input into these calls and do nothing else. That
 * is what makes the interaction model testable without composing anything, and
 * what lets Windows and Android demonstrably share one behaviour rather than two
 * implementations that merely look similar.
 */
object AmiiboInteraction {

    /**
     * A plain tap or click: browse, never inspect.
     *
     * The single rule the whole model rests on is `single = browse, double =
     * inspect`, so this deliberately cannot open anything. During selection mode
     * a tap means something else entirely, which is why it routes to
     * [toggleSelection] rather than quietly doing both.
     */
    fun activate(state: AmiiboInteractionState, id: String): AmiiboInteractionState =
        if (state.selecting) toggleSelection(state, id) else focus(state, id)

    /**
     * Move the highlight.
     *
     * MOVING FOCUS CLOSES AN OPEN INSPECTOR. The user explicitly asked to see A;
     * tapping B is a return to browsing, not a request to see B. Silently
     * swapping the sheet's contents would make the details surface follow every
     * highlight, which is the behaviour this pass exists to remove. B is
     * inspected by asking for it — double tap, double click, or the explicit
     * command.
     *
     * Re-focusing the item already being inspected leaves it open: that is the
     * first half of a double tap, not a request to close.
     */
    fun focus(state: AmiiboInteractionState, id: String): AmiiboInteractionState =
        state.copy(focusedId = id, inspectedId = if (state.inspectedId == id) id else null)

    /**
     * Explicitly open the details surface for one item.
     *
     * Refused during selection mode. A multi-selection and a single-item
     * inspector on screen together give every command in the inspector an
     * ambiguous scope — does Initialize mean this one or those twelve? — so the
     * two are mutually exclusive by construction.
     */
    fun openInspector(state: AmiiboInteractionState, id: String): AmiiboInteractionState =
        if (state.selecting) state else state.copy(focusedId = id, inspectedId = id)

    /** Dismiss the details surface. The browser keeps its place. */
    fun closeInspector(state: AmiiboInteractionState): AmiiboInteractionState =
        state.copy(inspectedId = null)

    /**
     * Long press, or the accessible "Select" action: begin a multi-selection.
     *
     * Entering selection closes the inspector for the reason given on
     * [openInspector]. Long-pressing while already selecting adds rather than
     * restarting, so a stray long press cannot discard a set the user has been
     * building.
     */
    fun enterSelection(state: AmiiboInteractionState, id: String): AmiiboInteractionState =
        if (state.isSelected(id)) state.copy(focusedId = id, inspectedId = null)
        else add(state, id)

    /**
     * Add or remove one item from the bulk set.
     *
     * Also the entry point when nothing is selected yet, which is what makes a
     * desktop Ctrl+click on the first item start a selection.
     */
    fun toggleSelection(state: AmiiboInteractionState, id: String): AmiiboInteractionState =
        if (state.isSelected(id)) {
            state.copy(
                focusedId = id,
                inspectedId = null,
                selection = canonical(state.selection - id),
            )
        } else {
            add(state, id)
        }

    /**
     * Leave selection mode. Browsing is otherwise undisturbed.
     *
     * Deliberately touches neither focus nor any query state: cancelling a
     * selection must not also lose the user's place, their search or their
     * filters.
     */
    fun clearSelection(state: AmiiboInteractionState): AmiiboInteractionState =
        state.copy(selection = emptyList())

    /**
     * Back, or Escape: undo the most specific thing that is open.
     *
     * One gesture with an unambiguous order, so the user never has to guess
     * which of two modes it will cancel. Selection is the more disruptive mode
     * and is dismissed first; the caller can tell nothing happened — and that
     * Back should navigate away instead — by comparing the result.
     */
    fun escape(state: AmiiboInteractionState): AmiiboInteractionState = when {
        state.selecting -> clearSelection(state)
        state.inspectorOpen -> closeInspector(state)
        else -> state
    }

    /**
     * Reconcile with a library that has gained or lost entries.
     *
     * Selection holds STABLE LIBRARY IDS, never positions or row objects, so
     * sorting, filtering and re-projecting cannot disturb it — only an entry
     * actually leaving the library can. This is the single place that happens,
     * and it also closes an inspector describing something that no longer
     * exists.
     */
    fun prune(state: AmiiboInteractionState, libraryIds: Collection<String>): AmiiboInteractionState {
        val live = libraryIds.toHashSet()
        return state.copy(
            focusedId = state.focusedId?.takeIf { it in live },
            inspectedId = state.inspectedId?.takeIf { it in live },
            selection = canonical(state.selection.filter { it in live }),
        )
    }

    /**
     * Where the highlight lands after entries are removed.
     *
     * The neighbour that took the removed item's place, so deleting from the
     * middle of a thousand-item library leaves the user where they were working
     * rather than at the top. Falls back to the last survivor when the removal
     * took everything from the end, and to nothing when it took everything.
     */
    fun focusAfterRemoval(
        ordered: List<String>,
        removed: Collection<String>,
        currentFocus: String?,
    ): String? {
        val gone = removed.toHashSet()
        val survivors = ordered.filter { it !in gone }
        if (survivors.isEmpty()) return null
        if (currentFocus != null && currentFocus !in gone) return currentFocus

        val firstRemoved = ordered.indexOfFirst { it in gone }
        if (firstRemoved < 0) return survivors.first()

        for (index in firstRemoved until ordered.size) {
            if (ordered[index] !in gone) return ordered[index]
        }
        return survivors.last()
    }

    /** Settle the browser after a bulk removal: prune, re-focus, stop selecting. */
    fun afterRemoval(
        state: AmiiboInteractionState,
        orderedBefore: List<String>,
        removed: Collection<String>,
    ): AmiiboInteractionState {
        val gone = removed.toHashSet()
        val focus = focusAfterRemoval(orderedBefore, gone, state.focusedId)
        return prune(
            state.copy(focusedId = focus, selection = emptyList()),
            orderedBefore.filter { it !in gone },
        )
    }

    private fun add(state: AmiiboInteractionState, id: String): AmiiboInteractionState = state.copy(
        focusedId = id,
        inspectedId = null,
        selection = canonical(state.selection + id),
    )

    /** One order for one set, so equality compares content and not history. */
    private fun canonical(ids: List<String>): List<String> = ids.distinct().sorted()
}

/**
 * What a bulk command actually did, item by item.
 *
 * A batch over a thousand-item library is not all-or-nothing: one dump can fail
 * to decrypt while the rest re-sign perfectly. Returning a bare success flag
 * would force the caller to either roll back work that succeeded or claim work
 * that did not, so the result carries both lists and the summary sentence is
 * derived from them rather than written at each call site.
 */
data class AmiiboBulkOutcome(
    val succeeded: List<String> = emptyList(),
    val failed: List<AmiiboBulkFailure> = emptyList(),
) {
    val total: Int get() = succeeded.size + failed.size

    val anyFailed: Boolean get() = failed.isNotEmpty()

    /**
     * One sentence stating exactly what happened, in the user's terms.
     *
     * Never rounds a partial result up to a success. When some entries failed
     * the sentence says how many of each, because "12 Amiibo initialized" after
     * three of them failed is a lie the user only discovers much later.
     *
     * @param verb past tense, e.g. "initialized" or "deleted".
     */
    fun summary(verb: String): String = when {
        // "Amiibo" is its own plural, so no count-dependent noun is needed.
        failed.isEmpty() -> "${succeeded.size} Amiibo $verb"
        succeeded.isEmpty() -> "No Amiibo $verb; ${failed.size} failed"
        else -> "${succeeded.size} of $total Amiibo $verb; ${failed.size} failed"
    }
}

/** One entry a bulk command could not process, and why. */
data class AmiiboBulkFailure(val id: String, val name: String, val reason: String)
