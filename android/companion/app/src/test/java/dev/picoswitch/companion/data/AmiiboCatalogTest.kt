package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
import dev.picoswitch.companion.model.AmiiboCatalogState
import dev.picoswitch.companion.model.AmiiboLibraryItem
import dev.picoswitch.companion.model.AmiiboSortOrder
import dev.picoswitch.companion.model.resolveAmiiboCatalogState
import dev.picoswitch.companion.model.sortAmiiboLibrary
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class AmiiboCatalogTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `parses and matches AmiiboAPI figure id without network`() {
        val entries = AmiiboCatalogStore.parseCatalog(
            """
            {"amiibo":[{"id":"FFFFFFFFFFFFFFFF","head":"00000000","tail":"00000002","name":"Kirby - Super Smash Bros.","character":"Kirby",
              "gameSeries":"Kirby","amiiboSeries":"Super Smash Bros.","type":"Figure",
              "image":"https://example.invalid/kirby.png",
              "release":{"us":"2015-11-20","jp":"2015-11-05"},
              "gamesSwitch":[{"gameName":"Super Smash Bros. Ultimate","gameID":["01006A800016E000"]},
                              {"gameName":"Super Smash Bros. Ultimate","gameID":[]}]}]}
            """.trimIndent(),
        )
        assertEquals(1, entries.size)
        val kirby = entries.single()
        assertEquals("0000000000000002", kirby.id)
        assertEquals("Kirby - Super Smash Bros.", kirby.name)
        assertEquals("Kirby", kirby.character)
        assertEquals("2015-11-05", kirby.releaseDate)
        assertEquals(listOf("Super Smash Bros. Ultimate"), kirby.games["Switch"])
        assertEquals("Super Smash Bros. Ultimate", kirby.titleIds["01006A800016E000"])
    }

    @Test fun `malformed or incomplete catalog entries are ignored`() {
        assertTrue(AmiiboCatalogStore.parseCatalog("{broken").isEmpty())
        val entries = AmiiboCatalogStore.parseCatalog(
            """{"amiibo":[{"character":"No id"},{"head":"0011","tail":"22"},{"head":"GGGGGGGG","tail":"00000001"}]}""",
        )
        assertTrue(entries.isEmpty())
    }

    @Test fun `catalog cache survives restart and unmatched ids stay offline safe`() = runTest {
        val root = temporary.newFolder("catalog")
        val store = AmiiboCatalogStore(root)
        store.saveCached(
            listOf(AmiiboCatalogEntry("0000000000000002", "Kirby", "Kirby", "Super Smash Bros.", "Figure", "2015-11-20", "https://example.invalid/kirby.png", titleIds = mapOf("01006A800016E000" to "Super Smash Bros. Ultimate"), name = "Kirby - Super Smash Bros.")),
            cachedAtMillis = System.currentTimeMillis(),
        )
        val reopened = AmiiboCatalogStore(root)
        assertEquals("Kirby", reopened.find("0000000000000002")?.character)
        assertEquals("Kirby - Super Smash Bros.", reopened.find("0000000000000002")?.name)
        assertEquals("Super Smash Bros. Ultimate", reopened.gameNameForTitleId("01006A800016E000"))
        assertNull(reopened.find("0000000000009999"))
        assertTrue(reopened.ensureLoaded())
    }

    @Test fun `adapter lookup exposes loading terminal states without hiding raw identity`() {
        assertEquals(AmiiboCatalogState.Available, resolveAmiiboCatalogState(found = true, catalogAvailable = true))
        assertEquals(AmiiboCatalogState.Unmatched, resolveAmiiboCatalogState(found = false, catalogAvailable = true))
        assertEquals(AmiiboCatalogState.Offline, resolveAmiiboCatalogState(found = false, catalogAvailable = false))
    }

    @Test fun `library sort order is deterministic and changes the displayed order`() {
        val older = AmiiboLibraryItem("a", "Local Z", "a.bin", 540, "1", "uid-a", "0000000000000001", 10)
        val newer = AmiiboLibraryItem("b", "Local A", "b.bin", 540, "2", "uid-b", "0000000000000002", 20)
        val catalog = mapOf(
            older.id to AmiiboCatalogEntry(older.figureId, "Zelda", "The Legend of Zelda", "Smash", "Figure", "", ""),
            newer.id to AmiiboCatalogEntry(newer.figureId, "Mario", "Super Mario", "Smash", "Figure", "", ""),
        )
        assertEquals(listOf(newer.id, older.id), sortAmiiboLibrary(listOf(older, newer), catalog, AmiiboSortOrder.Name).map { it.id })
        assertEquals(listOf(newer.id, older.id), sortAmiiboLibrary(listOf(older, newer), catalog, AmiiboSortOrder.Series).map { it.id })
        assertEquals(listOf(newer.id, older.id), sortAmiiboLibrary(listOf(older, newer), catalog, AmiiboSortOrder.RecentlyAdded).map { it.id })
    }
}
