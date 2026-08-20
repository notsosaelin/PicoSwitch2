package dev.picoswitch.companion.model

enum class ConnectionPhase { Idle, Scanning, Connecting, Connected, Reconnecting, Disconnecting, Failed }

data class ConnectionState(
    val phase: ConnectionPhase = ConnectionPhase.Idle,
    val deviceName: String? = null,
    val address: String? = null,
    val message: String? = null,
    val attempt: Int = 0,
) {
    val connected: Boolean get() = phase == ConnectionPhase.Connected
}

// Management domain truth lives in the Android-free :management-core module.
// These aliases preserve the existing presentation package while ensuring the
// app cannot grow a second copy of the firmware contract.
typealias Personality = dev.picoswitch.management.Personality
typealias FirmwareInfo = dev.picoswitch.management.FirmwareInfo
typealias ControllerInfo = dev.picoswitch.management.ControllerInfo
typealias RgbColor = dev.picoswitch.management.RgbColor
typealias AdapterConfig = dev.picoswitch.management.AdapterConfig
typealias PersonalityState = dev.picoswitch.management.PersonalityState
typealias AmiiboUpload = dev.picoswitch.management.AmiiboUpload
typealias AmiiboStatus = dev.picoswitch.management.AmiiboStatus
typealias BondInfo = dev.picoswitch.management.BondInfo
typealias BondPage = dev.picoswitch.management.BondPage
typealias BondEnumeration = dev.picoswitch.management.BondEnumeration
typealias AdapterInputSource = dev.picoswitch.management.AdapterInputSource
typealias AdapterInputState = dev.picoswitch.management.AdapterInputState
typealias CapabilityState = dev.picoswitch.management.CapabilityState
typealias AdapterCapabilities = dev.picoswitch.management.AdapterCapabilities
typealias AdapterSnapshot = dev.picoswitch.management.AdapterSnapshot

data class AmiiboLibraryItem(
    val id: String,
    val displayName: String,
    val fileName: String,
    val size: Int,
    val crc32: String,
    val uid: String,
    val figureId: String,
    val importedAtMillis: Long,
    val updatedAtMillis: Long = importedAtMillis,
    val dirtyFromAdapter: Boolean = false,
    val characterGameCode: String = "",
    val characterVariant: Int = 0,
    val tagType: AmiiboTagType = AmiiboTagType.Ntag215,
    val typeName: String = "Figure",
    val modelNumber: String = "",
    val seriesCode: Int = 0,
    val formatVersion: Int = 0,
    val extendedVariant: String = "",
)

enum class AmiiboTagType { Ntag215, FigureV3 }

data class AmiiboIdentity(
    val uid: String,
    val figureId: String,
    val tagType: AmiiboTagType,
    val characterGameCode: String,
    val characterVariant: Int,
    val typeName: String,
    val modelNumber: String,
    val seriesCode: Int,
    val formatVersion: Int,
    val extendedVariant: String,
)

/** Identity and decrypted register metadata for one local Amiibo copy. */
data class AmiiboDetails(
    val uid: String,
    val figureId: String,
    val tagType: AmiiboTagType,
    val size: Int,
    val crc32: String,
    val characterGameCode: String,
    val characterVariant: Int,
    val typeName: String,
    val modelNumber: String,
    val seriesCode: Int,
    val formatVersion: Int,
    val extendedVariant: String,
    val crypto: AmiiboCryptoState = AmiiboCryptoState.NotAttempted,
    val owner: String = "",
    val nickname: String = "",
    val setUp: Boolean = false,
    val setupDate: String? = null,
    val lastWriteDate: String? = null,
    val writeCounter: Int? = null,
    val hasAppData: Boolean? = null,
    val titleId: String = "",
    val appId: String = "",
    val appDataLabel: String = "",
)

enum class NfcScanPhase { Unavailable, Idle, Armed, Reading, Saving, Saved, Rejected }

data class NfcScanStatus(
    val phase: NfcScanPhase = NfcScanPhase.Unavailable,
    val message: String = "",
)

data class AmiiboCatalogEntry(
    val id: String,
    val character: String,
    val gameSeries: String,
    val amiiboSeries: String,
    val type: String,
    val releaseDate: String,
    val imageUrl: String,
    val games: Map<String, List<String>> = emptyMap(),
    val titleIds: Map<String, String> = emptyMap(),
    /** AmiiboAPI's friendly display name (kept separate from character). */
    val name: String = "",
)

/**
 * State of the optional friendly-name/catalog enrichment. Local Amiibo
 * identity and adapter operations remain usable in every state except the
 * transient lookup state.
 */
enum class AmiiboCatalogState { Idle, Loading, Available, Offline, Unmatched }

enum class AmiiboSortOrder { Name, Series, RecentlyAdded }

/** Deterministic library ordering used by both compact rows and the wide grid. */
fun sortAmiiboLibrary(
    items: List<AmiiboLibraryItem>,
    catalogById: Map<String, AmiiboCatalogEntry>,
    order: AmiiboSortOrder,
): List<AmiiboLibraryItem> = when (order) {
    AmiiboSortOrder.Name -> items.sortedWith(
        compareBy<AmiiboLibraryItem> { friendlyAmiiboName(it, catalogById[it.id]).lowercase() }
            .thenBy { it.id },
    )
    AmiiboSortOrder.Series -> items.sortedWith(
        compareBy<AmiiboLibraryItem> { friendlyAmiiboSeries(catalogById[it.id]).lowercase() }
            .thenBy { friendlyAmiiboName(it, catalogById[it.id]).lowercase() }
            .thenBy { it.id },
    )
    AmiiboSortOrder.RecentlyAdded -> items.sortedWith(
        compareByDescending<AmiiboLibraryItem> { it.importedAtMillis }
            .thenBy { friendlyAmiiboName(it, catalogById[it.id]).lowercase() }
            .thenBy { it.id },
    )
}

private fun friendlyAmiiboName(item: AmiiboLibraryItem, catalog: AmiiboCatalogEntry?): String =
    catalog?.name?.takeIf(String::isNotBlank) ?: catalog?.character?.takeIf(String::isNotBlank) ?: item.displayName

private fun friendlyAmiiboSeries(catalog: AmiiboCatalogEntry?): String =
    listOfNotNull(catalog?.gameSeries?.takeIf(String::isNotBlank), catalog?.amiiboSeries?.takeIf(String::isNotBlank))
        .joinToString(" · ")

/** Resolve the terminal state of an optional catalog lookup without hiding raw identity. */
fun resolveAmiiboCatalogState(found: Boolean, catalogAvailable: Boolean): AmiiboCatalogState = when {
    found -> AmiiboCatalogState.Available
    catalogAvailable -> AmiiboCatalogState.Unmatched
    else -> AmiiboCatalogState.Offline
}

enum class AmiiboCryptoState { NotAttempted, KeyUnavailable, Valid, Invalid }

data class OperationProgress(
    val label: String,
    val completed: Int,
    val total: Int,
) {
    val fraction: Float get() = if (total <= 0) 0f else completed.toFloat() / total.toFloat()
}
