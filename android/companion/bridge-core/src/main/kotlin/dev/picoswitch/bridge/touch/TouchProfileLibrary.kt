package dev.picoswitch.bridge.touch

/**
 * A named set of user customizations for one console-facing controller.
 *
 * ```text
 * TouchProfileId (personality)
 *        |
 *        v
 * TouchLayoutTemplate      immutable, shipped
 *        |
 *        v
 * TouchLayoutProfile.override    sparse, user-owned
 *        |
 *        v
 * TouchLayoutComposer -> TouchLayoutResolver -> ResolvedTouchLayout
 * ```
 *
 * A profile is an ENVELOPE around the existing [TouchLayoutOverride] document,
 * not a second layout representation. The personality, template identity and
 * template revision a profile was authored against are read back off that
 * override rather than stored again beside it: two copies of the same fact drift,
 * and the composer already refuses an override whose template identity does not
 * match the shipped one. The design specification names those fields on the
 * profile, and they are present here — as derived properties.
 */
data class TouchLayoutProfile(
    val id: String,
    val name: String,
    /** The sparse user document. For the factory profile this is always empty. */
    val override: TouchLayoutOverride,
    val metadata: TouchProfileMetadata = TouchProfileMetadata(),
) {
    val personality: TouchProfileId get() = override.profileId
    val templateId: String get() = override.templateId
    val templateRevision: Int get() = override.basedOnRevision

    /**
     * Factory profiles are the shipped defaults.
     *
     * Identified structurally by [TouchProfileLibrary.FACTORY_PROFILE_ID] rather
     * than by a mutable flag, because the protection this identity carries —
     * cannot be renamed, overwritten or deleted — must not be something a stored
     * document can turn off.
     */
    val isFactory: Boolean get() = id == TouchProfileLibrary.FACTORY_PROFILE_ID

    /** True when the profile changes nothing about the shipped template. */
    val isPristine: Boolean get() = override.controls.isEmpty()
}

/**
 * Bookkeeping that is not layout.
 *
 * [gameKey] is reserved for the per-game profiles named as future work in the
 * editor design. Nothing in this build writes or reads it for selection; it
 * exists so that adding automatic selection later does not require a schema
 * migration of every stored profile.
 */
data class TouchProfileMetadata(
    val createdAtEpochMs: Long = 0L,
    val updatedAtEpochMs: Long = 0L,
    val gameKey: String? = null,
)

/**
 * Every profile available for one personality, plus which one is active.
 *
 * The factory profile is NOT a member of [userProfiles] and is never persisted.
 * It is synthesized from the shipped template on every read, which is what makes
 * "cannot be overwritten, cannot be deleted, always available" a property of the
 * type instead of a rule some call site has to remember. A corrupt or truncated
 * stored document therefore degrades to the shipped controller rather than to a
 * controller with no layout at all.
 */
data class TouchProfileLibrary(
    val personality: TouchProfileId,
    val userProfiles: List<TouchLayoutProfile> = emptyList(),
    val selectedProfileId: String = FACTORY_PROFILE_ID,
) {
    val factoryProfile: TouchLayoutProfile = TouchLayoutProfile(
        id = FACTORY_PROFILE_ID,
        name = FACTORY_PROFILE_NAME,
        override = TouchLayoutEditor.empty(TouchProfileCatalog.require(personality)),
    )

    /** Factory first, then user profiles in creation order. */
    val profiles: List<TouchLayoutProfile> = listOf(factoryProfile) + userProfiles

    val selected: TouchLayoutProfile =
        profiles.firstOrNull { it.id == selectedProfileId } ?: factoryProfile

    /** The override the runtime should compose with; `null` means the shipped default. */
    val activeOverride: TouchLayoutOverride? = selected.override.takeIf { it.controls.isNotEmpty() }

    fun profile(id: String): TouchLayoutProfile? = profiles.firstOrNull { it.id == id }

    companion object {
        /** Reserved id; a stored user profile may never claim it. */
        const val FACTORY_PROFILE_ID = "factory-default"
        const val FACTORY_PROFILE_NAME = "Default"

        /**
         * Enough for the per-game sets this architecture anticipates, small
         * enough that the profile picker never becomes a scrolling list on a
         * phone in landscape.
         */
        const val MAX_USER_PROFILES = 12
        const val MAX_NAME_LENGTH = 32

        fun empty(personality: TouchProfileId) = TouchProfileLibrary(personality)
    }
}

/** Outcome of a library edit; a rejection explains itself rather than silently no-op'ing. */
sealed interface TouchProfileEdit {
    data class Applied(val library: TouchProfileLibrary, val profileId: String) : TouchProfileEdit
    data class Rejected(val reason: String) : TouchProfileEdit
}

/**
 * Pure profile-library operations, shared by every host that has an editor.
 *
 * Kept beside [TouchLayoutEditor] and in the same platform-neutral module for
 * the same reason: profile protection, naming and identity are rules about the
 * user's data, and rules that live in a UI layer are rules that hold only on the
 * platform where somebody happened to write them.
 */
object TouchProfileLibraryEditor {

    fun select(library: TouchProfileLibrary, profileId: String): TouchProfileEdit =
        when (library.profile(profileId)) {
            null -> TouchProfileEdit.Rejected("That layout profile no longer exists")
            else -> TouchProfileEdit.Applied(library.copy(selectedProfileId = profileId), profileId)
        }

    /** A new empty profile: identical to the shipped default until it is edited. */
    fun create(
        library: TouchProfileLibrary,
        name: String,
        nowEpochMs: Long,
    ): TouchProfileEdit = insert(
        library,
        name,
        TouchLayoutEditor.empty(TouchProfileCatalog.require(library.personality)),
        nowEpochMs,
    )

    /**
     * Copy an existing profile, including the factory one.
     *
     * Duplicating the factory profile is how a user starts from the official
     * layout without endangering it, so it is explicitly allowed even though the
     * source itself can never be written.
     */
    fun duplicate(
        library: TouchProfileLibrary,
        sourceId: String,
        name: String? = null,
        nowEpochMs: Long,
    ): TouchProfileEdit {
        val source = library.profile(sourceId)
            ?: return TouchProfileEdit.Rejected("That layout profile no longer exists")
        return insert(library, name ?: source.name, source.override, nowEpochMs)
    }

    fun rename(library: TouchProfileLibrary, profileId: String, name: String): TouchProfileEdit {
        val target = library.userProfiles.firstOrNull { it.id == profileId }
            ?: return TouchProfileEdit.Rejected(
                if (profileId == TouchProfileLibrary.FACTORY_PROFILE_ID) FACTORY_PROTECTED
                else "That layout profile no longer exists",
            )
        val clean = uniqueName(sanitizeName(name), library.userProfiles, exceptId = profileId)
        return TouchProfileEdit.Applied(
            library.copy(
                userProfiles = library.userProfiles.map {
                    if (it.id == profileId) it.copy(name = clean) else it
                },
            ),
            target.id,
        )
    }

    fun delete(library: TouchProfileLibrary, profileId: String): TouchProfileEdit {
        if (profileId == TouchProfileLibrary.FACTORY_PROFILE_ID) {
            return TouchProfileEdit.Rejected(FACTORY_PROTECTED)
        }
        if (library.userProfiles.none { it.id == profileId }) {
            return TouchProfileEdit.Rejected("That layout profile no longer exists")
        }
        val remaining = library.userProfiles.filterNot { it.id == profileId }
        // Deleting the active profile must land somewhere that certainly exists.
        // The factory profile is the only such place.
        val selected = if (library.selectedProfileId == profileId) {
            TouchProfileLibrary.FACTORY_PROFILE_ID
        } else {
            library.selectedProfileId
        }
        return TouchProfileEdit.Applied(
            library.copy(userProfiles = remaining, selectedProfileId = selected),
            selected,
        )
    }

    /**
     * Store an edited override into a profile.
     *
     * Saving onto the factory profile does not fail and does not overwrite it:
     * it creates a new user profile carrying the edit and selects that. Refusing
     * outright would mean discarding work the user just did, and overwriting
     * would destroy the one layout that is always supposed to be recoverable.
     */
    fun save(
        library: TouchProfileLibrary,
        profileId: String,
        override: TouchLayoutOverride,
        nowEpochMs: Long,
        newProfileName: String = DEFAULT_NEW_PROFILE_NAME,
    ): TouchProfileEdit {
        val template = TouchProfileCatalog.require(library.personality).defaultTemplate
        if (override.profileId != library.personality || override.templateId != template.id) {
            return TouchProfileEdit.Rejected("That layout belongs to another controller")
        }
        if (profileId == TouchProfileLibrary.FACTORY_PROFILE_ID) {
            return insert(library, newProfileName, override, nowEpochMs)
        }
        val target = library.userProfiles.firstOrNull { it.id == profileId }
            ?: return TouchProfileEdit.Rejected("That layout profile no longer exists")
        return TouchProfileEdit.Applied(
            library.copy(
                userProfiles = library.userProfiles.map {
                    if (it.id != profileId) it
                    else it.copy(
                        override = override,
                        metadata = it.metadata.copy(updatedAtEpochMs = nowEpochMs),
                    )
                },
                selectedProfileId = profileId,
            ),
            target.id,
        )
    }

    /**
     * Drop a user profile's customizations without deleting the profile.
     *
     * On the factory profile this is already true, so it succeeds and changes
     * nothing — "Reset to default" should never report an error.
     */
    fun resetToDefault(
        library: TouchProfileLibrary,
        profileId: String,
        nowEpochMs: Long,
    ): TouchProfileEdit {
        if (profileId == TouchProfileLibrary.FACTORY_PROFILE_ID) {
            return TouchProfileEdit.Applied(library, profileId)
        }
        val empty = TouchLayoutEditor.empty(TouchProfileCatalog.require(library.personality))
        return save(library, profileId, empty, nowEpochMs)
    }

    /** Adopt an imported document as a new profile of this personality. */
    fun import(
        library: TouchProfileLibrary,
        profile: TouchLayoutProfile,
        nowEpochMs: Long,
    ): TouchProfileEdit {
        if (profile.personality != library.personality) {
            return TouchProfileEdit.Rejected("That layout was exported for another controller")
        }
        return insert(library, profile.name, profile.override, nowEpochMs)
    }

    /**
     * Adopt the single pre-profile override document as a user profile.
     *
     * The first release stored exactly one override per personality with no name
     * and no identity. Discarding it on upgrade would silently throw away every
     * layout anybody had already tuned, so it becomes a normal profile and is
     * selected, which is what the user last saw.
     */
    fun adoptLegacyOverride(
        personality: TouchProfileId,
        override: TouchLayoutOverride,
        nowEpochMs: Long,
        name: String = LEGACY_PROFILE_NAME,
    ): TouchProfileLibrary {
        val library = TouchProfileLibrary.empty(personality)
        if (override.controls.isEmpty() || override.profileId != personality) return library
        return when (val edit = insert(library, name, override, nowEpochMs)) {
            is TouchProfileEdit.Applied -> edit.library
            is TouchProfileEdit.Rejected -> library
        }
    }

    private fun insert(
        library: TouchProfileLibrary,
        name: String,
        override: TouchLayoutOverride,
        nowEpochMs: Long,
    ): TouchProfileEdit {
        if (library.userProfiles.size >= TouchProfileLibrary.MAX_USER_PROFILES) {
            return TouchProfileEdit.Rejected(
                "This controller already has ${TouchProfileLibrary.MAX_USER_PROFILES} layout profiles",
            )
        }
        val id = allocateId(library, nowEpochMs)
        val profile = TouchLayoutProfile(
            id = id,
            name = uniqueName(sanitizeName(name), library.userProfiles, exceptId = null),
            override = override,
            metadata = TouchProfileMetadata(
                createdAtEpochMs = nowEpochMs,
                updatedAtEpochMs = nowEpochMs,
            ),
        )
        return TouchProfileEdit.Applied(
            library.copy(
                userProfiles = library.userProfiles + profile,
                selectedProfileId = id,
            ),
            id,
        )
    }

    /**
     * Ids are derived, not random.
     *
     * A pure function of the library and the clock keeps every operation here
     * testable without injecting a generator, and the collision suffix means two
     * profiles created inside the same millisecond still get distinct identities.
     */
    internal fun allocateId(library: TouchProfileLibrary, nowEpochMs: Long): String {
        val base = "p" + nowEpochMs.coerceAtLeast(0L).toString(36)
        val taken = library.profiles.mapTo(mutableSetOf()) { it.id }
        if (base !in taken) return base
        var suffix = 2
        while ("$base-$suffix" in taken) suffix++
        return "$base-$suffix"
    }

    internal fun sanitizeName(raw: String): String {
        val collapsed = raw.trim().replace(WHITESPACE, " ")
            .filter { it.isLetterOrDigit() || it == ' ' || it in "-_()+." }
            .take(TouchProfileLibrary.MAX_NAME_LENGTH)
            .trim()
        return collapsed.ifBlank { DEFAULT_NEW_PROFILE_NAME }
    }

    /**
     * Names are for the user's benefit, so two identical ones are a defect even
     * though ids stay unique. The factory name is reserved as well: a second
     * "Default" in the picker would make the protected profile unidentifiable.
     */
    internal fun uniqueName(
        candidate: String,
        existing: List<TouchLayoutProfile>,
        exceptId: String?,
    ): String {
        val taken = existing.filterNot { it.id == exceptId }.mapTo(mutableSetOf()) { it.name } +
            TouchProfileLibrary.FACTORY_PROFILE_NAME
        if (candidate !in taken) return candidate
        var suffix = 2
        while (true) {
            val room = TouchProfileLibrary.MAX_NAME_LENGTH - (" $suffix".length)
            val next = candidate.take(room).trim() + " $suffix"
            if (next !in taken) return next
            suffix++
        }
    }

    private val WHITESPACE = Regex("\\s+")
    private const val FACTORY_PROTECTED = "The default layout cannot be changed"
    const val DEFAULT_NEW_PROFILE_NAME = "Custom"
    const val LEGACY_PROFILE_NAME = "My layout"
}

/**
 * Storage boundary implemented by each host.
 *
 * A document that cannot be understood is reported, never deleted: a later build
 * may understand it, and the runtime is safe in the meantime because the factory
 * profile needs nothing from storage.
 */
interface TouchProfileLibraryStore {
    fun load(personality: TouchProfileId): TouchProfileLibraryLoad
    fun save(library: TouchProfileLibrary)
}

/** What a host's storage produced, and what to tell the user if it was not usable. */
data class TouchProfileLibraryLoad(
    val library: TouchProfileLibrary,
    val warning: String? = null,
)
