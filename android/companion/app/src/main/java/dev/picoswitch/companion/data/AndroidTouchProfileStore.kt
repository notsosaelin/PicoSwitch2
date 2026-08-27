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
 * touch_layout_profiles/profiles_<personality>          this store, schema 2
 * touch_layout_profiles/profiles_<personality>.v1       kept once, on migration
 * touch_layout_overrides/profile_<personality>          pre-profile, read on upgrade
 * ```
 *
 * ## Migration safety
 *
 * Two schema-1 shapes can be found here, and both are migrated on READ and only
 * written back once the migrated library exists in memory:
 *
 * 1. a schema-1 profile library, decoded and migrated by the codec;
 * 2. the very first release's single anonymous override document.
 *
 * The original raw text is copied to a `.v1` key BEFORE the migrated document
 * replaces it, and the pre-profile override file is never touched at all. A
 * migration that produced something unusable therefore costs nothing that
 * cannot be read back out of storage by hand.
 */
class AndroidTouchProfileStore(context: Context) : TouchProfileLibraryStore {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
    private val legacy = AndroidTouchLayoutOverrideStore(context)

    override fun load(personality: TouchProfileId): TouchProfileLibraryLoad {
        val raw = preferences.getString(key(personality), null)
            ?: return migrateLegacyOverride(personality)
        return when (val decoded = TouchProfileLibraryJsonCodec.decode(raw, personality)) {
            is TouchProfileLibraryDecodeResult.Valid -> {
                // Persist the migrated form immediately, keeping the original
                // beside it. Doing this on read rather than on the next save
                // means the upgrade completes even for a user who never opens
                // the editor again.
                if (decoded.migrated) adoptMigrated(personality, raw, decoded.value)
                TouchProfileLibraryLoad(decoded.value)
            }
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

    private fun adoptMigrated(
        personality: TouchProfileId,
        rawBefore: String,
        library: TouchProfileLibrary,
    ) {
        val migrated = TouchProfileLibraryJsonCodec.encode(library)
        preferences.edit {
            // Backup first, in the same atomic commit as the replacement, so the
            // two can never disagree about which document is the original.
            putString(backupKey(personality), rawBefore)
            putString(key(personality), migrated)
        }
    }

    /**
     * Adopt the single pre-profile override, once.
     *
     * The first release stored one anonymous sparse override per personality.
     * Ignoring it on upgrade would silently discard every layout anybody had
     * already tuned, so it is migrated to an instance document, becomes an
     * ordinary named profile and is selected — which is exactly the controller
     * the user last had on screen. The old document is left in place rather than
     * deleted; the new one is written immediately, so this path runs at most once
     * per personality.
     */
    private fun migrateLegacyOverride(personality: TouchProfileId): TouchProfileLibraryLoad {
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

    private fun backupKey(personality: TouchProfileId) = "${key(personality)}$BACKUP_SUFFIX"

    companion object {
        internal const val FILE_NAME = "touch_layout_profiles"

        /** Where the pre-migration document is kept; read by hand, never by the app. */
        internal const val BACKUP_SUFFIX = ".v1"
    }
}
