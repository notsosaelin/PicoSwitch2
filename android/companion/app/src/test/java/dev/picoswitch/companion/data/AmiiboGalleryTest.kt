package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
import dev.picoswitch.companion.model.AmiiboLibraryItem
import dev.picoswitch.companion.model.AmiiboTagType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The Android library browser's query state.
 *
 * A DELIBERATE MIRROR of the Windows `AmiiboGalleryTests` and
 * `AmiiboBrowserStateTests`: the same search fields, sort modes, tie-breaks and
 * filter semantics, so the same query produces equivalent results on both
 * platforms. The two UIs look nothing alike and should not; the behaviour is
 * what has to match.
 *
 * THE DEFECT THESE ALSO PIN. On Windows, clicking an Amiibo rebuilt the browser
 * collection and sent a 1000-item library back to the top. The structural fix is
 * that a selection cannot influence the projection —
 * [AmiiboGalleryFilters] carries no selection at all — and Android's LazyGrid
 * would lose its scroll position the same way if the item list were rebuilt per
 * selection.
 */
class AmiiboGalleryTest {

    private fun item(
        id: String,
        name: String,
        figureId: String = "0183000002420502",
        importedAt: Long = 0L,
        dirty: Boolean = false,
    ) = AmiiboLibraryItem(
        id = id,
        displayName = name,
        fileName = "$id.bin",
        size = 540,
        crc32 = "00000000",
        uid = "04" + id.padStart(12, '0'),
        figureId = figureId,
        importedAtMillis = importedAt,
        dirtyFromAdapter = dirty,
    )

    private fun entry(
        figureId: String,
        name: String,
        gameSeries: String = "Animal Crossing",
        amiiboSeries: String = "Animal Crossing",
        type: String = "Figure",
        release: String = "2015-07-30",
        image: String = "https://example.invalid/a.png",
    ) = AmiiboCatalogEntry(
        id = figureId,
        character = name,
        gameSeries = gameSeries,
        amiiboSeries = amiiboSeries,
        type = type,
        releaseDate = release,
        imageUrl = image,
        name = name,
    )

    private fun catalog(vararg entries: AmiiboCatalogEntry): (String) -> AmiiboCatalogEntry? {
        val index = entries.associateBy { it.id.uppercase() }
        return { figureId -> index[figureId.uppercase()] }
    }

    private val noCatalog: (String) -> AmiiboCatalogEntry? = { null }

    private val library = listOf(
        item("a", "Nook", "0183000002420502", importedAt = 100),
        item("bb", "Kirby", "1F00000004C41E03", importedAt = 200),
        item("ccc", "Dedede", "1F02000004C71E03", importedAt = 300),
    )

    private val fullCatalog = catalog(
        entry("0183000002420502", "Tom Nook", "Animal Crossing", "Animal Crossing"),
        entry("1F00000004C41E03", "Kirby", "Kirby", "Air Riders"),
        entry("1F02000004C71E03", "King Dedede", "Kirby", "Air Riders"),
    )

    private fun ids(filters: AmiiboGalleryFilters, loadedId: String? = null) =
        AmiiboGallery.build(library, fullCatalog, loadedId, filters).map { it.id }

    // -------------------------------------------------- the root-cause guard

    @Test fun `the query record carries no selection`() {
        // The structural guarantee. If a selection could be put here, some
        // future projection could read it and the scroll-reset defect could come
        // back by accident.
        val names = AmiiboGalleryFilters::class.java.declaredFields.map { it.name.lowercase() }
        assertFalse(names.any { it.contains("select") })
    }

    @Test fun `projecting is independent of which item is selected`() {
        val filters = AmiiboGalleryFilters(sort = AmiiboSort.Name)
        assertEquals(ids(filters), ids(filters))
        // The adapter's tag changes badges without changing the sequence.
        assertEquals(ids(filters), ids(filters, loadedId = "bb"))
    }

    @Test fun `view mode is not part of the query identity`() {
        // Switching view must not re-run the query; re-running is what would
        // rebuild the list and lose scroll position.
        val grid = AmiiboGalleryFilters(view = AmiiboViewMode.Grid)
        assertEquals(grid.queryIdentity, grid.copy(view = AmiiboViewMode.List).queryIdentity)
    }

    @Test fun `every narrowing or ordering change is part of the query identity`() {
        val base = AmiiboGalleryFilters()
        listOf(
            base.copy(search = "x"),
            base.copy(gameSeries = "Kirby"),
            base.copy(amiiboSeries = "Air Riders"),
            base.copy(type = "Figure"),
            base.copy(sort = AmiiboSort.Name),
            base.copy(descending = true),
        ).forEach { assertTrue(base.queryIdentity != it.queryIdentity) }
    }

    @Test fun `all three views produce the same result`() {
        val filters = AmiiboGalleryFilters(search = "kir", sort = AmiiboSort.Name)
        val grid = ids(filters.copy(view = AmiiboViewMode.Grid))
        assertEquals(grid, ids(filters.copy(view = AmiiboViewMode.Carousel)))
        assertEquals(grid, ids(filters.copy(view = AmiiboViewMode.List)))
        assertTrue(grid.isNotEmpty())
    }

    @Test fun `clearing filters keeps sort and view`() {
        val filters = AmiiboGalleryFilters(
            search = "nook", gameSeries = "Animal Crossing",
            sort = AmiiboSort.Release, descending = true, view = AmiiboViewMode.List,
        )
        assertTrue(filters.any)

        val cleared = AmiiboGalleryFilters(
            sort = filters.sort, descending = filters.descending, view = filters.view,
        )
        assertFalse(cleared.any)
        assertEquals(AmiiboSort.Release, cleared.sort)
        assertTrue(cleared.descending)
        assertEquals(AmiiboViewMode.List, cleared.view)
    }

    // ------------------------------------------------------------------ cards

    @Test fun `the catalog name leads and the user's own name is kept`() {
        val cards = AmiiboGallery.build(
            listOf(item("a", "Nook for trading")),
            catalog(entry("0183000002420502", "Tom Nook")),
            null,
            AmiiboGalleryFilters(),
        )
        val card = cards.single()
        assertEquals("Tom Nook", card.title)
        assertEquals("Nook for trading", card.ownName)
        assertTrue(card.subtitle.contains("Animal Crossing"))
        assertTrue(card.hasArtwork)
    }

    @Test fun `with no catalog the tag still describes itself`() {
        val card = AmiiboGallery.build(
            listOf(item("a", "My backup")), noCatalog, null, AmiiboGalleryFilters(),
        ).single()

        assertEquals("My backup", card.title)
        assertTrue(card.subtitle.contains("NTAG215"))
        assertTrue(card.subtitle.contains("0183000002420502"))
        assertFalse(card.hasArtwork)
        assertEquals("", card.ownName)
    }

    @Test fun `badges report the adapter, with changed outranking loaded`() {
        assertEquals(
            "On adapter",
            AmiiboGallery.build(listOf(item("a", "N")), noCatalog, "a", AmiiboGalleryFilters())
                .single().badge,
        )
        assertEquals(
            "Changed",
            AmiiboGallery.build(
                listOf(item("a", "N", dirty = true)), noCatalog, "a", AmiiboGalleryFilters(),
            ).single().badge,
        )
        assertEquals(
            "",
            AmiiboGallery.build(listOf(item("a", "N")), noCatalog, null, AmiiboGalleryFilters())
                .single().badge,
        )
    }

    @Test fun `artwork failure is represented as simply having none`() {
        // What an unknown figure or a failed fetch looks like from here. The
        // card still renders; the UI falls back to a placeholder.
        val card = AmiiboGallery.build(
            listOf(item("a", "Nook")),
            catalog(entry("0183000002420502", "Tom Nook", image = "")),
            null,
            AmiiboGalleryFilters(),
        ).single()

        assertFalse(card.hasArtwork)
        assertEquals("Tom Nook", card.title)
        assertTrue(card.subtitle.isNotEmpty())
    }

    // ----------------------------------------------------------------- search

    @Test fun `search matches anything a person might remember`() {
        listOf("Nook", "tom", "animal", "0183", "CROSSING").forEach { needle ->
            assertEquals(
                "'$needle' should have matched",
                1,
                ids(AmiiboGalleryFilters(search = needle)).size,
            )
        }
        assertTrue(ids(AmiiboGalleryFilters(search = "Metroid")).isEmpty())
    }

    @Test fun `search ignores surrounding space`() {
        assertEquals(1, ids(AmiiboGalleryFilters(search = "  tom  ")).size)
    }

    // ---------------------------------------------------------------- filters

    @Test fun `filters narrow by series and type`() {
        assertEquals(listOf("bb", "ccc").sorted(), ids(AmiiboGalleryFilters(gameSeries = "Kirby")).sorted())
        assertEquals(listOf("bb", "ccc").sorted(), ids(AmiiboGalleryFilters(amiiboSeries = "Air Riders")).sorted())
        assertEquals(3, ids(AmiiboGalleryFilters(type = "Figure")).size)
    }

    @Test fun `a filtered field the catalog cannot answer excludes the tag`() {
        // An unknown figure has no series, so filtering BY series must not
        // silently include it.
        val cards = AmiiboGallery.build(
            listOf(item("a", "Mystery")), noCatalog, null,
            AmiiboGalleryFilters(gameSeries = "Animal Crossing"),
        )
        assertTrue(cards.isEmpty())
    }

    @Test fun `filters combine`() {
        assertTrue(
            ids(AmiiboGalleryFilters(gameSeries = "Animal Crossing", search = "Kirby")).isEmpty(),
        )
    }

    @Test fun `filter options come from what the user owns`() {
        val options = AmiiboGallery.options(library, fullCatalog)
        assertEquals(listOf("Animal Crossing", "Kirby"), options.gameSeries)
        assertEquals(listOf("Air Riders", "Animal Crossing"), options.amiiboSeries)
        assertEquals(listOf("Figure"), options.types)
    }

    @Test fun `filter options are empty with no catalog`() {
        val options = AmiiboGallery.options(library, noCatalog)
        assertTrue(options.gameSeries.isEmpty())
        assertTrue(options.types.isEmpty())
    }

    // ---------------------------------------------------------------- sorting

    @Test fun `the default order is most recently added`() {
        assertEquals(listOf("ccc", "bb", "a"), ids(AmiiboGalleryFilters()))
    }

    @Test fun `sort by name uses the displayed name`() {
        // The CATALOG name, because that is the one on the card. Sorting by the
        // user's own name would order the grid by something invisible.
        assertEquals(listOf("ccc", "bb", "a"), ids(AmiiboGalleryFilters(sort = AmiiboSort.Name)))
    }

    @Test fun `sort by number groups a series the way a shelf does`() {
        assertEquals(listOf("a", "bb", "ccc"), ids(AmiiboGalleryFilters(sort = AmiiboSort.Number)))
    }

    @Test fun `sort by release puts undated figures last`() {
        // An empty date would otherwise sort before every real one and put the
        // unknowns on top, which is the opposite of useful.
        val mixed = listOf(item("known", "Known", "0183000002420502"), item("unknown", "Unknown", "1F00000004C41E03"))
        val partial = catalog(entry("0183000002420502", "Known", release = "2015-07-30"))

        val ordered = AmiiboGallery
            .build(mixed, partial, null, AmiiboGalleryFilters(sort = AmiiboSort.Release))
            .map { it.id }

        assertEquals(listOf("known", "unknown"), ordered)
    }

    @Test fun `reversing applies to every sort mode`() {
        AmiiboSort.entries.forEach { sort ->
            val forward = ids(AmiiboGalleryFilters(sort = sort))
            val reversed = ids(AmiiboGalleryFilters(sort = sort, descending = true))
            assertEquals("reversal for $sort", forward.reversed(), reversed)
        }
    }

    @Test fun `an empty library produces no cards rather than throwing`() {
        assertTrue(AmiiboGallery.build(emptyList(), noCatalog, null, AmiiboGalleryFilters()).isEmpty())
        assertTrue(AmiiboGallery.options(emptyList(), noCatalog).gameSeries.isEmpty())
    }

    // ------------------------------------------------------- list view columns

    @Test fun `every card carries the detailed list columns`() {
        // The list view reads the SAME cards as the grid rather than a second
        // projection -- two projections is how "one shared query state" quietly
        // stops being true.
        val card = AmiiboGallery
            .build(library, fullCatalog, null, AmiiboGalleryFilters())
            .first { it.id == "a" }

        assertEquals("Animal Crossing", card.gameSeries)
        assertEquals("Animal Crossing", card.amiiboSeries)
        assertEquals("2015-07-30", card.releaseDate)
        assertEquals("0183000002420502", card.figureId)
    }

    @Test fun `columns are empty rather than invented with no catalog`() {
        val card = AmiiboGallery
            .build(listOf(item("a", "Nook")), noCatalog, null, AmiiboGalleryFilters())
            .single()

        assertEquals("", card.gameSeries)
        assertEquals("", card.releaseDate)
        // The figure id comes from the tag itself, so it is always known.
        assertEquals("0183000002420502", card.figureId)
    }

    // --------------------------------------------------- the resident guard

    /**
     * THE DEFECT THIS PINS. The detail pane used to enable Present, Eject and
     * Sync whenever the adapter held ANY Amiibo. Those commands act on whatever
     * is resident, so under a pane headed "Kirby" they would eject the Nook the
     * adapter was actually holding, or overwrite Kirby's backup with Nook's
     * bytes. The rule is identity, not mere occupancy, and it lives in one place
     * so the browser's badge and the pane's buttons cannot drift apart.
     */
    @Test fun `residency is identity, not mere occupancy`() {
        val nook = library.first { it.id == "a" }
        val kirby = library.first { it.id == "bb" }

        assertTrue(AmiiboGallery.residentOn(nook, nook.uid, adapterHasAmiibo = true))
        assertFalse(AmiiboGallery.residentOn(kirby, nook.uid, adapterHasAmiibo = true))
    }

    @Test fun `an empty adapter is resident of nothing`() {
        val nook = library.first { it.id == "a" }
        assertFalse(AmiiboGallery.residentOn(nook, nook.uid, adapterHasAmiibo = false))
        assertNull(AmiiboGallery.residentId(library, nook.uid, adapterHasAmiibo = false))
    }

    /** A silent adapter is not evidence that it holds this figure. */
    @Test fun `a blank uid matches nothing rather than everything`() {
        val blank = item("blank", "Unknown").copy(uid = "")
        assertFalse(AmiiboGallery.residentOn(blank, "", adapterHasAmiibo = true))
        assertNull(AmiiboGallery.residentId(listOf(blank), "", adapterHasAmiibo = true))
        assertNull(AmiiboGallery.residentId(library, "", adapterHasAmiibo = true))
    }

    @Test fun `uid comparison ignores case`() {
        val nook = library.first { it.id == "a" }
        assertTrue(AmiiboGallery.residentOn(nook, nook.uid.lowercase(), adapterHasAmiibo = true))
        assertEquals("a", AmiiboGallery.residentId(library, nook.uid.lowercase(), true))
    }

    /** The badge the browser draws and the rule the pane trusts are the same. */
    @Test fun `the resident id is the card the browser badges`() {
        val nook = library.first { it.id == "a" }
        val badged = AmiiboGallery
            .build(library, fullCatalog, AmiiboGallery.residentId(library, nook.uid, true), AmiiboGalleryFilters())
            .filter { it.badge.isNotEmpty() }

        assertEquals(listOf("a"), badged.map { it.id })
    }

    @Test fun `an adapter holding a figure the phone lacks resolves to no id`() {
        assertNull(AmiiboGallery.residentId(library, "04FFFFFFFFFFFF", adapterHasAmiibo = true))
    }

    // ------------------------------------------------- the late-catalog guard

    /**
     * THE DEFECT THIS PINS, and it is a real one this project shipped.
     *
     * The catalog is fetched over the network and arrives long after the library
     * is first projected. Windows decided whether to rebuild its tiles from a
     * signature of id and badge alone — neither of which the catalog touches —
     * so the arrival changed nothing on screen and a thousand figures sat as
     * untitled placeholders until something else forced a rebuild.
     *
     * The test states the property the caller's cache key must respect: the same
     * library and the same query produce MATERIALLY DIFFERENT cards before and
     * after the catalog lands. Any memoisation that omits the catalog is
     * therefore returning stale content, not a valid cache hit.
     */
    @Test fun `a catalog arriving after the first projection changes the cards`() {
        val filters = AmiiboGalleryFilters()
        val before = AmiiboGallery.build(library, noCatalog, null, filters)
        val after = AmiiboGallery.build(library, fullCatalog, null, filters)

        // Identity and ordering are unchanged -- this is enrichment, not a new
        // query -- which is exactly why an id-based signature misses it.
        assertEquals(before.map { it.id }, after.map { it.id })
        assertEquals(before.map { it.badge }, after.map { it.badge })

        assertTrue("the catalog must change the projection", before != after)

        val nookBefore = before.first { it.id == "a" }
        val nookAfter = after.first { it.id == "a" }

        // Title and artwork are the two fields the browser actually renders.
        assertEquals("Nook", nookBefore.title)
        assertEquals("Tom Nook", nookAfter.title)
        assertFalse(nookBefore.hasArtwork)
        assertTrue(nookAfter.hasArtwork)
        assertEquals("", nookBefore.gameSeries)
        assertEquals("Animal Crossing", nookAfter.gameSeries)
    }

    /**
     * The same guard for the filter options: they are derived from the catalog,
     * so an empty filter menu before the fetch must not be cached past it.
     */
    @Test fun `filter options appear when the catalog arrives`() {
        assertTrue(AmiiboGallery.options(library, noCatalog).gameSeries.isEmpty())
        assertTrue(AmiiboGallery.options(library, fullCatalog).gameSeries.isNotEmpty())
    }

    /**
     * The library is the phone's own data and the catalog is decoration. Every
     * card still projects, sorts and searches with no network at all.
     */
    @Test fun `the library is fully usable offline`() {
        val offline = AmiiboGallery.build(library, noCatalog, null, AmiiboGalleryFilters())
        assertEquals(library.size, offline.size)
        assertTrue(offline.all { it.title.isNotBlank() })
        assertTrue(offline.none { it.hasArtwork })

        // Sorting and searching work on the names the phone stored itself.
        assertEquals(
            listOf("ccc", "bb", "a"),
            AmiiboGallery
                .build(library, noCatalog, null, AmiiboGalleryFilters(sort = AmiiboSort.Name))
                .map { it.id },
        )
        assertEquals(
            listOf("bb"),
            AmiiboGallery
                .build(library, noCatalog, null, AmiiboGalleryFilters(search = "kirby"))
                .map { it.id },
        )
    }
}
