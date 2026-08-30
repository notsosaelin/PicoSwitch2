package dev.picoswitch.management

/**
 * What the editor is doing, from the user's point of view.
 *
 * The three that matter and are easy to conflate:
 *
 * - [Dirty] — edited locally. NOTHING has been sent to the adapter.
 * - [SavedNotApplied] — the adapter stored it, and the console is still running
 *   the old mapping. This state exists because Save and Apply are different
 *   acts, and a UI that cannot show it will lie.
 * - [Active] — the realized mapping matches this profile's saved content.
 */
enum class KbmDraftState {
    /** Draft equals the adapter's saved profile, which is not applied. */
    Clean,

    /** Draft equals the saved profile, and that is what is running. */
    Active,

    /** Edited locally. Zero adapter writes have happened. */
    Dirty,

    /** Saved to the library; the console still runs something else. */
    SavedNotApplied,

    /** The adapter's profile moved on since this draft was based on it. */
    Conflict,

    /** No live session. Nothing here may be presented as live truth. */
    Disconnected,
}

/**
 * A local, editable copy of one profile.
 *
 * The reason this type exists: the previous editor sent `kbm bind` on every
 * keystroke, which erased flash once per changed key and made Save and Discard
 * meaningless — there was nothing to discard, because it had already happened.
 * Every edit here is a pure transformation, and nothing in this file talks to an
 * adapter.
 *
 * Deliberately in management-core rather than the app: Windows and Android must
 * agree about what dirty, conflicted and applied MEAN, and the rules are easier
 * to keep identical when they sit beside the wire types they are derived from.
 */
data class KbmDraft(
    val profileId: Int,
    val layout: KbmProfile,
    val baseRevision: Int,
    val name: String,
    val bindings: List<KbmBinding>,
    val mouse: KbmMouseConfig,
    val baseName: String,
    val baseBindings: List<KbmBinding>,
    val baseMouse: KbmMouseConfig,
) {
    /**
     * True when the draft differs from what the adapter has stored.
     *
     * Compared on CONTENT rather than tracked with a flag, so a user who edits a
     * key and then puts it back is correctly clean again and Save stays disabled.
     */
    val dirty: Boolean
        get() = name != baseName ||
            mouse != baseMouse ||
            canonical(bindings) != canonical(baseBindings)

    /** Editing the built-in template. Save must offer "create" instead. */
    val isBuiltin: Boolean get() = profileId == KbmProfileIds.DEFAULT

    /** Rebind one input. Zero adapter writes. */
    fun with(source: KbmSource, destination: KbmDestination): KbmDraft {
        // NONE is a real, storable answer — "this key does nothing" — and is kept
        // rather than dropped, because dropping it would restore the adapter's
        // canonical default instead.
        val kept = bindings.filterNot { it.source == source } +
            KbmBinding(source, destination, custom = true)
        return copy(bindings = kept)
    }

    fun withName(value: String): KbmDraft = copy(name = value)

    fun withMouse(value: KbmMouseConfig): KbmDraft = copy(mouse = value)

    /** Throw the local edits away. Zero adapter writes. */
    fun discard(): KbmDraft =
        copy(name = baseName, bindings = baseBindings, mouse = baseMouse)

    /**
     * Adopt what the adapter now reports, after a successful save or a reload.
     * The draft becomes clean against the new revision.
     */
    fun rebased(profileId: Int, revision: Int, name: String): KbmDraft = copy(
        profileId = profileId,
        baseRevision = revision,
        name = name,
        baseName = name,
        baseBindings = bindings,
        baseMouse = mouse,
    )

    /**
     * Where this draft stands, given what the adapter currently reports.
     *
     * Deliberately computed from adapter truth on every call rather than latched:
     * a cached "Active" flag is exactly the lie this model exists to prevent, and
     * it goes stale the moment another companion applies something.
     */
    fun stateAgainst(adapter: KbmProfiles, connected: Boolean): KbmDraftState {
        if (!connected) return KbmDraftState.Disconnected
        if (dirty) return KbmDraftState.Dirty

        val saved = adapter.find(profileId)
        if (!isBuiltin && saved != null && saved.revision != baseRevision) {
            // Someone else saved this profile while this draft was open.
            return KbmDraftState.Conflict
        }

        val active = adapter.activeFor(layout) ?: return KbmDraftState.Clean

        // Both halves matter. The id says which profile produced the realized
        // mapping; matchesSaved says whether that profile has been edited since.
        // An id match alone is what would let the UI claim "Active" for a profile
        // that was saved and never applied.
        return if (active.sourceId == profileId && active.matchesSaved) {
            KbmDraftState.Active
        } else {
            KbmDraftState.SavedNotApplied
        }
    }

    companion object {
        fun from(
            profile: KbmProfileInfo,
            bindings: List<KbmBinding>,
            mouse: KbmMouseConfig,
        ): KbmDraft = KbmDraft(
            profileId = profile.id,
            layout = profile.layout,
            baseRevision = profile.revision,
            name = profile.name,
            bindings = bindings,
            mouse = mouse,
            baseName = profile.name,
            baseBindings = bindings,
            baseMouse = mouse,
        )

        private fun canonical(bindings: List<KbmBinding>): List<KbmBinding> =
            bindings
                .sortedWith(compareBy({ it.source.kind }, { it.source.code }))
                .map { it.copy(custom = true) }
    }
}
