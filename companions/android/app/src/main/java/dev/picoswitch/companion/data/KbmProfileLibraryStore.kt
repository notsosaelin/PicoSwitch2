package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit

/**
 * Where the local KB/M profile library is kept.
 *
 * An interface so the library repository has no Android dependency and its rules
 * — which are the part that can be got subtly wrong — are covered by ordinary
 * JVM tests rather than only on a device. Mirrors
 * `TouchProfileLibraryStore`, which splits the same way for the same reason.
 */
interface KbmProfileLibraryStore {
    fun load(): KbmProfileLibrary
    fun save(library: KbmProfileLibrary)
}

/**
 * The library on Android, in app-private preferences.
 *
 * Deliberately thin, like [AdapterRegistryStore]: everything that can be got
 * wrong — schema, tolerance of a damaged document, refusal to invent a binding —
 * is in [KbmProfileLibraryCodec]. This class only decides *where* and *when*.
 *
 * ```text
 * kbm_profile_library/document     this store, schema 1
 * ```
 *
 * There is no legacy document to migrate: before this existed the app had no
 * local library at all, because it treated the adapter's six resident profiles
 * as the user's collection.
 */
class AndroidKbmProfileLibraryStore(context: Context) : KbmProfileLibraryStore {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    override fun load(): KbmProfileLibrary =
        KbmProfileLibraryCodec.decode(preferences.getString(KEY_DOCUMENT, null))

    override fun save(library: KbmProfileLibrary) {
        preferences.edit { putString(KEY_DOCUMENT, KbmProfileLibraryCodec.encode(library)) }
    }

    /** True when this install has a library document of its own; diagnostics only. */
    fun hasDocument(): Boolean = preferences.contains(KEY_DOCUMENT)

    companion object {
        internal const val FILE_NAME = "kbm_profile_library"
        private const val KEY_DOCUMENT = "document"
    }
}
