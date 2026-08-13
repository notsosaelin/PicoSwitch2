package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
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
}
