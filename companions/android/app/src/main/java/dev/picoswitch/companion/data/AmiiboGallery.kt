package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
import dev.picoswitch.companion.model.AmiiboLibraryItem
import dev.picoswitch.companion.model.AmiiboTagType

/**
 * How the library is ordered.
 *
 * The same four the Windows companion and the web portal offer, because they
 * answer different questions: [Default] is the order tags were added, which is
 * what someone looking for the one they just imported wants; [Number] sorts by
 * figure id, which groups a series in release order the way a shelf does.
 */
enum class AmiiboSort { Default, Name, Number, Release }

/**
 * How the library is presented.
 *
 * Three genuinely different jobs, not three sizes of the same list. Grid is for
 * recognising a figure by its artwork, Carousel is for unhurried browsing of a
 * few at a time, and List is for scanning and managing hundreds. All three read
 * the SAME query result — see [AmiiboGallery.build] — so switching between them
 * is a change of presentation and nothing else.
 */
enum class AmiiboViewMode { Grid, Carousel, List }

/**
 * What the user has narrowed the library to, and how they are looking at it.
 *
 * DELIBERATELY CARRIES NO SELECTION. Which item is selected cannot influence
 * which items are shown or their order, and keeping selection out of this record
 * is what makes that true by construction rather than by discipline: there is no
 * way to write a projection that depends on it.
 *
 * That separation is the fix for a real defect found on Windows, where the page
 * rebuilt the whole browser from its selection handler and threw away the scroll
 * position on every click. Android's `LazyGrid` would lose its scroll position
 * the same way if the item list were rebuilt per selection.
 */
data class AmiiboGalleryFilters(
    val search: String = "",
    val gameSeries: String = "",
    val amiiboSeries: String = "",
    val type: String = "",
    val sort: AmiiboSort = AmiiboSort.Default,
    val descending: Boolean = false,
    /** Presentation only. Never affects which items are produced. */
    val view: AmiiboViewMode = AmiiboViewMode.Grid,
) {
    /**
     * True when anything is NARROWING the library.
     *
     * Sort and view are excluded on purpose: they are preferences, not filters,
     * and "Clear filters" must not silently reset them.
     */
    val any: Boolean
        get() = search.isNotEmpty() || gameSeries.isNotEmpty() ||
            amiiboSeries.isNotEmpty() || type.isNotEmpty()

    /**
     * Everything that decides WHICH cards are produced, and in what order.
     *
     * Excludes [view], because changing view must not re-run the query, and
     * excludes selection, which is not here at all.
     */
    val queryIdentity: String
        get() = "$search$gameSeries$amiiboSeries$type$sort$descending"
}

/** One item in the library browser, in whichever view is showing. */
data class AmiiboCard(
    val id: String,
    /** What to call it: the catalog's name, else the user's. */
    val title: String,
    /** Series and type, or the tag's own facts when the catalog is silent. */
    val subtitle: String,
    val imageUrl: String = "",
    /** The user's own name, when it differs from the catalog's. */
    val ownName: String = "",
    val onAdapter: Boolean = false,
    val changed: Boolean = false,
    val gameSeries: String = "",
    val amiiboSeries: String = "",
    val releaseDate: String = "",
    val figureId: String = "",
) {
    val hasArtwork: Boolean get() = imageUrl.isNotEmpty()

    /** A short badge, or empty. It sits on a tile, so it stays short. */
    val badge: String
        get() = when {
            changed -> "Changed"
            onAdapter -> "On adapter"
            else -> ""
        }
}

/**
 * The choices the filter controls should offer.
 *
 * Derived from what the user owns, not from the whole catalog: a menu listing
 * every Amiibo series in existence when the library holds three figures is a
 * worse control than one listing those three's series.
 */
data class AmiiboGalleryOptions(
    val gameSeries: List<String> = emptyList(),
    val amiiboSeries: List<String> = emptyList(),
    val types: List<String> = emptyList(),
)

/**
 * The library as a browsable gallery: searched, filtered, sorted, illustrated.
 *
 * A DELIBERATE MIRROR OF THE WINDOWS `AmiiboGallery`. The same search fields,
 * the same sort modes and tie-breaks, the same filter semantics, so the same
 * query produces equivalent results on both platforms. Behavioural parity is
 * what matters here; the two UIs look nothing alike and should not.
 *
 * Pure, and the catalog arrives as a lookup function rather than as a
 * dependency, so the ordering and matching rules are testable without Android.
 */
object AmiiboGallery {

    /**
     * Whether the adapter is holding THIS backup.
     *
     * ONE RULE, IN ONE PLACE. The browser needs it to badge a card, the detail
     * pane needs it to decide whether Present, Eject and Sync act on the figure
     * being described, and the two disagreeing is not a cosmetic problem: those
     * commands operate on whatever is resident, so a pane that offers them for a
     * figure the adapter is not holding invites the user to eject or overwrite
     * something they are not looking at.
     *
     * The UID is the only identity the adapter reports that survives a rename,
     * and a blank one matches nothing rather than everything — an adapter that
     * has not said what it holds is not evidence that it holds this.
     */
    fun residentOn(item: AmiiboLibraryItem, adapterUid: String, adapterHasAmiibo: Boolean): Boolean =
        adapterHasAmiibo && item.uid.isNotBlank() && item.uid.equals(adapterUid, ignoreCase = true)

    /** The library id the adapter is holding, if the phone has a copy of it. */
    fun residentId(
        library: List<AmiiboLibraryItem>,
        adapterUid: String,
        adapterHasAmiibo: Boolean,
    ): String? = library.firstOrNull { residentOn(it, adapterUid, adapterHasAmiibo) }?.id

    fun build(
        library: List<AmiiboLibraryItem>,
        catalog: (String) -> AmiiboCatalogEntry?,
        loadedId: String?,
        filters: AmiiboGalleryFilters,
    ): List<AmiiboCard> {
        val rows = library
            .map { item -> Row(card(item, catalog(item.figureId), item.id == loadedId), catalog(item.figureId), item) }
            .filter { matches(it, filters) }

        return order(rows, filters).map { it.card }
    }

    fun options(
        library: List<AmiiboLibraryItem>,
        catalog: (String) -> AmiiboCatalogEntry?,
    ): AmiiboGalleryOptions {
        val entries = library.mapNotNull { catalog(it.figureId) }
        return AmiiboGalleryOptions(
            gameSeries = distinct(entries.map { it.gameSeries }),
            amiiboSeries = distinct(entries.map { it.amiiboSeries }),
            types = distinct(entries.map { it.type }),
        )
    }

    private fun distinct(values: List<String>): List<String> =
        values.filter { it.isNotBlank() }
            .distinctBy { it.lowercase() }
            .sortedBy { it.lowercase() }

    private data class Row(
        val card: AmiiboCard,
        val entry: AmiiboCatalogEntry?,
        val item: AmiiboLibraryItem,
    )

    private fun card(
        item: AmiiboLibraryItem,
        entry: AmiiboCatalogEntry?,
        onAdapter: Boolean,
    ): AmiiboCard {
        val known = entry?.name?.takeIf { it.isNotBlank() }
            ?: entry?.character?.takeIf { it.isNotBlank() }

        // The catalog's name leads when there is one, because "Tom Nook" is what
        // the figure IS. The user's own name is kept alongside rather than
        // discarded: they may well have called it "Nook for trading".
        val title = known ?: item.displayName
        val own = if (known != null && !known.equals(item.displayName, ignoreCase = true)) {
            item.displayName
        } else {
            ""
        }

        val subtitle = buildList {
            entry?.gameSeries?.takeIf { it.isNotBlank() }?.let(::add)
            entry?.type?.takeIf { it.isNotBlank() }?.let(::add)
            if (isEmpty()) {
                // Nothing from the catalog: show what the tag itself says rather
                // than an empty line.
                add(if (item.tagType == AmiiboTagType.FigureV3) "Figure v3" else "NTAG215")
                add(item.figureId)
            }
        }.joinToString(" · ")

        return AmiiboCard(
            id = item.id,
            title = title,
            subtitle = subtitle,
            imageUrl = entry?.imageUrl.orEmpty(),
            ownName = own,
            onAdapter = onAdapter,
            changed = item.dirtyFromAdapter,
            gameSeries = entry?.gameSeries.orEmpty(),
            amiiboSeries = entry?.amiiboSeries.orEmpty(),
            releaseDate = entry?.releaseDate.orEmpty(),
            figureId = item.figureId,
        )
    }

    /**
     * Search matches anything a person might type.
     *
     * The user's own name, the catalog name, the character, both series, and the
     * figure id. Someone hunting for a tag does not know or care which of those
     * fields their memory of it came from.
     */
    private fun matches(row: Row, filters: AmiiboGalleryFilters): Boolean {
        if (filters.gameSeries.isNotEmpty() &&
            !row.entry?.gameSeries.orEmpty().equals(filters.gameSeries, ignoreCase = true)
        ) {
            return false
        }
        if (filters.amiiboSeries.isNotEmpty() &&
            !row.entry?.amiiboSeries.orEmpty().equals(filters.amiiboSeries, ignoreCase = true)
        ) {
            return false
        }
        if (filters.type.isNotEmpty() &&
            !row.entry?.type.orEmpty().equals(filters.type, ignoreCase = true)
        ) {
            return false
        }

        val needle = filters.search.trim()
        if (needle.isEmpty()) return true

        return listOf(
            row.item.displayName,
            row.entry?.name.orEmpty(),
            row.entry?.character.orEmpty(),
            row.entry?.gameSeries.orEmpty(),
            row.entry?.amiiboSeries.orEmpty(),
            row.item.figureId,
        ).any { it.contains(needle, ignoreCase = true) }
    }

    private fun order(rows: List<Row>, filters: AmiiboGalleryFilters): List<Row> {
        val ordered = when (filters.sort) {
            // Case-insensitive by lowercasing the key rather than passing a
            // comparator, so the tie-break chain stays one inferred type.
            AmiiboSort.Name -> rows.sortedWith(
                compareBy<Row> { it.card.title.lowercase() }
                    .thenBy { it.item.figureId },
            )

            // By figure id, which groups a series the way a shelf does.
            AmiiboSort.Number -> rows.sortedWith(
                compareBy<Row> { it.item.figureId }
                    .thenBy { it.card.title.lowercase() },
            )

            // Undated figures sort LAST rather than first: an empty string would
            // otherwise sort before every real date and put the unknowns on top.
            AmiiboSort.Release -> rows.sortedWith(
                compareBy<Row> { if (it.entry?.releaseDate.isNullOrBlank()) 1 else 0 }
                    .thenBy { it.entry?.releaseDate.orEmpty() }
                    .thenBy { it.card.title.lowercase() },
            )

            // The order they were added, newest first, so the tag someone just
            // imported is where they will look for it.
            AmiiboSort.Default -> rows.sortedByDescending { it.item.importedAtMillis }
        }

        return if (filters.descending) ordered.reversed() else ordered
    }
}
