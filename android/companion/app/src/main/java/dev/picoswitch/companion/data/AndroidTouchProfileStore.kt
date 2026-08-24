package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit
import dev.picoswitch.bridge.touch.TouchOverrideDecodeResult
import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.bridge.touch.TouchProfileLibrary
import dev.picoswitch.bridge.touch.TouchProfileLibraryEditor
import dev.picoswitch.bridge.touch.TouchProfileLibraryJsonCodec
import dev.picoswitch.bridge.touch.TouchProfileLibraryDecodeResult
import dev.picoswitch.bridge.touch.TouchProfileLibraryLoad
import dev.picoswitch.bridge.touch.TouchProfileLibraryStore

/**
 * One versioned, app-private profile-library document per personality.
 *
 * The factory profile is never written here. It is synthesized from the shipped
 * template on every read, so an absent, truncated or unreadable document leaves
 * the user with the official controller rather than with none.
 *
 * ```text
 * touch_layout_profiles/profiles_<personality>   this store, current
 * touch_layout_overrides/profile_<personality>   pre-profile, read once on upgrade
 * ```
 */
class AndroidTouchProfileStore(context: Context) : TouchProfileLibraryStore {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
    private val legacy = AndroidTouchLayoutOverrideStore(context)

    override fun load(personality: TouchProfileId): TouchProfileLibraryLoad {
        val raw = preferences.getString(key(personality), null)
            ?: return migrateLegacy(personality)
        return when (val decoded = TouchProfileLibraryJsonCodec.decode(raw, personality)) {
            is TouchProfileLibraryDecodeResult.Valid -> TouchProfileLibraryLoad(decoded.value)
            // Deliberately not deleted: a later app may understand it, and the
            // factory profile needs nothing from storage in the meantime.
            is TouchProfileLibraryDecodeResult.Invalid -> TouchProfileLibraryLoad(
                TouchProfileLibrary.empty(personality),
                decoded.problem,
            )
        }
    }

    override fun save(library: TouchProfileLibrary) {
        preferences.edit {
            putString(key(library.personality), TouchProfileLibraryJsonCodec.encode(library))
        }
    }

    /**
     * Adopt the single pre-profile override, once.
     *
     * The first release stored one anonymous override per personality. Ignoring
     * it on upgrade would silently discard every layout anybody had already
     * tuned, so it becomes an ordinary named profile and is selected — which is
     * exactly the controller the user last had on screen. The old document is
     * left in place rather than deleted; the new one is written immediately, so
     * this path runs at most once per personality.
     */
    private fun migrateLegacy(personality: TouchProfileId): TouchProfileLibraryLoad {
        val stored = legacy.load(personality)
        val override = (stored as? TouchOverrideDecodeResult.Valid)?.value
        val warning = (stored as? TouchOverrideDecodeResult.Invalid)?.problem
        if (override == null || override.controls.isEmpty()) {
            return TouchProfileLibraryLoad(TouchProfileLibrary.empty(personality), warning)
        }
        val library = TouchProfileLibraryEditor.adoptLegacyOverride(
            personality = personality,
            override = override,
            nowEpochMs = System.currentTimeMillis(),
        )
        save(library)
        return TouchProfileLibraryLoad(library)
    }

    private fun key(personality: TouchProfileId) = "profiles_${personality.key}"

    companion object { internal const val FILE_NAME = "touch_layout_profiles" }
}
