package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
import dev.picoswitch.companion.model.AmiiboCryptoState
import dev.picoswitch.companion.model.AmiiboDetails
import dev.picoswitch.companion.model.AmiiboLibraryItem
import dev.picoswitch.companion.model.AmiiboTagType

/**
 * Which part of the selected Amiibo is being inspected.
 *
 * The same three the Windows companion uses. Only one category's rows are built
 * at a time — rendering all of them into one column is what turns an inspector
 * into a scrolling settings form.
 */
enum class AmiiboCategory { Overview, Tag, Adapter }

/**
 * One label/value line.
 *
 * @param monospace for identifiers a person compares character by character — a
 * UID or a figure id — where proportional digits make scanning harder.
 */
data class AmiiboDetailRow(
    val label: String,
    val value: String,
    val monospace: Boolean = false,
)

/** A named group of related lines. */
data class AmiiboDetailGroup(val title: String, val rows: List<AmiiboDetailRow>) {
    val any: Boolean get() = rows.isNotEmpty()
}

/**
 * The selected Amiibo, organised the way someone actually reads it.
 *
 * A DELIBERATE MIRROR of the Windows `AmiiboInspection`, so the two companions
 * describe a figure with the same groups, the same labels and the same omission
 * rules. The UIs look nothing alike; what a user is told should not.
 *
 * Groups are omitted when they have nothing to say. A tag that was never set up
 * has no owner, no nickname and no dates, and printing five empty lines implies
 * the information exists and is missing when the tag is simply blank.
 *
 * Pure, so the grouping and omission rules are tested without Compose.
 */
object AmiiboInspection {

    fun build(
        item: AmiiboLibraryItem,
        catalog: AmiiboCatalogEntry?,
        decoded: AmiiboDetails?,
        onAdapter: Boolean,
        adapterChanged: Boolean,
    ): List<AmiiboDetailGroup> = listOfNotNull(
        AmiiboDetailGroup("Identity", identity(item, catalog)),
        AmiiboDetailGroup("Tag", tag(item, decoded)),
        registration(decoded).takeIf { it.isNotEmpty() }
            ?.let { AmiiboDetailGroup("Registration", it) },
        AmiiboDetailGroup("Game data", gameData(decoded, catalog)),
        AmiiboDetailGroup("Adapter", adapter(onAdapter, adapterChanged)),
    ).filter { it.any }

    /** The rows for one category, which is all the UI ever shows at once. */
    fun forCategory(
        category: AmiiboCategory,
        groups: List<AmiiboDetailGroup>,
    ): List<AmiiboDetailGroup> {
        val wanted = when (category) {
            AmiiboCategory.Overview -> setOf("Identity")
            AmiiboCategory.Tag -> setOf("Tag", "Registration", "Game data")
            AmiiboCategory.Adapter -> setOf("Adapter")
        }
        return groups.filter { it.title in wanted }
    }

    private fun identity(
        item: AmiiboLibraryItem,
        catalog: AmiiboCatalogEntry?,
    ): List<AmiiboDetailRow> = buildList {
        add(AmiiboDetailRow("Your name", item.displayName))

        val figure = catalog?.name?.takeIf { it.isNotBlank() }
            ?: catalog?.character?.takeIf { it.isNotBlank() }

        // Only when it says something the user's own name does not.
        if (figure != null && !figure.equals(item.displayName, ignoreCase = true)) {
            add(AmiiboDetailRow("Figure", figure))
        }

        catalog?.gameSeries?.takeIf { it.isNotBlank() }
            ?.let { add(AmiiboDetailRow("Game series", it)) }
        catalog?.amiiboSeries?.takeIf { it.isNotBlank() }
            ?.let { add(AmiiboDetailRow("Collection", it)) }
        catalog?.type?.takeIf { it.isNotBlank() }
            ?.let { add(AmiiboDetailRow("Type", it)) }
        catalog?.releaseDate?.takeIf { it.isNotBlank() }
            ?.let { add(AmiiboDetailRow("Released", it)) }

        add(AmiiboDetailRow("Figure ID", item.figureId, monospace = true))
    }

    private fun tag(
        item: AmiiboLibraryItem,
        decoded: AmiiboDetails?,
    ): List<AmiiboDetailRow> = listOf(
        AmiiboDetailRow(
            "Tag type",
            if (item.tagType == AmiiboTagType.FigureV3) "Figure v3" else "NTAG215",
        ),
        AmiiboDetailRow("UID", item.uid, monospace = true),
        AmiiboDetailRow("Size", "${item.size} bytes"),
        AmiiboDetailRow("Contents", contents(decoded)),
    )

    /**
     * What could be read from the encrypted body, in the user's terms.
     *
     * Three distinguishable answers, because they need three different actions:
     * import a key, investigate a bad file, or nothing at all.
     */
    private fun contents(decoded: AmiiboDetails?): String = when (decoded?.crypto) {
        AmiiboCryptoState.Valid -> if (decoded.setUp) "Set up" else "Blank, never set up"
        AmiiboCryptoState.Invalid -> "Could not be decrypted with the imported keys"
        else -> "Import your Amiibo keys to read this"
    }

    private fun registration(decoded: AmiiboDetails?): List<AmiiboDetailRow> {
        if (decoded?.crypto != AmiiboCryptoState.Valid || !decoded.setUp) return emptyList()
        return buildList {
            decoded.nickname.takeIf { it.isNotBlank() }
                ?.let { add(AmiiboDetailRow("Nickname", it)) }
            decoded.owner.takeIf { it.isNotBlank() }
                ?.let { add(AmiiboDetailRow("Owner", it)) }
            decoded.setupDate?.takeIf { it.isNotBlank() }
                ?.let { add(AmiiboDetailRow("Registered", it)) }
            decoded.lastWriteDate?.takeIf { it.isNotBlank() }
                ?.let { add(AmiiboDetailRow("Last written", it)) }
            decoded.writeCounter?.takeIf { it > 0 }
                ?.let { add(AmiiboDetailRow("Times written", it.toString())) }
        }
    }

    private fun gameData(
        decoded: AmiiboDetails?,
        catalog: AmiiboCatalogEntry?,
    ): List<AmiiboDetailRow> {
        if (decoded?.crypto != AmiiboCryptoState.Valid) return emptyList()
        if (decoded.hasAppData != true) return listOf(AmiiboDetailRow("Game", "None"))

        // The catalog can name the game the firmware's small built-in table
        // cannot; its answer is preferred when it has one.
        val named = decoded.titleId.takeIf { it.isNotBlank() }
            ?.let { titleId -> catalog?.titleIds?.entries?.firstOrNull { it.key.equals(titleId, true) }?.value }

        return buildList {
            add(AmiiboDetailRow("Game", named ?: decoded.appDataLabel))
            decoded.titleId.takeIf { it.isNotBlank() }
                ?.let { add(AmiiboDetailRow("Title ID", it, monospace = true)) }
        }
    }

    private fun adapter(onAdapter: Boolean, changed: Boolean): List<AmiiboDetailRow> = listOf(
        AmiiboDetailRow(
            "Status",
            when {
                changed -> "On the adapter, changed by the console"
                onAdapter -> "On the adapter"
                else -> "Not on the adapter"
            },
        ),
    )
}
