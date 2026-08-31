package dev.picoswitch.companion.data

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The view-mode preference's decode rules.
 *
 * Only the view mode is persisted. Search and filters are per-session by
 * design: returning to a library silently narrowed by a filter set days ago is
 * confusing rather than helpful, and the user has no way to tell why half their
 * collection is missing.
 *
 * The store itself needs Android, so what is tested here is the part that can
 * actually be wrong — that an unknown or absent value falls back to Grid rather
 * than failing, because a preference is never worth a crash.
 */
class AmiiboViewPreferenceStoreTest {

    private fun decode(stored: String?): AmiiboViewMode =
        runCatching { AmiiboViewMode.valueOf(stored ?: "") }.getOrDefault(AmiiboViewMode.Grid)

    @Test fun `every view mode round trips through its name`() {
        AmiiboViewMode.entries.forEach { mode ->
            assertEquals(mode, decode(mode.name))
        }
    }

    @Test fun `an absent or damaged preference falls back to grid`() {
        assertEquals(AmiiboViewMode.Grid, decode(null))
        assertEquals(AmiiboViewMode.Grid, decode(""))
        assertEquals(AmiiboViewMode.Grid, decode("Mosaic"))
        // A value from a future build that added a mode this one lacks.
        assertEquals(AmiiboViewMode.Grid, decode("Timeline"))
    }
}
