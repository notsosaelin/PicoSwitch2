package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.*
import kotlinx.serialization.json.*

/** Exact newline-JSON management contract shared with config.c and the Web Portal. */
object ManagementProtocol {
    const val SERVICE_UUID = "7c5ad4ed-2731-417c-b316-058505c7c083"
    const val RX_UUID = "5252186a-817f-489f-ad75-94c3bd444769"
    const val TX_UUID = "81462706-8e64-407a-bc3d-d303529fbe1c"
    const val MAX_COMMAND_BYTES = 127
    /** Firmware bridge buffer is 512 bytes including its terminating newline. */
    const val MAX_REPLY_PAYLOAD_BYTES = 511
    const val MIN_GATT_PAYLOAD = 20
    const val AMIIBO_CHUNK_BYTES = 32
    const val BONDS_PROTOCOL_VERSION = 2

    private val json = Json { ignoreUnknownKeys = true }

    fun frame(command: String): ByteArray {
        require(command.isNotBlank()) { "Command cannot be blank" }
        require(!command.contains('\n') && !command.contains('\r')) { "Command must be one line" }
        val commandBytes = command.encodeToByteArray()
        require(commandBytes.size <= MAX_COMMAND_BYTES) { "Command exceeds $MAX_COMMAND_BYTES bytes" }
        return commandBytes + '\n'.code.toByte()
    }

    fun chunks(command: String, payloadBytes: Int): List<ByteArray> {
        require(payloadBytes >= MIN_GATT_PAYLOAD)
        return frame(command).toList().chunked(payloadBytes).map { it.toByteArray() }
    }

    fun requireReplyWithinLimit(payloadBytes: Int) {
        if (payloadBytes > MAX_REPLY_PAYLOAD_BYTES) throw ManagementReplyTooLargeException(
            "Adapter reply exceeded the 511-byte wireless limit. A large bond list is the likely cause; no partial data was accepted.",
        )
    }

    fun objectOrThrow(command: String, response: String): JsonObject {
        val root = try {
            json.parseToJsonElement(response.trim()).jsonObject
        } catch (error: Exception) {
            throw ManagementException("Adapter returned malformed JSON for '$command'", error)
        }
        root["error"]?.jsonPrimitive?.contentOrNull?.let { message ->
            val code = root["code"]?.jsonPrimitive?.intOrNull
            throw AdapterCommandException(command, code, message)
        }
        return root
    }

    fun firmware(value: JsonObject) = FirmwareInfo(
        id = value.string("id"), product = value.string("product"), version = value.string("version"),
        // Optional: absent on firmware that predates contract reporting, which is
        // itself the signal that it is older than this app. Never defaulted to
        // the expected version -- see BridgeContract.Compatibility.Unknown.
        bridgeContract = value.int("bridge_contract"),
        build = value.string("build"),
    ).also { requireShape(it.id.isNotBlank() && it.version.isNotBlank(), "info") }

    fun controller(value: JsonObject) = ControllerInfo(
        name = value.string("name", "No controller").ifBlank { "No controller" },
        vid = value.int("vid"), pid = value.int("pid"),
        batteryValid = value.boolInt("batteryValid"),
        batteryPercent = value.int("battery"), charging = value.boolInt("charging"),
    )

    fun personality(value: JsonObject) = PersonalityState(
        current = Personality.fromWire(value.string("current")),
        available = value["available"]?.jsonArray?.map { Personality.fromWire(it.jsonPrimitive.content) }
            ?: emptyList(),
    ).also { requireShape(it.current != Personality.Unknown && it.available.isNotEmpty(), "personality") }

    fun config(value: JsonObject): AdapterConfig {
        requireShape(value.containsKey("body_color") && value.containsKey("joycon2_left_accent") && value.containsKey("joycon2_right_accent"), "get")
        fun color(key: String): RgbColor {
            val a = value[key]?.jsonArray ?: return RgbColor(0, 0, 0)
            return RgbColor(a.getOrNull(0)?.jsonPrimitive?.intOrNull ?: 0,
                a.getOrNull(1)?.jsonPrimitive?.intOrNull ?: 0,
                a.getOrNull(2)?.jsonPrimitive?.intOrNull ?: 0)
        }
        return AdapterConfig(color("body_color"), color("joycon2_left_accent"), color("joycon2_right_accent"))
    }

    fun amiibo(value: JsonObject) = AmiiboStatus(
        loaded = value.bool("loaded"), dirty = value.bool("dirty"),
        presented = value.bool("presented"), v3Loaded = value.bool("v3loaded"),
        persisted = value.bool("persisted"), persistPending = value.bool("persistPending"),
        size = value.int("size"), signature = value.bool("signature"),
        hasSave2 = value.bool("hasSave2"), usingSave2 = value.bool("usingSave2"),
        generation = value.long("generation"), payloadCrc = value.string("payloadCrc", "00000000"),
        uid = value.string("uid"), figureId = value.string("figureId"),
        upload = value["upload"]?.jsonObject?.let { AmiiboUpload(it.bool("active"), it.int("received"), it.int("size")) }
            ?: AmiiboUpload(),
    ).also { requireShape(value.containsKey("loaded") && value.containsKey("v3loaded") && value.containsKey("upload"), "amiibo status") }

    /**
     * Outcome of an app-initiated console wake, as reported by `wake status`.
     *
     * The `wake` command itself can only confirm DELIVERY -- the adapter latches
     * the request on one core and performs it on the other -- so treating its
     * acknowledgement as success is what previously made the app claim the
     * console had been woken when nothing happened. These are the adapter's real
     * outcomes; anything unrecognised stays [Unknown] rather than being assumed
     * successful.
     */
    enum class WakeResult { Pending, Advertised, ConsoleAwake, NoIdentity, RadioBusy, Unknown }

    data class WakeStatus(
        val result: WakeResult,
        val consoleAsleep: Boolean,
        val identityValid: Boolean,
        val attempts: Long,
    )

    fun wakeStatus(value: JsonObject): WakeStatus = WakeStatus(
        result = when (value["result"]?.jsonPrimitive?.contentOrNull) {
            "pending" -> WakeResult.Pending
            "advertised" -> WakeResult.Advertised
            "console_awake" -> WakeResult.ConsoleAwake
            "no_identity" -> WakeResult.NoIdentity
            "radio_busy" -> WakeResult.RadioBusy
            else -> WakeResult.Unknown
        },
        consoleAsleep = value["consoleAsleep"]?.jsonPrimitive?.booleanOrNull ?: false,
        identityValid = value["identityValid"]?.jsonPrimitive?.booleanOrNull ?: false,
        attempts = value["attempts"]?.jsonPrimitive?.longOrNull ?: 0L,
    )

    fun managementEnabled(value: JsonObject) = value["enabled"]?.jsonPrimitive?.booleanOrNull

    fun inputSources(value: JsonObject): AdapterInputState {
        fun requiredLong(key: String): Long = value[key]?.jsonPrimitive?.longOrNull
            ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
        fun requiredBoolean(key: String): Boolean = value[key]?.jsonPrimitive?.booleanOrNull
            ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
        val sources = value["sources"]?.jsonArray?.map { element ->
            val source = element.jsonObject
            val id = source["id"]?.jsonPrimitive?.longOrNull
                ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
            val connection = source["conn"]?.jsonPrimitive?.intOrNull
                ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
            val transport = source["transport"]?.jsonPrimitive?.intOrNull
                ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
            val generation = source["generation"]?.jsonPrimitive?.longOrNull
                ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
            val name = source["name"]?.jsonPrimitive?.contentOrNull
                ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
            requireShape(id in 1..0xFFFF_FFFFL && connection in 0..255 &&
                transport in 0..255 && generation in 0..0xFFFF_FFFFL, "input sources")
            AdapterInputSource(id, connection, transport, generation, name.ifBlank { "Controller" })
        } ?: throw ManagementException("Adapter returned an incomplete response for 'input sources'")
        val active = requiredLong("active")
        val pending = requiredLong("pending")
        val transitions = requiredLong("transitions")
        requireShape(active in 0..0xFFFF_FFFFL && pending in 0..0xFFFF_FFFFL &&
            transitions in 0..0xFFFF_FFFFL && sources.map { it.id }.distinct().size == sources.size,
            "input sources")
        return AdapterInputState(
            activeId = active,
            pendingId = pending,
            explicit = requiredBoolean("explicit"),
            awaitingFresh = requiredBoolean("fresh"),
            transitions = transitions,
            sources = sources,
            truncated = requiredBoolean("more"),
        )
    }

    /** Parse one complete or cursor-paginated v2 bond page. */
    fun bondsPage(value: JsonObject): BondPage {
        requireShape(value["v"]?.jsonPrimitive?.intOrNull == BONDS_PROTOCOL_VERSION, "bonds list v2")
        val total = value["total"]?.jsonPrimitive?.intOrNull
            ?: throw ManagementException("Adapter returned an incomplete response for 'bonds list v2'")
        requireShape(total >= 0, "bonds list v2")
        val array = value["bonds"]?.jsonArray
            ?: throw ManagementException("Adapter returned an incomplete response for 'bonds list v2'")
        val nextElement = value["next"]
            ?: throw ManagementException("Adapter returned an incomplete response for 'bonds list v2'")
        val next: Int? = when (nextElement) {
            JsonNull -> null
            is JsonPrimitive -> {
                if (nextElement.isString) {
                    throw ManagementException("Adapter returned an incomplete response for 'bonds list v2'")
                }
                nextElement.intOrNull
                    ?: throw ManagementException("Adapter returned an incomplete response for 'bonds list v2'")
            }
            else -> throw ManagementException("Adapter returned an incomplete response for 'bonds list v2'")
        }
        requireShape(next == null || next >= 0, "bonds list v2")
        val entries = array.mapIndexed { position, element ->
            val item = element.jsonObject
            BondInfo(
                index = item["i"]?.jsonPrimitive?.intOrNull
                    ?: item["index"]?.jsonPrimitive?.intOrNull ?: position,
                address = item["addr"]?.jsonPrimitive?.content
                    ?: item["address"]?.jsonPrimitive?.content.orEmpty(),
                name = item["name"]?.jsonPrimitive?.contentOrNull,
                type = item["type"]?.jsonPrimitive?.intOrNull,
            )
        }
        requireShape(entries.size <= total, "bonds list v2")
        // A cursor-bearing page must make progress.  The terminal page is
        // only one segment of the aggregate, so it need not contain `total`.
        requireShape(next == null || entries.isNotEmpty(), "bonds list v2")
        return BondPage(entries, total, next)
    }

    fun readData(value: JsonObject): ByteArray {
        val hex = value.string("data")
        if (hex.length % 2 != 0) throw ManagementException("Adapter returned odd-length Amiibo data")
        return try {
            ByteArray(hex.length / 2) { index -> hex.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
        } catch (error: NumberFormatException) {
            throw ManagementException("Adapter returned non-hex Amiibo data", error)
        }
    }

    fun requireOk(command: String, value: JsonObject): JsonObject {
        if (value["ok"]?.jsonPrimitive?.booleanOrNull != true) {
            throw ManagementException("Adapter returned an unexpected response for '$command'")
        }
        return value
    }

    fun hex(data: ByteArray) = data.joinToString("") { "%02X".format(it.toInt() and 0xFF) }

    private fun JsonObject.string(key: String, fallback: String = "") = this[key]?.jsonPrimitive?.contentOrNull ?: fallback
    private fun JsonObject.int(key: String) = this[key]?.jsonPrimitive?.intOrNull ?: 0
    private fun JsonObject.long(key: String) = this[key]?.jsonPrimitive?.longOrNull ?: 0L
    private fun JsonObject.bool(key: String) = this[key]?.jsonPrimitive?.booleanOrNull ?: false
    private fun JsonObject.boolInt(key: String): Boolean {
        val p = this[key]?.jsonPrimitive ?: return false
        return p.booleanOrNull ?: ((p.intOrNull ?: 0) != 0)
    }

    private fun requireShape(valid: Boolean, command: String) {
        if (!valid) throw ManagementException("Adapter returned an incomplete response for '$command'")
    }
}
