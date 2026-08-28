package dev.picoswitch.companion.data

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * Identity of one physical adapter, as this app knows it.
 *
 * WHY THE ADDRESS AND NOT SOMETHING NEW
 *
 * The management peripheral advertises with `BD_ADDR_TYPE_LE_PUBLIC`
 * (`config_ble_start_advertising` in `btstack_host.c`), so the adapter already
 * has one stable public identifier that this app can see, that Android resolves
 * bonds against, and that costs no extra broadcast. Inventing a firmware-issued
 * ID and advertising it would add a permanent radio identifier for no benefit
 * the address does not already provide — see the Phase 0 audit,
 * `docs/bluetooth/bt-management-2.0-phase0-audit.md` §7.1.
 *
 * It is wrapped rather than passed as a bare `String` for two reasons. It makes
 * "identity" impossible to confuse with the user's alias or with a list
 * position, which the design forbids as identity; and if a firmware-supplied
 * identity is ever added, only [fromAddress] and the codec change.
 *
 * Strong evidence, not confirmed: the public address survives a firmware flash.
 * The whole repair path depends on it (a reflashed adapter answers at the same
 * address with no key, which is what `AdapterResetSignature` matches), but it
 * has never been the subject of its own test. [AdapterRegistryReconciler] must
 * therefore keep treating an identity change as a real possibility.
 */
@JvmInline
value class AdapterId(val value: String) {
    /**
     * Four hex characters for disambiguating two adapters the user gave the
     * same alias. Presentation only; never an identity of its own.
     */
    val shortLabel: String get() = value.replace(":", "").takeLast(4)

    companion object {
        private val MAC = Regex("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}")

        /** Null for anything that is not a Bluetooth address; callers must not invent one. */
        fun fromAddress(address: String?): AdapterId? =
            address?.takeIf { MAC.matches(it) }?.let { AdapterId(it.uppercase()) }
    }
}

/**
 * One adapter this app remembers.
 *
 * Everything here except [id] and [address] is cache or presentation. None of it
 * is authoritative about the adapter's own state: [lastFirmwareVersion] and
 * [lastPersonality] are what was true at the last verified connection and exist
 * so the adapter list can say something honest about an adapter that is not
 * currently connected. Live truth still comes from `AdapterSnapshot` and belongs
 * to whichever adapter is connected right now.
 */
data class AdapterRecord(
    val id: AdapterId,
    val address: String,
    val associationId: Int? = null,
    val userAlias: String? = null,
    val lastKnownName: String = DEFAULT_PRODUCT_NAME,
    val lastSeenAtMillis: Long? = null,
    val lastConnectedAtMillis: Long? = null,
    val lastFirmwareVersion: String? = null,
    val lastPersonality: String? = null,
    /**
     * Android believes it is bonded but this adapter rejected or lacked the key
     * — what a reflash looks like from the phone. Per-adapter on purpose:
     * reflashing one adapter must not push the others into repair.
     */
    val repairRequired: Boolean = false,
) {
    /**
     * Display priority: user alias, then the last known Bluetooth/product name,
     * then product plus a short identity suffix. The alias never becomes
     * identity and is never written to the adapter.
     */
    val displayName: String
        get() = userAlias?.takeIf(String::isNotBlank)
            ?: lastKnownName.takeIf(String::isNotBlank)
            ?: "$DEFAULT_PRODUCT_NAME ${id.shortLabel}"

    /** The value the existing connection lifecycle consumes. */
    fun toRelationship() = AdapterRelationship(
        address = address,
        associationId = associationId,
        displayName = displayName,
    )

    companion object {
        const val DEFAULT_PRODUCT_NAME = "PicoSwitch2"

        fun of(address: String, associationId: Int? = null, name: String? = null): AdapterRecord? {
            val id = AdapterId.fromAddress(address) ?: return null
            return AdapterRecord(
                id = id,
                address = id.value,
                associationId = associationId,
                lastKnownName = name?.takeIf(String::isNotBlank) ?: DEFAULT_PRODUCT_NAME,
            )
        }
    }
}

/**
 * Every adapter this app knows, plus which one is currently selected.
 *
 * Many known adapters, at most one active management session (design §9.1). The
 * registry deliberately holds no live connection state: it is what survives
 * process death, and a live GATT session does not.
 */
data class AdapterRegistry(
    val records: List<AdapterRecord> = emptyList(),
    val activeId: AdapterId? = null,
) {
    val active: AdapterRecord? get() = record(activeId)

    fun record(id: AdapterId?): AdapterRecord? = id?.let { wanted -> records.firstOrNull { it.id == wanted } }

    fun record(address: String?): AdapterRecord? = record(AdapterId.fromAddress(address))

    /** Insert or replace by identity, preserving list order for existing rows. */
    fun with(record: AdapterRecord): AdapterRegistry {
        val index = records.indexOfFirst { it.id == record.id }
        val next = if (index < 0) records + record else records.toMutableList().also { it[index] = record }
        return copy(records = next)
    }

    fun update(id: AdapterId, transform: (AdapterRecord) -> AdapterRecord): AdapterRegistry =
        record(id)?.let { with(transform(it)) } ?: this

    /**
     * Remove one adapter from the app. This is not a Bluetooth operation: the
     * Android bond and the adapter's own bonds are untouched.
     */
    fun without(id: AdapterId): AdapterRegistry = copy(
        records = records.filterNot { it.id == id },
        activeId = activeId.takeUnless { it == id },
    )

    /** Selecting an unknown adapter is meaningless; it clears the selection instead. */
    fun selecting(id: AdapterId?): AdapterRegistry = copy(activeId = id?.takeIf { record(it) != null })

    /**
     * Whether this row must show its short identity to stay distinguishable.
     *
     * Duplicate aliases are allowed — the design says so — but two rows reading
     * "Living Room" with no way to tell them apart is not a list, it is a
     * coin toss.
     */
    fun needsShortLabel(record: AdapterRecord): Boolean =
        records.count { it.displayName.equals(record.displayName, ignoreCase = true) } > 1
}

/**
 * User-supplied adapter names are untrusted text that reaches the UI, the
 * diagnostic log and persisted JSON, so they are cleaned at the one point where
 * they enter the app rather than at each place they are displayed.
 */
object AdapterAlias {
    const val MAX_LENGTH = 40

    /**
     * Returns null for anything that is empty once cleaned, which is how the
     * user clears an alias and falls back to the adapter's own name.
     */
    fun sanitize(raw: String?): String? {
        val cleaned = raw.orEmpty()
            // Control characters cover the newline that would otherwise split a
            // diagnostic line, and the tab/NUL that survive an ordinary trim.
            .map { if (it.isISOControl()) ' ' else it }
            .joinToString("")
            .trim()
            .replace(WHITESPACE_RUN, " ")
        return cleaned.take(MAX_LENGTH).trim().takeIf(String::isNotEmpty)
    }

    private val WHITESPACE_RUN = Regex("\\s+")
}

/**
 * The persisted registry document.
 *
 * Versioned from the start (design §58). Decoding is deliberately total: a
 * document this app cannot read must degrade to "no adapters known", never to a
 * crash on launch, because the registry is read before anything else can be
 * shown. Individual unreadable rows are dropped rather than failing the whole
 * document, so one corrupt entry cannot cost the user their other adapters.
 */
object AdapterRegistryCodec {
    const val SCHEMA = 1

    private val json = Json { ignoreUnknownKeys = true }

    fun encode(registry: AdapterRegistry): String = buildJsonObject {
        put("schema", SCHEMA)
        registry.activeId?.let { put("active", it.value) }
        put(
            "adapters",
            buildJsonArray {
                registry.records.forEach { record ->
                    add(
                        buildJsonObject {
                            put("id", record.id.value)
                            put("address", record.address)
                            record.associationId?.let { put("association", it) }
                            record.userAlias?.let { put("alias", it) }
                            put("name", record.lastKnownName)
                            record.lastSeenAtMillis?.let { put("lastSeen", it) }
                            record.lastConnectedAtMillis?.let { put("lastConnected", it) }
                            record.lastFirmwareVersion?.let { put("firmware", it) }
                            record.lastPersonality?.let { put("personality", it) }
                            if (record.repairRequired) put("repair", true)
                        },
                    )
                }
            },
        )
    }.toString()

    fun decode(text: String?): AdapterRegistry {
        if (text.isNullOrBlank()) return AdapterRegistry()
        val root = runCatching { json.parseToJsonElement(text).jsonObject }.getOrNull() ?: return AdapterRegistry()
        // An unknown future schema is not readable by this build. Returning an
        // empty registry is wrong for the user but safe; the document is left on
        // disk untouched so a later build can still read it.
        val schema = root["schema"]?.jsonPrimitive?.intOrNull ?: return AdapterRegistry()
        if (schema > SCHEMA) return AdapterRegistry()
        val records = runCatching { root["adapters"]?.jsonArray }.getOrNull().orEmpty()
            .mapNotNull { element -> runCatching { decodeRecord(element.jsonObject) }.getOrNull() }
            .distinctBy { it.id }
        val active = AdapterId.fromAddress(root["active"]?.jsonPrimitive?.contentOrNull)
        return AdapterRegistry(records, active).selecting(active)
    }

    /**
     * Adopt the single-adapter store this replaces.
     *
     * The migrated adapter becomes the active one, because it is the adapter
     * the user was already using; anything else would silently disconnect them
     * on upgrade. The legacy store is not cleared — it costs one preferences
     * file and makes a bad migration recoverable by hand.
     */
    fun migrate(legacy: AdapterRelationship?): AdapterRegistry {
        val record = legacy?.let {
            AdapterRecord.of(it.address, it.associationId, it.displayName)
        } ?: return AdapterRegistry()
        return AdapterRegistry(listOf(record), record.id)
    }

    private fun decodeRecord(row: JsonObject): AdapterRecord? {
        val address = row["address"]?.jsonPrimitive?.contentOrNull ?: row["id"]?.jsonPrimitive?.contentOrNull
        val id = AdapterId.fromAddress(address) ?: return null
        return AdapterRecord(
            id = id,
            address = id.value,
            associationId = row["association"]?.jsonPrimitive?.intOrNull,
            // Re-sanitize on read: the document may have been hand-edited, and a
            // value that entered clean is not proof the bytes on disk still are.
            userAlias = AdapterAlias.sanitize(row["alias"]?.jsonPrimitive?.contentOrNull),
            lastKnownName = AdapterAlias.sanitize(row["name"]?.jsonPrimitive?.contentOrNull)
                ?: AdapterRecord.DEFAULT_PRODUCT_NAME,
            lastSeenAtMillis = row["lastSeen"]?.jsonPrimitive?.longOrNull,
            lastConnectedAtMillis = row["lastConnected"]?.jsonPrimitive?.longOrNull,
            lastFirmwareVersion = row["firmware"]?.jsonPrimitive?.contentOrNull?.take(MAX_CACHED_TEXT),
            lastPersonality = row["personality"]?.jsonPrimitive?.contentOrNull?.take(MAX_CACHED_TEXT),
            repairRequired = row["repair"]?.jsonPrimitive?.booleanOrNull == true,
        )
    }

    private const val MAX_CACHED_TEXT = 32
}
