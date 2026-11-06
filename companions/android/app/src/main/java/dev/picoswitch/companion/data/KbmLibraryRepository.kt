package dev.picoswitch.companion.data

import dev.picoswitch.management.KbmBinding
import dev.picoswitch.management.KbmFingerprint
import dev.picoswitch.management.KbmLocalDraft
import dev.picoswitch.management.KbmMouseConfig
import dev.picoswitch.management.KbmProfile
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.UUID

/**
 * The local profile library: the ONLY thing Save writes to.
 *
 * THE RULE THIS TYPE ENFORCES: nothing here talks to the adapter. Not create,
 * not duplicate, not rename, not delete, not save, not discard. It has no
 * [dev.picoswitch.management.ManagementClient], no transport, and cannot acquire
 * one — which is what makes "zero adapter writes while editing" a structural
 * property rather than a convention someone has to remember.
 *
 * Sending a local profile to the adapter is a separate, explicit act that lives
 * on [AdapterRepository], where the management session is.
 *
 * Every operation here works with nothing paired. That is the point: the library
 * belongs to the user, not to a device.
 */
class KbmLibraryRepository(private val store: KbmProfileLibraryStore) {
    private val _library = MutableStateFlow(store.load())
    val library: StateFlow<KbmProfileLibrary> = _library.asStateFlow()

    val value: KbmProfileLibrary get() = _library.value

    /** Create a profile from a layout's canonical Default. */
    fun create(
        layout: KbmProfile,
        name: String,
        bindings: List<KbmBinding> = emptyList(),
        mouse: KbmMouseConfig = KbmMouseConfig(),
        now: Long = System.currentTimeMillis(),
    ): KbmLocalProfile {
        val overrides = KbmFingerprint.canonical(bindings)
        val profile = KbmLocalProfile(
            // A fresh UUID, never derived from the name or from any adapter
            // identity: renaming must not change identity, and deleting then
            // recreating must not alias the old one.
            id = UUID.randomUUID().toString().replace("-", ""),
            layout = layout,
            name = name,
            bindings = overrides,
            mouse = mouse,
            fingerprint = KbmFingerprint.compute(layout, overrides, mouse),
            modifiedMillis = now,
        )
        commit(value.with(profile))
        return profile
    }

    /**
     * SAVE. Local persistence only, and the whole point of the draft model.
     *
     * This runs while an older copy of the same profile may be resident on the
     * adapter, and it deliberately leaves that copy alone. The bank projection
     * reports the divergence; only an explicit adapter action resolves it.
     * Conflating the two is what made every keystroke a flash erase.
     */
    fun save(
        id: String,
        name: String,
        bindings: List<KbmBinding>,
        mouse: KbmMouseConfig,
        now: Long = System.currentTimeMillis(),
    ): KbmLocalProfile {
        val existing = value.find(id)
        val overrides = KbmFingerprint.canonical(bindings)
        val layout = existing?.layout ?: KbmProfile.Keyboard
        val profile = KbmLocalProfile(
            id = id,
            layout = layout,
            name = name,
            bindings = overrides,
            mouse = mouse,
            fingerprint = KbmFingerprint.compute(layout, overrides, mouse),
            modifiedMillis = now,
        )
        commit(value.with(profile))
        return profile
    }

    /** Persist an open draft. Creates when the draft is on the built-in template. */
    fun save(draft: KbmLocalDraft, now: Long = System.currentTimeMillis()): KbmLocalProfile =
        if (draft.isBuiltin) {
            create(
                layout = draft.layout,
                name = value.suggestName(draft.layout, draft.name),
                bindings = draft.overrides,
                mouse = draft.mouse,
                now = now,
            )
        } else {
            save(draft.profileId, draft.name, draft.overrides, draft.mouse, now)
        }

    /** A copy under a new identity, so edits to it cannot reach the original. */
    fun duplicate(id: String, name: String, now: Long = System.currentTimeMillis()): KbmLocalProfile? {
        val source = value.find(id) ?: return null
        return create(source.layout, name, source.bindings, source.mouse, now)
    }

    fun rename(id: String, name: String, now: Long = System.currentTimeMillis()): KbmLocalProfile? {
        val existing = value.find(id) ?: return null
        // Identity and fingerprint are untouched: a rename changes no behaviour,
        // so an adapter copy that matched before still matches.
        val renamed = existing.copy(name = name, modifiedMillis = now)
        commit(value.with(renamed))
        return renamed
    }

    /**
     * Remove from the LIBRARY. Any copy resident on the adapter survives.
     *
     * The resident copy is a separate snapshot the adapter owns and may be
     * running right now. Deleting it here would change console behaviour from a
     * library operation, which is exactly the coupling this model removes.
     */
    fun delete(id: String): Boolean {
        if (value.find(id) == null) return false
        commit(value.without(id))
        return true
    }

    /**
     * Import a profile that is resident on the adapter into this library.
     *
     * The cross-platform bridge: a Windows-created profile reaches Android only
     * as an adapter resident copy, and vice versa. Local ids are NOT shared
     * between platforms, so the match is by CONTENT — same layout, same
     * fingerprint. Without that, every reconnect would add another duplicate.
     */
    fun import(
        layout: KbmProfile,
        name: String,
        bindings: List<KbmBinding>,
        mouse: KbmMouseConfig,
        now: Long = System.currentTimeMillis(),
    ): KbmLocalProfile {
        val overrides = KbmFingerprint.canonical(bindings)
        val fingerprint = KbmFingerprint.compute(layout, overrides, mouse)

        value.forLayout(layout).firstOrNull { it.fingerprint == fingerprint }
            ?.let { return it }

        return create(layout, value.suggestName(layout, name), overrides, mouse, now)
    }

    private fun commit(updated: KbmProfileLibrary) {
        _library.value = updated
        store.save(updated)
    }
}

/** Open a local profile in the editor. Zero adapter traffic. */
fun KbmLocalProfile.toDraft(): KbmLocalDraft = KbmLocalDraft(
    profileId = id,
    layout = layout,
    name = name,
    overrides = bindings,
    mouse = mouse,
    baseName = name,
    baseOverrides = bindings,
    baseMouse = mouse,
)

/** Rebase an open draft after a local save. */
fun KbmLocalDraft.rebased(saved: KbmLocalProfile): KbmLocalDraft = copy(
    profileId = saved.id,
    name = saved.name,
    overrides = saved.bindings,
    mouse = saved.mouse,
    baseName = saved.name,
    baseOverrides = saved.bindings,
    baseMouse = saved.mouse,
)
