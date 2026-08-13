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

enum class Personality(val wireName: String, val title: String) {
    Pro2("pro2", "Pro Controller 2"),
    GameCube("gc", "GameCube"),
    JoyConLeft("jcl", "Joy-Con 2 (L)"),
    JoyConRight("jcr", "Joy-Con 2 (R)"),
    Config("config", "Configuration"),
    Unknown("unknown", "Unknown");

    companion object {
        fun fromWire(value: String?) = entries.firstOrNull { it.wireName == value } ?: Unknown
    }
}

data class FirmwareInfo(val id: String = "", val product: String = "", val version: String = "")

data class ControllerInfo(
    val name: String = "No controller",
    val vid: Int = 0,
    val pid: Int = 0,
    val batteryValid: Boolean = false,
    val batteryPercent: Int = 0,
    val charging: Boolean = false,
) {
    val attached: Boolean get() = vid != 0 || pid != 0 ||
        (name.isNotBlank() && name != "No controller")
}

data class RgbColor(val red: Int, val green: Int, val blue: Int) {
    init {
        require(red in 0..255 && green in 0..255 && blue in 0..255)
    }
    fun wire() = "$red $green $blue"
    fun argb() = 0xFF000000L.or((red.toLong() shl 16)).or((green.toLong() shl 8)).or(blue.toLong())
}

data class AdapterConfig(
    val bodyColor: RgbColor = RgbColor(0, 0, 0),
    val leftAccent: RgbColor = RgbColor(0, 0, 0),
    val rightAccent: RgbColor = RgbColor(0, 0, 0),
)

data class PersonalityState(
    val current: Personality = Personality.Unknown,
    val available: List<Personality> = emptyList(),
)

data class AmiiboUpload(val active: Boolean = false, val received: Int = 0, val size: Int = 0)

data class AmiiboStatus(
    val loaded: Boolean = false,
    val dirty: Boolean = false,
    val presented: Boolean = false,
    val v3Loaded: Boolean = false,
    val persisted: Boolean = false,
    val persistPending: Boolean = false,
    val size: Int = 0,
    val signature: Boolean = false,
    val hasSave2: Boolean = false,
    val usingSave2: Boolean = false,
    val generation: Long = 0,
    val payloadCrc: String = "00000000",
    val uid: String = "",
    val figureId: String = "",
    val upload: AmiiboUpload = AmiiboUpload(),
)

data class BondInfo(val index: Int, val address: String, val name: String? = null, val type: Int? = null)

/** One bounded page from the versioned adapter bond enumeration. */
data class BondPage(val entries: List<BondInfo>, val total: Int, val next: Int?)

/** The aggregate result retained by the management repository. */
data class BondEnumeration(
    val entries: List<BondInfo>,
    val complete: Boolean,
    val total: Int? = null,
)

data class AdapterInputSource(
    val id: Long,
    val connection: Int,
    val transport: Int,
    val generation: Long,
    val name: String,
)

data class AdapterInputState(
    val activeId: Long = 0,
    val pendingId: Long = 0,
    val explicit: Boolean = false,
    val awaitingFresh: Boolean = false,
    val transitions: Long = 0,
    val sources: List<AdapterInputSource> = emptyList(),
    val truncated: Boolean = false,
) {
    val activeSource: AdapterInputSource? get() = sources.firstOrNull { it.id == activeId }
}

enum class CapabilityState { Available, Unsupported, Unknown }

data class AdapterCapabilities(
    val core: CapabilityState = CapabilityState.Unknown,
    val personality: CapabilityState = CapabilityState.Unknown,
    val colors: CapabilityState = CapabilityState.Unknown,
    val amiibo: CapabilityState = CapabilityState.Unknown,
    val managementGate: CapabilityState = CapabilityState.Unknown,
    val bonds: CapabilityState = CapabilityState.Unknown,
    val wake: CapabilityState = CapabilityState.Unknown,
    val activeInput: CapabilityState = CapabilityState.Unknown,
)

data class AdapterSnapshot(
    val firmware: FirmwareInfo = FirmwareInfo(),
    val controller: ControllerInfo = ControllerInfo(),
    val personality: PersonalityState = PersonalityState(),
    val config: AdapterConfig = AdapterConfig(),
    val amiibo: AmiiboStatus = AmiiboStatus(),
    val managementEnabled: Boolean? = null,
    val bonds: List<BondInfo> = emptyList(),
    /** False means a legacy/unversioned list was received; null means unavailable. */
    val bondsComplete: Boolean? = null,
    val bondsTotal: Int? = null,
    val input: AdapterInputState = AdapterInputState(),
    val capabilities: AdapterCapabilities = AdapterCapabilities(),
    val refreshedAtMillis: Long = 0,
)

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
