package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit

/**
 * Where per-adapter controller history lives on disk.
 *
 * Deliberately thin, exactly like [AdapterRegistryStore]: schema, tolerance of a
 * damaged document and every merge rule live in [PeerHistoryCodec] and
 * [AdapterPeerHistory], which have no Android dependency and are covered by
 * ordinary JVM tests. This class decides only *where* and *when*.
 *
 * ```text
 * peer_history/document        this store, schema 1
 * ```
 *
 * There is no legacy document to migrate: history is new in Phase 4, and an
 * install with none simply starts remembering from its next inventory read.
 */
class PeerHistoryStore(context: Context) {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun load(): PeerHistoryBook = PeerHistoryCodec.decode(preferences.getString(KEY_DOCUMENT, null))

    fun save(book: PeerHistoryBook) {
        preferences.edit { putString(KEY_DOCUMENT, PeerHistoryCodec.encode(book)) }
    }

    companion object {
        internal const val FILE_NAME = "peer_history"
        private const val KEY_DOCUMENT = "document"
    }
}
