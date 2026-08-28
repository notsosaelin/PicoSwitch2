package dev.picoswitch.companion.data

import dev.picoswitch.management.PeerInfo
import dev.picoswitch.management.PeerInventory
import dev.picoswitch.management.PeerNaming
import dev.picoswitch.management.PeerRole
import dev.picoswitch.management.PeerTransport
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * What controllers an adapter has known, as opposed to what it has stored.
 *
 * TWO DIFFERENT QUESTIONS
 *
 * The adapter's peer inventory answers "which security records exist right now,
 * and what can I currently prove about their owners". It is authoritative and
 * the app never second-guesses it. But it is also amnesiac by construction:
 * role classification is live evidence only, so after the adapter reboots a
 * saved controller that has not reconnected is reported `unknown` with no name,
 * and a controller the user forgot last week is simply absent.
 *
 * This is the app-side half the design requires (§24.1). It remembers, per
 * adapter, what each peer was the last time the adapter could actually say —
 * and nothing else. It holds no key material, and it is never consulted to
 * contradict the adapter.
 *
 * WHY THE FIRMWARE HALF IS NOT HERE
 *
 * §24.2 offers persisting the same metadata on the adapter, which would make it
 * survive being managed from a different phone. It is deliberately not done:
 * that work was attempted after this phase's first pass, destabilised the
 * Bluetooth core, and was withdrawn. §24.2 is explicitly conditional
 * ("recommended if storage audit permits", "app-side history is sufficient for
 * initial release") and this is the sufficient half. Do not reintroduce adapter
 * flash writes for peer metadata without new evidence about that failure.
 *
 * WHAT HISTORY IS AND IS NOT ALLOWED TO CLAIM
 *
 * The management protocol requires that a client render `unknown` as
 * unidentified and never promote it to `controller`. History does not violate
 * that: it never rewrites [PeerInfo.role]. It supplies a *remembered* label and
 * a remembered role alongside the adapter's live answer, and presentation says
 * which is which. The one thing a remembered role does decide is exclusion —
 * a peer this app has proven to be the user's own phone stays out of the saved
 * controller list even when the adapter has forgotten what it is, because
 * offering to forget the user's phone as though it were a controller is the
 * specific failure the whole role model exists to prevent.
 */
data class PeerHistoryRecord(
    /** The adapter's opaque peer handle. Stable across reboots, not a slot index. */
    val peerId: String,
    val address: String,
    /** Best remote-supplied name ever seen for this peer. */
    val lastKnownName: String? = null,
    /** Best adapter-derived classification ever seen, e.g. `Sony DualSense`. */
    val classification: String? = null,
    val vendorId: Int = 0,
    val productId: Int = 0,
    /** The strongest role the adapter has ever actually proven for this peer. */
    val provenRole: PeerRole = PeerRole.Unknown,
    val transports: Set<PeerTransport> = emptySet(),
    val firstSeenAtMillis: Long = 0,
    /** When this peer was last present in an inventory read. */
    val lastSeenAtMillis: Long = 0,
    val lastConnectedAtMillis: Long? = null,
    /** Whether the adapter still held a security record at the last complete read. */
    val bonded: Boolean = false,
) {
    /** The best name this app can offer without the adapter's live answer. */
    val rememberedName: String
        get() = PeerNaming.label(
            address = address,
            classification = classification,
            name = lastKnownName,
            vendorId = vendorId,
            productId = productId,
        )

    /** The user's own phone, in either of its two relationships. */
    val isCompanionRole: Boolean
        get() = provenRole == PeerRole.ManagementCompanion || provenRole == PeerRole.ControllerLink
}

/** Everything one adapter has known. Keyed by peer id, which the adapter owns. */
data class AdapterPeerHistory(val records: List<PeerHistoryRecord> = emptyList()) {
    fun record(peerId: String): PeerHistoryRecord? = records.firstOrNull { it.peerId == peerId }

    /** Peers the adapter no longer holds a record for; the "Recent" section. */
    val forgotten: List<PeerHistoryRecord> get() = records.filterNot { it.bonded }

    fun without(peerId: String): AdapterPeerHistory =
        copy(records = records.filterNot { it.peerId == peerId })

    /**
     * Fold one COMPLETE inventory read into history.
     *
     * Complete is load-bearing and the caller must not pass a partial read: a
     * missing row would be indistinguishable from a peer the adapter has
     * forgotten, and this function would then mark a live saved controller as
     * historical.
     */
    fun observing(inventory: PeerInventory, nowMillis: Long): AdapterPeerHistory {
        require(inventory.complete) { "History may only observe a complete inventory read" }
        val seen = inventory.peers.associateBy { it.id }
        val updated = records.map { record ->
            seen[record.peerId]?.let { record.updatedFrom(it, nowMillis) }
            // Absent from a complete read means the adapter no longer holds a
            // record for it. The peer is kept -- that is the whole point of
            // history -- but it stops being a saved pairing.
                ?: record.copy(bonded = false)
        }
        val added = inventory.peers
            .filter { peer -> records.none { it.peerId == peer.id } }
            .map { peer -> newRecord(peer, nowMillis) }
        return copy(records = prune(updated + added))
    }

    private fun newRecord(peer: PeerInfo, nowMillis: Long) = PeerHistoryRecord(
        peerId = peer.id,
        address = peer.address,
        lastKnownName = peer.name,
        classification = peer.classification,
        vendorId = peer.vendorId,
        productId = peer.productId,
        provenRole = peer.role,
        transports = peer.transports,
        firstSeenAtMillis = nowMillis,
        lastSeenAtMillis = nowMillis,
        lastConnectedAtMillis = nowMillis.takeIf { peer.connected },
        bonded = peer.bonded,
    )

    /**
     * Keep the newest [MAX_RECORDS] and never evict something the adapter still
     * holds a key for. A saved pairing dropped from history would silently lose
     * the only name the app can show for it once the adapter reboots.
     */
    private fun prune(records: List<PeerHistoryRecord>): List<PeerHistoryRecord> {
        if (records.size <= MAX_RECORDS) return records
        val (kept, evictable) = records.partition { it.bonded }
        val room = (MAX_RECORDS - kept.size).coerceAtLeast(0)
        return kept + evictable.sortedByDescending { it.lastSeenAtMillis }.take(room)
    }

    companion object {
        /**
         * Twice the adapter's 32-record bond capacity, so a full adapter still
         * leaves room for a comparable number of previously-forgotten devices.
         */
        const val MAX_RECORDS = 64
    }
}

/**
 * Merge one live observation into what is already remembered.
 *
 * Every field takes the newer answer only when the newer answer says something.
 * The adapter reporting `unknown` after a reboot is not evidence that the peer
 * stopped being a controller, so it must not erase the role that was proven
 * while it was connected.
 */
private fun PeerHistoryRecord.updatedFrom(peer: PeerInfo, nowMillis: Long) = copy(
    address = peer.address.takeIf(String::isNotBlank) ?: address,
    lastKnownName = peer.name ?: lastKnownName,
    classification = peer.classification ?: classification,
    vendorId = if (peer.hasUsbIdentity) peer.vendorId else vendorId,
    productId = if (peer.hasUsbIdentity) peer.productId else productId,
    provenRole = strongerRole(provenRole, peer.role),
    // Transports accumulate: a peer seen only over LE this session still holds
    // the Classic key the adapter reported last session.
    transports = transports + peer.transports,
    firstSeenAtMillis = firstSeenAtMillis.takeIf { it > 0 } ?: nowMillis,
    lastSeenAtMillis = nowMillis,
    lastConnectedAtMillis = if (peer.connected) nowMillis else lastConnectedAtMillis,
    bonded = peer.bonded,
)

/**
 * Which of two role claims about one peer wins.
 *
 * Written out rather than derived from the enum's declaration order, for the
 * same reason the firmware's `role_precedence()` is: reordering the enum must
 * not silently change which role a phone that is both the management companion
 * and a Controller Link peer ends up with.
 */
internal fun strongerRole(a: PeerRole, b: PeerRole): PeerRole =
    if (rolePrecedence(b) > rolePrecedence(a)) b else a

private fun rolePrecedence(role: PeerRole): Int = when (role) {
    PeerRole.ManagementCompanion -> 3
    PeerRole.ControllerLink -> 2
    PeerRole.PhysicalController -> 1
    PeerRole.Unknown -> 0
}

/** History for every adapter this app knows. One document, one file. */
data class PeerHistoryBook(val byAdapter: Map<AdapterId, AdapterPeerHistory> = emptyMap()) {
    fun forAdapter(id: AdapterId?): AdapterPeerHistory =
        id?.let { byAdapter[it] } ?: AdapterPeerHistory()

    fun with(id: AdapterId, history: AdapterPeerHistory): PeerHistoryBook =
        copy(byAdapter = byAdapter + (id to history))

    /** Dropped with the adapter: history about an adapter the app no longer knows is orphaned. */
    fun without(id: AdapterId): PeerHistoryBook = copy(byAdapter = byAdapter - id)
}

/**
 * The persisted history document.
 *
 * Versioned, and decoding is total for the same reason the adapter registry's
 * is: this is read at startup, and a document this build cannot parse must cost
 * the user their history, never their ability to launch the app. One unreadable
 * row is dropped rather than failing its adapter, and one unreadable adapter is
 * dropped rather than failing the document.
 */
object PeerHistoryCodec {
    const val SCHEMA = 1

    private val json = Json { ignoreUnknownKeys = true }

    fun encode(book: PeerHistoryBook): String = buildJsonObject {
        put("schema", SCHEMA)
        put(
            "adapters",
            buildJsonArray {
                book.byAdapter.forEach { (id, history) ->
                    add(
                        buildJsonObject {
                            put("adapter", id.value)
                            put(
                                "peers",
                                buildJsonArray {
                                    history.records.forEach { record -> add(encodeRecord(record)) }
                                },
                            )
                        },
                    )
                }
            },
        )
    }.toString()

    fun decode(text: String?): PeerHistoryBook {
        if (text.isNullOrBlank()) return PeerHistoryBook()
        val root = runCatching { json.parseToJsonElement(text).jsonObject }.getOrNull()
            ?: return PeerHistoryBook()
        val schema = root["schema"]?.jsonPrimitive?.intOrNull ?: return PeerHistoryBook()
        // A future schema is not readable here. The document is left on disk
        // untouched so a later build can still read it.
        if (schema > SCHEMA) return PeerHistoryBook()
        val adapters = runCatching { root["adapters"]?.jsonArray }.getOrNull().orEmpty()
            .mapNotNull { element -> runCatching { decodeAdapter(element.jsonObject) }.getOrNull() }
        return PeerHistoryBook(adapters.toMap())
    }

    private fun encodeRecord(record: PeerHistoryRecord): JsonObject = buildJsonObject {
        put("id", record.peerId)
        put("addr", record.address)
        record.lastKnownName?.let { put("name", it) }
        record.classification?.let { put("class", it) }
        if (record.vendorId != 0 || record.productId != 0) {
            put("vid", record.vendorId)
            put("pid", record.productId)
        }
        put("role", record.provenRole.wireName)
        put("tr", record.transports.sumOf { it.bit })
        put("firstSeen", record.firstSeenAtMillis)
        put("lastSeen", record.lastSeenAtMillis)
        record.lastConnectedAtMillis?.let { put("lastConnected", it) }
        if (record.bonded) put("bonded", true)
    }

    private fun decodeAdapter(row: JsonObject): Pair<AdapterId, AdapterPeerHistory>? {
        val id = AdapterId.fromAddress(row["adapter"]?.jsonPrimitive?.contentOrNull) ?: return null
        val records = runCatching { row["peers"]?.jsonArray }.getOrNull().orEmpty()
            .mapNotNull { element -> runCatching { decodeRecord(element.jsonObject) }.getOrNull() }
            .distinctBy { it.peerId }
        return id to AdapterPeerHistory(records)
    }

    private fun decodeRecord(row: JsonObject): PeerHistoryRecord? {
        val peerId = row["id"]?.jsonPrimitive?.contentOrNull?.takeIf(String::isNotBlank) ?: return null
        return PeerHistoryRecord(
            peerId = peerId.take(MAX_TEXT),
            address = row["addr"]?.jsonPrimitive?.contentOrNull.orEmpty().take(MAX_TEXT),
            // Re-sanitised on read. These strings originated as untrusted remote
            // Bluetooth names; that they entered clean is not proof the bytes on
            // disk still are, and they go straight back into the UI.
            lastKnownName = PeerText.sanitize(row["name"]?.jsonPrimitive?.contentOrNull),
            classification = PeerText.sanitize(row["class"]?.jsonPrimitive?.contentOrNull),
            vendorId = row["vid"]?.jsonPrimitive?.intOrNull?.coerceIn(0, 0xFFFF) ?: 0,
            productId = row["pid"]?.jsonPrimitive?.intOrNull?.coerceIn(0, 0xFFFF) ?: 0,
            provenRole = PeerRole.fromWire(row["role"]?.jsonPrimitive?.contentOrNull),
            transports = PeerTransport.fromMask(row["tr"]?.jsonPrimitive?.intOrNull ?: 0),
            firstSeenAtMillis = row["firstSeen"]?.jsonPrimitive?.longOrNull ?: 0,
            lastSeenAtMillis = row["lastSeen"]?.jsonPrimitive?.longOrNull ?: 0,
            lastConnectedAtMillis = row["lastConnected"]?.jsonPrimitive?.longOrNull,
            bonded = row["bonded"]?.jsonPrimitive?.booleanOrNull == true,
        )
    }

    private const val MAX_TEXT = 64
}

/**
 * Cleaning for text that came off the radio.
 *
 * The adapter already reduces remote names to printable ASCII before they reach
 * the wire, so this is the second line rather than the first: it exists because
 * this app also reads these strings back from its own storage, where they could
 * have been edited, and because a name reaching a diagnostic line must not be
 * able to split it.
 */
object PeerText {
    const val MAX_LENGTH = 48

    fun sanitize(raw: String?): String? = raw.orEmpty()
        .map { if (it.isISOControl()) ' ' else it }
        .joinToString("")
        .trim()
        .replace(WHITESPACE_RUN, " ")
        .take(MAX_LENGTH)
        .trim()
        .takeIf(String::isNotEmpty)

    private val WHITESPACE_RUN = Regex("\\s+")
}
