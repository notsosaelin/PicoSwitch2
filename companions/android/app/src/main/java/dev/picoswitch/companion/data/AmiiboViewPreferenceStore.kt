package dev.picoswitch.companion.data

import android.content.Context
import androidx.core.content.edit

/**
 * Which library view the user prefers.
 *
 * A stable preference, not a per-visit choice: re-picking Grid or List on every
 * launch is friction. Only the view mode is persisted — search and filters are
 * per-session by design, because coming back to a library silently narrowed by a
 * filter set days ago is confusing rather than helpful.
 *
 * Deliberately total: an unreadable or unknown value falls back to Grid rather
 * than failing, because a preference is never worth a crash.
 */
class AmiiboViewPreferenceStore(context: Context) {
    private val preferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun load(): AmiiboViewMode =
        runCatching { AmiiboViewMode.valueOf(preferences.getString(VIEW_KEY, null) ?: "") }
            .getOrDefault(AmiiboViewMode.Grid)

    fun save(mode: AmiiboViewMode) {
        preferences.edit { putString(VIEW_KEY, mode.name) }
    }

    companion object {
        internal const val FILE_NAME = "amiibo_view"
        internal const val VIEW_KEY = "view_mode"
    }
}
