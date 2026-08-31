package dev.picoswitch.management

/**
 * The editor's working copy of a LOCAL library profile.
 *
 * REPLACES [KbmDraft] ON THE EDITING PATH, and the difference is the whole
 * correction: that type is keyed on an ADAPTER profile id and its save is a
 * staged management transaction. Editing through it made "the profile I have
 * open" and "the profile resident on the adapter" the same object, so creating a
 * profile wrote to flash and saving one changed what the console might run.
 *
 * This is keyed on the LOCAL id — a UUID no adapter has ever seen. It can be
 * created, edited, saved and discarded with nothing connected, and it holds no
 * adapter state at all. Getting content onto the adapter is a separate, explicit
 * assignment.
 *
 * [KbmDraft] survives for the staged upload itself, which is still exactly the
 * right mechanism for that job.
 *
 * NOTE ON STATE. The only states this type has are clean and [dirty]. Everything
 * else a user is told — on the adapter, adapter copy out of date, active — is a
 * RELATIONSHIP between a local profile and the bank, not a property of the
 * draft, and lives in the bank projection. Folding those into the draft is what
 * produced a single flag that could not distinguish "edited locally" from
 * "saved but the adapter still has the old copy".
 */
data class KbmLocalDraft(
    /** The local library id, or empty while editing the built-in template. */
    val profileId: String,
    val layout: KbmProfile,
    val name: String,
    /**
     * Sparse overrides, as stored. The full mapping is composed on demand from
     * [KbmDefaults], so the draft never carries a copy of the canonical table.
     */
    val overrides: List<KbmBinding> = emptyList(),
    val mouse: KbmMouseConfig = KbmMouseConfig(),
    // What was last saved, so dirty is a comparison rather than a flag.
    val baseName: String,
    val baseOverrides: List<KbmBinding> = emptyList(),
    val baseMouse: KbmMouseConfig = KbmMouseConfig(),
) {
    /**
     * Editing the built-in template, which is not a library profile.
     *
     * Default is read-only: saving it creates a NEW local profile instead, which
     * is what keeps the template always available as a starting point.
     */
    val isBuiltin: Boolean get() = profileId.isEmpty()

    /**
     * Compared on CONTENT rather than tracked with a flag, so a user who edits a
     * key and puts it back is correctly clean again.
     */
    val dirty: Boolean
        get() = name != baseName || mouse != baseMouse || fingerprint != baseFingerprint

    val fingerprint: Long
        get() = KbmFingerprint.compute(layout, KbmFingerprint.canonical(overrides), mouse)

    val baseFingerprint: Long
        get() = KbmFingerprint.compute(layout, KbmFingerprint.canonical(baseOverrides), baseMouse)

    /** The full mapping the grid draws, composed from the defaults. */
    val effective: List<KbmBinding> get() = KbmDefaults.effective(layout, overrides)

    /** Rebind one input. Local only; zero adapter traffic. */
    fun with(source: KbmSource, destination: KbmDestination): KbmLocalDraft {
        val kept = overrides.filterNot { it.source == source }.toMutableList()

        // An override equal to the layout's canonical default is DROPPED rather
        // than stored, so putting a key back really does return the profile to
        // clean. None is never dropped: "does nothing" is a real answer that
        // differs from the default the adapter would otherwise apply.
        val canonical = KbmDefaults.forLayout(layout).bindings
            .firstOrNull { it.source == source }
        if (canonical == null || canonical.destination != destination) {
            kept += KbmBinding(source, destination, custom = true)
        }

        return copy(overrides = kept)
    }

    /** Restore one input to the layout's canonical default. */
    fun restore(source: KbmSource): KbmLocalDraft =
        copy(overrides = overrides.filterNot { it.source == source })

    fun withName(value: String): KbmLocalDraft = copy(name = value)

    fun withMouse(value: KbmMouseConfig): KbmLocalDraft = copy(mouse = value)

    /** Throw the local edits away. Zero adapter traffic. */
    fun discard(): KbmLocalDraft =
        copy(name = baseName, overrides = baseOverrides, mouse = baseMouse)

    companion object {
        /** A draft on the built-in template of a layout. No adapter needed. */
        fun fromDefault(layout: KbmProfile): KbmLocalDraft {
            val mouse = KbmDefaults.forLayout(layout).mouse
            return KbmLocalDraft(
                profileId = "",
                layout = layout,
                name = "Default",
                baseName = "Default",
                mouse = mouse,
                baseMouse = mouse,
            )
        }
    }
}
