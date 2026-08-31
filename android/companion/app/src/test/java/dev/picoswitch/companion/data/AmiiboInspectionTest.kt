package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
import dev.picoswitch.companion.model.AmiiboCryptoState
import dev.picoswitch.companion.model.AmiiboDetails
import dev.picoswitch.companion.model.AmiiboLibraryItem
import dev.picoswitch.companion.model.AmiiboTagType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The grouping, omission and category rules the detail surface renders.
 *
 * These are the rules that decide what a user is TOLD about a figure, which is
 * why they are tested here rather than left to a Compose layout: the same rules
 * back the Windows companion, and the two must not drift into describing the
 * same dump differently.
 */
class AmiiboInspectionTest {

    private fun item(
        displayName: String = "My Link",
        figureId: String = "01000000_00000002",
        uid: String = "04A1B2C3D4E580",
        tagType: AmiiboTagType = AmiiboTagType.Ntag215,
        size: Int = 540,
    ) = AmiiboLibraryItem(
        id = "item-1",
        displayName = displayName,
        fileName = "link.bin",
        size = size,
        crc32 = "DEADBEEF",
        uid = uid,
        figureId = figureId,
        importedAtMillis = 0L,
        tagType = tagType,
    )

    private fun catalog(
        name: String = "Link",
        character: String = "Link",
        gameSeries: String = "The Legend of Zelda",
        amiiboSeries: String = "Super Smash Bros.",
        type: String = "Figure",
        releaseDate: String = "2014-11-21",
        titleIds: Map<String, String> = emptyMap(),
    ) = AmiiboCatalogEntry(
        id = "01000000_00000002",
        character = character,
        gameSeries = gameSeries,
        amiiboSeries = amiiboSeries,
        type = type,
        releaseDate = releaseDate,
        imageUrl = "https://example.invalid/link.png",
        titleIds = titleIds,
        name = name,
    )

    private fun decoded(
        crypto: AmiiboCryptoState = AmiiboCryptoState.Valid,
        setUp: Boolean = true,
        owner: String = "Miles",
        nickname: String = "Champion",
        setupDate: String? = "2015-03-01",
        lastWriteDate: String? = "2016-07-04",
        writeCounter: Int? = 12,
        hasAppData: Boolean? = false,
        titleId: String = "",
        appDataLabel: String = "",
    ) = AmiiboDetails(
        uid = "04A1B2C3D4E580",
        figureId = "01000000_00000002",
        tagType = AmiiboTagType.Ntag215,
        size = 540,
        crc32 = "DEADBEEF",
        characterGameCode = "",
        characterVariant = 0,
        typeName = "Figure",
        modelNumber = "",
        seriesCode = 0,
        formatVersion = 0,
        extendedVariant = "",
        crypto = crypto,
        owner = owner,
        nickname = nickname,
        setUp = setUp,
        setupDate = setupDate,
        lastWriteDate = lastWriteDate,
        writeCounter = writeCounter,
        hasAppData = hasAppData,
        titleId = titleId,
        appDataLabel = appDataLabel,
    )

    private fun build(
        item: AmiiboLibraryItem = item(),
        catalog: AmiiboCatalogEntry? = catalog(),
        decoded: AmiiboDetails? = decoded(),
        onAdapter: Boolean = false,
        adapterChanged: Boolean = false,
    ) = AmiiboInspection.build(item, catalog, decoded, onAdapter, adapterChanged)

    private fun List<AmiiboDetailGroup>.group(title: String) = firstOrNull { it.title == title }

    private fun List<AmiiboDetailGroup>.row(label: String) =
        flatMap { it.rows }.firstOrNull { it.label == label }?.value

    // -- Overview ----------------------------------------------------------

    @Test fun `overview carries the fields the catalog contributes`() {
        val overview = AmiiboInspection.forCategory(AmiiboCategory.Overview, build())
        assertEquals(listOf("Identity"), overview.map { it.title })

        assertEquals("My Link", overview.row("Your name"))
        assertEquals("Link", overview.row("Figure"))
        assertEquals("The Legend of Zelda", overview.row("Game series"))
        assertEquals("Super Smash Bros.", overview.row("Collection"))
        assertEquals("Figure", overview.row("Type"))
        assertEquals("2014-11-21", overview.row("Released"))
        assertEquals("01000000_00000002", overview.row("Figure ID"))
    }

    /**
     * The bug this project actually shipped on Windows: a projection that
     * ignored catalog-derived fields left a library permanently unenriched.
     * Here the equivalent guarantee is that the catalog materially changes the
     * output, so a caller that fails to re-run on catalog arrival is producing
     * visibly different content and cannot be mistaken for a no-op.
     */
    @Test fun `a catalog arriving after the first projection adds overview content`() {
        val before = AmiiboInspection.forCategory(AmiiboCategory.Overview, build(catalog = null))
        val after = AmiiboInspection.forCategory(AmiiboCategory.Overview, build(catalog = catalog()))

        assertNull(before.row("Game series"))
        assertNull(before.row("Collection"))
        assertNull(before.row("Released"))
        assertEquals("The Legend of Zelda", after.row("Game series"))

        assertTrue(
            "catalog arrival must change what the overview renders",
            before.flatMap { it.rows } != after.flatMap { it.rows },
        )
    }

    @Test fun `without a catalog the identity still names the figure`() {
        val overview = AmiiboInspection.forCategory(AmiiboCategory.Overview, build(catalog = null))
        assertEquals("My Link", overview.row("Your name"))
        assertEquals("01000000_00000002", overview.row("Figure ID"))
    }

    /** Two lines saying the same word teach nothing. */
    @Test fun `the figure row is dropped when it repeats the user's own name`() {
        val same = build(item = item(displayName = "Link"))
        assertNull(AmiiboInspection.forCategory(AmiiboCategory.Overview, same).row("Figure"))

        val cased = build(item = item(displayName = "lInK"))
        assertNull(AmiiboInspection.forCategory(AmiiboCategory.Overview, cased).row("Figure"))
    }

    @Test fun `the catalog's display name is preferred over the character`() {
        val groups = build(catalog = catalog(name = "Link (Majora's Mask)", character = "Link"))
        assertEquals("Link (Majora's Mask)", groups.row("Figure"))
    }

    @Test fun `the character is used when the catalog has no display name`() {
        val groups = build(catalog = catalog(name = "", character = "Toon Link"))
        assertEquals("Toon Link", groups.row("Figure"))
    }

    // -- Tag ---------------------------------------------------------------

    @Test fun `tag reports the physical facts about the dump`() {
        val tag = AmiiboInspection.forCategory(AmiiboCategory.Tag, build())
        assertEquals("NTAG215", tag.row("Tag type"))
        assertEquals("04A1B2C3D4E580", tag.row("UID"))
        assertEquals("540 bytes", tag.row("Size"))
    }

    @Test fun `a v3 figure is named as one`() {
        val groups = build(item = item(tagType = AmiiboTagType.FigureV3, size = 2048))
        assertEquals("Figure v3", groups.row("Tag type"))
        assertEquals("2048 bytes", groups.row("Size"))
    }

    /** Identifiers are compared by eye; prose is not. */
    @Test fun `only identifiers are marked monospace`() {
        val rows = build().flatMap { it.rows }
        assertTrue(rows.first { it.label == "UID" }.monospace)
        assertTrue(rows.first { it.label == "Figure ID" }.monospace)
        assertFalse(rows.first { it.label == "Your name" }.monospace)
        assertFalse(rows.first { it.label == "Game series" }.monospace)
    }

    @Test fun `the three unreadable-body states are distinguishable`() {
        assertEquals(
            "Import your Amiibo keys to read this",
            build(decoded = null).row("Contents"),
        )
        assertEquals(
            "Import your Amiibo keys to read this",
            build(decoded = decoded(crypto = AmiiboCryptoState.NotAttempted)).row("Contents"),
        )
        assertEquals(
            "Could not be decrypted with the imported keys",
            build(decoded = decoded(crypto = AmiiboCryptoState.Invalid)).row("Contents"),
        )
        assertEquals("Set up", build(decoded = decoded(setUp = true)).row("Contents"))
        assertEquals(
            "Blank, never set up",
            build(decoded = decoded(setUp = false)).row("Contents"),
        )
    }

    // -- Registration ------------------------------------------------------

    @Test fun `registration appears only when the tag was set up`() {
        val setUp = build(decoded = decoded(setUp = true))
        assertEquals("Miles", setUp.row("Owner"))
        assertEquals("Champion", setUp.row("Nickname"))
        assertEquals("2015-03-01", setUp.row("Registered"))
        assertEquals("2016-07-04", setUp.row("Last written"))
        assertEquals("12", setUp.row("Times written"))
    }

    /**
     * A blank tag prints NO registration group at all. Five empty lines would
     * imply the information exists and failed to load.
     */
    @Test fun `a blank tag has no registration group`() {
        assertNull(build(decoded = decoded(setUp = false)).group("Registration"))
        assertNull(build(decoded = null).group("Registration"))
        assertNull(
            build(decoded = decoded(crypto = AmiiboCryptoState.Invalid)).group("Registration"),
        )
    }

    @Test fun `empty registration fields are omitted rather than shown blank`() {
        val groups = build(
            decoded = decoded(
                owner = "",
                nickname = "",
                setupDate = null,
                lastWriteDate = null,
                writeCounter = 0,
            ),
        )
        assertNull(groups.group("Registration"))
    }

    @Test fun `a never-written tag omits the write count`() {
        val groups = build(decoded = decoded(writeCounter = 0))
        assertNull(groups.row("Times written"))
        // The group survives on its other rows.
        assertEquals("Miles", groups.row("Owner"))
    }

    // -- Game data ---------------------------------------------------------

    @Test fun `a figure with no game data says so under the same label`() {
        assertEquals("None", build(decoded = decoded(hasAppData = false)).row("Game"))
    }

    @Test fun `the catalog names the game when it can`() {
        val groups = build(
            catalog = catalog(titleIds = mapOf("0100000000010000" to "Super Mario Odyssey")),
            decoded = decoded(
                hasAppData = true,
                titleId = "0100000000010000",
                appDataLabel = "Unknown title",
            ),
        )
        assertEquals("Super Mario Odyssey", groups.row("Game"))
        assertEquals("0100000000010000", groups.row("Title ID"))
    }

    @Test fun `the catalog's title id match ignores case`() {
        val groups = build(
            catalog = catalog(titleIds = mapOf("0100000000010000" to "Super Mario Odyssey")),
            decoded = decoded(hasAppData = true, titleId = "0100000000010000".lowercase()),
        )
        assertEquals("Super Mario Odyssey", groups.row("Game"))
    }

    @Test fun `the decoded label is used when the catalog cannot name the game`() {
        val groups = build(
            catalog = catalog(titleIds = emptyMap()),
            decoded = decoded(hasAppData = true, titleId = "ABCD", appDataLabel = "Breath of the Wild"),
        )
        assertEquals("Breath of the Wild", groups.row("Game"))
    }

    @Test fun `game data is absent entirely when the body could not be read`() {
        assertNull(build(decoded = null).group("Game data"))
        assertNull(build(decoded = decoded(crypto = AmiiboCryptoState.Invalid)).group("Game data"))
    }

    // -- Adapter -----------------------------------------------------------

    @Test fun `adapter status distinguishes absent, resident and changed`() {
        assertEquals(
            "Not on the adapter",
            AmiiboInspection.forCategory(AmiiboCategory.Adapter, build(onAdapter = false)).row("Status"),
        )
        assertEquals(
            "On the adapter",
            AmiiboInspection.forCategory(AmiiboCategory.Adapter, build(onAdapter = true)).row("Status"),
        )
        assertEquals(
            "On the adapter, changed by the console",
            AmiiboInspection
                .forCategory(AmiiboCategory.Adapter, build(onAdapter = true, adapterChanged = true))
                .row("Status"),
        )
    }

    /**
     * The library is usable with no adapter attached. Nothing in the inspection
     * requires one, and the adapter category still renders a definite answer.
     */
    @Test fun `every category renders with no adapter and no catalog and no key`() {
        val groups = build(catalog = null, decoded = null, onAdapter = false)
        AmiiboCategory.entries.forEach { category ->
            val shown = AmiiboInspection.forCategory(category, groups)
            assertTrue("$category rendered nothing offline", shown.isNotEmpty())
            assertTrue("$category rendered no rows offline", shown.flatMap { it.rows }.isNotEmpty())
        }
    }

    // -- Categories --------------------------------------------------------

    @Test fun `the categories partition the groups with nothing lost`() {
        val groups = build()
        val partitioned = AmiiboCategory.entries.flatMap { AmiiboInspection.forCategory(it, groups) }

        assertEquals(groups, partitioned)
        assertEquals(partitioned.size, partitioned.distinct().size)
    }

    @Test fun `each category claims the groups it is responsible for`() {
        val groups = build()
        assertEquals(
            listOf("Identity"),
            AmiiboInspection.forCategory(AmiiboCategory.Overview, groups).map { it.title },
        )
        assertEquals(
            listOf("Tag", "Registration", "Game data"),
            AmiiboInspection.forCategory(AmiiboCategory.Tag, groups).map { it.title },
        )
        assertEquals(
            listOf("Adapter"),
            AmiiboInspection.forCategory(AmiiboCategory.Adapter, groups).map { it.title },
        )
    }

    /** No group is ever emitted with nothing in it. */
    @Test fun `empty groups never reach the caller`() {
        listOf(
            build(),
            build(catalog = null),
            build(decoded = null),
            build(decoded = decoded(setUp = false)),
            build(decoded = decoded(crypto = AmiiboCryptoState.Invalid)),
            build(catalog = null, decoded = null),
        ).forEach { groups ->
            assertTrue(groups.all { it.rows.isNotEmpty() })
            assertTrue(groups.all { it.any })
        }
    }

    /**
     * Selection state and query state stay separate all the way down: the
     * inspection describes ONE item and cannot see the library, so no detail
     * rendering can perturb the browser's collection.
     */
    @Test fun `inspecting the same item twice is stable`() {
        assertEquals(build(), build())
    }
}
