package dev.picoswitch.management

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull

/** Logical newline/JSON contract implemented by firmware `src/config.c`. */
object ManagementProtocol {
    const val MAX_COMMAND_BYTES = 127
    const val MAX_REPLY_PAYLOAD_BYTES = 511
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
        require(payloadBytes > 0) { "Payload size must be positive" }
        return frame(command).toList().chunked(payloadBytes).map { it.toByteArray() }
    }

    fun requireReplyWithinLimit(payloadBytes: Int) {
        if (payloadBytes > MAX_REPLY_PAYLOAD_BYTES) {
            throw ManagementReplyTooLargeException(
                "Adapter reply exceeded the $MAX_REPLY_PAYLOAD_BYTES-byte wireless payload limit",
            )
        }
    }

    fun firmware(command: String, response: String): FirmwareInfo = objectOrThrow(command, response).let { value ->
        FirmwareInfo(
            id = value.string("id"),
            product = value.string("product"),
            version = value.string("version"),
            bridgeContract = value.int("bridge_contract"),
            build = value.string("build"),
        ).also { requireShape(it.id.isNotBlank() && it.version.isNotBlank(), command) }
    }

    fun controller(command: String, response: String): ControllerInfo = objectOrThrow(command, response).let { value ->
        ControllerInfo(
            name = value.string("name", "No controller").ifBlank { "No controller" },
            vid = value.int("vid"),
            pid = value.int("pid"),
            batteryValid = value.boolInt("batteryValid"),
            batteryPercent = value.int("battery"),
            charging = value.boolInt("charging"),
        )
    }

    fun personality(command: String, response: String): PersonalityState = objectOrThrow(command, response).let { value ->
        PersonalityState(
            current = Personality.fromWire(value.string("current")),
            available = value["available"]?.jsonArray?.map { Personality.fromWire(it.jsonPrimitive.content) }
                ?: emptyList(),
        ).also {
            requireShape(it.current != Personality.Unknown && it.available.isNotEmpty(), command)
        }
    }

    fun config(command: String, response: String): AdapterConfig = objectOrThrow(command, response).let { value ->
        requireShape(
            value.containsKey("body_color") && value.containsKey("joycon2_left_accent") &&
                value.containsKey("joycon2_right_accent"),
            command,
        )

        fun color(key: String): RgbColor {
            val array = value[key]?.jsonArray ?: return RgbColor(0, 0, 0)
            return RgbColor(
                array.getOrNull(0)?.jsonPrimitive?.intOrNull ?: 0,
                array.getOrNull(1)?.jsonPrimitive?.intOrNull ?: 0,
                array.getOrNull(2)?.jsonPrimitive?.intOrNull ?: 0,
            )
        }

        AdapterConfig(color("body_color"), color("joycon2_left_accent"), color("joycon2_right_accent"))
    }

    fun amiibo(command: String, response: String): AmiiboStatus = objectOrThrow(command, response).let { value ->
        requireShape(
            value.containsKey("loaded") && value.containsKey("v3loaded") && value.containsKey("upload"),
            command,
        )
        AmiiboStatus(
            loaded = value.bool("loaded"),
            dirty = value.bool("dirty"),
            presented = value.bool("presented"),
            v3Loaded = value.bool("v3loaded"),
            persisted = value.bool("persisted"),
            persistPending = value.bool("persistPending"),
            size = value.int("size"),
            signature = value.bool("signature"),
            hasSave2 = value.bool("hasSave2"),
            usingSave2 = value.bool("usingSave2"),
            generation = value.long("generation"),
            payloadCrc = value.string("payloadCrc", "00000000"),
            uid = value.string("uid"),
            figureId = value.string("figureId"),
            upload = value["upload"]?.jsonObject?.let {
                AmiiboUpload(it.bool("active"), it.int("received"), it.int("size"))
            } ?: AmiiboUpload(),
        )
    }

    fun wakeStatus(command: String, response: String): WakeStatus = objectOrThrow(command, response).let { value ->
        WakeStatus(
            result = when (value.string("result")) {
                "pending" -> WakeResult.Pending
                "advertised" -> WakeResult.Advertised
                "console_awake" -> WakeResult.ConsoleAwake
                "no_identity" -> WakeResult.NoIdentity
                "radio_busy" -> WakeResult.RadioBusy
                else -> WakeResult.Unknown
            },
            consoleAsleep = value.bool("consoleAsleep"),
            identityValid = value.bool("identityValid"),
            attempts = value.long("attempts"),
        )
    }

    fun managementEnabled(command: String, response: String): Boolean? =
        objectOrThrow(command, response)["enabled"]?.jsonPrimitive?.booleanOrNull

    fun kbmStatus(command: String, response: String): KbmStatus = objectOrThrow(command, response).let { value ->
        val mode = KbmMode.fromWire(value.string("mode"))
        val override = KbmMode.fromWire(value.string("override"))
        val profile = KbmProfile.fromWire(value.string("profile"))
        requireShape(
            mode != null && override != null && profile != null && value.containsKey("keyboard") &&
                value.containsKey("mouse") && value.containsKey("nativeMouse"),
            command,
        )
        KbmStatus(
            mode = mode!!,
            modeOverride = override!!,
            profile = profile!!,
            keyboardConnected = value.bool("keyboard"),
            mouseConnected = value.bool("mouse"),
            nativeMouseOutput = value.bool("nativeMouse"),
            keyboardConn = value.int("keyboardConn"),
            mouseConn = value.int("mouseConn"),
            keyboardReports = value.long("keyboardReports"),
            mouseReports = value.long("mouseReports"),
            rejectedMode = value.long("rejectedMode"),
            rejectedDuplicate = value.long("rejectedDuplicate"),
            rejectedNotOwner = value.long("rejectedNotOwner"),
            rollover = value.long("rollover"),
            roleLosses = value.long("roleLosses"),
            mapGeneration = value.long("mapGeneration"),
            publishes = value.long("publishes"),
            recenters = value.long("recenters"),
        )
    }

    fun kbmMapPage(command: String, response: String): KbmMapPage = objectOrThrow(command, response).let { value ->
        val profile = KbmProfile.fromWire(value.string("profile"))
        val entries = value["bindings"] as? JsonArray
        requireShape(profile != null && entries != null && value.containsKey("total") && value.containsKey("more"), command)
        val page = value.int("page")
        val pageSize = value.int("pageSize")
        val total = value.int("total")
        requireShape(page >= 0 && pageSize > 0 && total >= 0, command)
        val bindings = entries!!.map { element ->
            val item = element.jsonObject
            val source = KbmSource.parse(item.string("src"))
            val destination = KbmDestination.fromWire(item.string("dst"))
            requireShape(source != null && destination != null, command)
            KbmBinding(source!!, destination!!, item.bool("custom"))
        }
        requireShape(bindings.size <= pageSize && bindings.size <= total, command)
        KbmMapPage(profile!!, page, pageSize, total, bindings, value.bool("more"))
    }

    fun kbmMouse(command: String, response: String): KbmMouseConfig = objectOrThrow(command, response).let { value ->
        requireShape(
            value.containsKey("sensitivityX") && value.containsKey("sensitivityY") &&
                value.containsKey("sensitivityMin") && value.containsKey("sensitivityMax") &&
                value.containsKey("antiDeadzoneMax"),
            command,
        )
        KbmMouseConfig(
            sensitivityX = value.int("sensitivityX"),
            sensitivityY = value.int("sensitivityY"),
            velocityWindowMs = value.int("recenterMs"),
            invertX = value.bool("invertX"),
            invertY = value.bool("invertY"),
            antiDeadzone = value.int("antiDeadzone"),
            sensitivityMin = value.int("sensitivityMin"),
            sensitivityMax = value.int("sensitivityMax"),
            velocityWindowMinMs = value.int("recenterMinMs"),
            velocityWindowMaxMs = value.int("recenterMaxMs"),
            antiDeadzoneMax = value.int("antiDeadzoneMax"),
        ).also {
            requireShape(
                it.sensitivityMax > it.sensitivityMin &&
                    it.velocityWindowMaxMs >= it.velocityWindowMinMs && it.antiDeadzoneMax >= 0,
                command,
            )
        }
    }

    fun inputSources(command: String, response: String): AdapterInputState = objectOrThrow(command, response).let { value ->
        fun requiredLong(key: String): Long = value[key]?.jsonPrimitive?.longOrNull
            ?: throw incomplete(command)
        fun requiredBoolean(key: String): Boolean = value[key]?.jsonPrimitive?.booleanOrNull
            ?: throw incomplete(command)
        val sources = value["sources"]?.jsonArray?.map { element ->
            val source = element.jsonObject
            val id = source["id"]?.jsonPrimitive?.longOrNull ?: throw incomplete(command)
            val connection = source["conn"]?.jsonPrimitive?.intOrNull ?: throw incomplete(command)
            val transport = source["transport"]?.jsonPrimitive?.intOrNull ?: throw incomplete(command)
            val generation = source["generation"]?.jsonPrimitive?.longOrNull ?: throw incomplete(command)
            val name = source["name"]?.jsonPrimitive?.contentOrNull ?: throw incomplete(command)
            requireShape(
                id in 1..0xFFFF_FFFFL && connection in 0..255 && transport in 0..255 &&
                    generation in 0..0xFFFF_FFFFL,
                command,
            )
            AdapterInputSource(id, connection, transport, generation, name.ifBlank { "Controller" })
        } ?: throw incomplete(command)
        val active = requiredLong("active")
        val pending = requiredLong("pending")
        val transitions = requiredLong("transitions")
        requireShape(
            active in 0..0xFFFF_FFFFL && pending in 0..0xFFFF_FFFFL &&
                transitions in 0..0xFFFF_FFFFL && sources.map { it.id }.distinct().size == sources.size,
            command,
        )
        AdapterInputState(
            activeId = active,
            pendingId = pending,
            explicit = requiredBoolean("explicit"),
            awaitingFresh = requiredBoolean("fresh"),
            transitions = transitions,
            sources = sources,
            truncated = requiredBoolean("more"),
        )
    }

    fun bondsPage(command: String, response: String): BondPage = objectOrThrow(command, response).let { value ->
        requireShape(value["v"]?.jsonPrimitive?.intOrNull == BONDS_PROTOCOL_VERSION, command)
        val total = value["total"]?.jsonPrimitive?.intOrNull ?: throw incomplete(command)
        val array = value["bonds"]?.jsonArray ?: throw incomplete(command)
        val nextElement = value["next"] ?: throw incomplete(command)
        val next: Int? = when (nextElement) {
            JsonNull -> null
            is JsonPrimitive -> nextElement.takeUnless { it.isString }?.intOrNull ?: throw incomplete(command)
            else -> throw incomplete(command)
        }
        requireShape(total >= 0 && (next == null || next >= 0), command)
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
        requireShape(entries.size <= total && (next == null || entries.isNotEmpty()), command)
        BondPage(entries, total, next)
    }

    fun legacyBonds(command: String, response: String): BondEnumeration = objectOrThrow(command, response).let { value ->
        val array = value["bonds"]?.jsonArray ?: throw incomplete(command)
        BondEnumeration(
            entries = array.mapIndexed { position, element ->
                val item = element.jsonObject
                BondInfo(
                    index = item["i"]?.jsonPrimitive?.intOrNull
                        ?: item["index"]?.jsonPrimitive?.intOrNull ?: position,
                    address = item["addr"]?.jsonPrimitive?.content
                        ?: item["address"]?.jsonPrimitive?.content.orEmpty(),
                    name = item["name"]?.jsonPrimitive?.contentOrNull,
                    type = item["type"]?.jsonPrimitive?.intOrNull,
                )
            },
            complete = false,
            total = null,
        )
    }

    fun isVersionedBondResponse(command: String, response: String): Boolean =
        objectOrThrow(command, response)["v"]?.jsonPrimitive?.intOrNull == BONDS_PROTOCOL_VERSION

    fun readData(command: String, response: String): ByteArray {
        val hex = objectOrThrow(command, response).string("data")
        if (hex.length % 2 != 0) throw ManagementProtocolException("Adapter returned odd-length Amiibo data")
        return try {
            ByteArray(hex.length / 2) { index -> hex.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
        } catch (error: NumberFormatException) {
            throw ManagementProtocolException("Adapter returned non-hex Amiibo data", error)
        }
    }

    fun acknowledgement(command: String, response: String): CommandAcknowledgement =
        objectOrThrow(command, response).let { value ->
            if (value["ok"]?.jsonPrimitive?.booleanOrNull != true) {
                throw ManagementProtocolException("Adapter returned an unexpected response for '$command'")
            }
            CommandAcknowledgement(
                queued = value.bool("queued"),
                switching = value.bool("switching"),
                unchanged = value.bool("unchanged"),
                reenumerating = value.bool("reenumerating"),
                enabled = value["enabled"]?.jsonPrimitive?.booleanOrNull,
            )
        }

    private fun objectOrThrow(command: String, response: String): JsonObject {
        val root = try {
            json.parseToJsonElement(response.trim()).jsonObject
        } catch (error: Exception) {
            throw ManagementProtocolException("Adapter returned malformed JSON for '$command'", error)
        }
        root["error"]?.jsonPrimitive?.contentOrNull?.let { message ->
            throw AdapterCommandException(command, root["code"]?.jsonPrimitive?.intOrNull, message)
        }
        return root
    }

    private fun JsonObject.string(key: String, fallback: String = "") =
        this[key]?.jsonPrimitive?.contentOrNull ?: fallback
    private fun JsonObject.int(key: String) = this[key]?.jsonPrimitive?.intOrNull ?: 0
    private fun JsonObject.long(key: String) = this[key]?.jsonPrimitive?.longOrNull ?: 0L
    private fun JsonObject.bool(key: String) = this[key]?.jsonPrimitive?.booleanOrNull ?: false
    private fun JsonObject.boolInt(key: String): Boolean {
        val primitive = this[key]?.jsonPrimitive ?: return false
        return primitive.booleanOrNull ?: ((primitive.intOrNull ?: 0) != 0)
    }

    private fun requireShape(valid: Boolean, command: String) {
        if (!valid) throw incomplete(command)
    }

    private fun incomplete(command: String) =
        ManagementProtocolException("Adapter returned an incomplete response for '$command'")
}

/** BLE carrier constants. These are transport contract, not logical domain semantics. */
object BleManagementContract {
    const val SERVICE_UUID = "7c5ad4ed-2731-417c-b316-058505c7c083"
    const val RX_UUID = "5252186a-817f-489f-ad75-94c3bd444769"
    const val TX_UUID = "81462706-8e64-407a-bc3d-d303529fbe1c"
    const val ATT_PAYLOAD_WITH_DEFAULT_MTU = 20
}

/** Exact command vocabulary and argument encoding accepted by production management. */
object ManagementCommands {
    const val INFO = "info"
    const val PING = "ping"
    const val GET_CONFIG = "get"
    const val DEVICE = "device"
    const val INPUT_SOURCES = "input sources"
    const val PERSONALITY = "personality"
    const val REENUMERATE = "reenumerate"
    const val WAKE = "wake"
    const val WAKE_STATUS = "wake status"
    const val MANAGEMENT_STATUS = "mgmt status"
    const val SAVE = "save"
    const val AMIIBO_STATUS = "amiibo status"
    const val KBM_STATUS = "kbm status"
    const val KBM_MOUSE = "kbm mouse"

    fun inputActive(sourceId: Long): String {
        require(sourceId in 0..0xFFFF_FFFFL)
        return "input active ${if (sourceId == 0L) "none" else sourceId}"
    }

    fun personality(value: Personality): String {
        require(value in setOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight))
        return "personality ${value.wireName}"
    }

    fun managementEnabled(enabled: Boolean) = if (enabled) "mgmt on" else "mgmt off"
    fun bondsPage(cursor: Int? = null) = "bonds list v2" + (cursor?.let { " $it" } ?: "")
    fun bondRemove(index: Int): String {
        require(index >= 0)
        return "bonds remove $index"
    }

    fun kbmMap(profile: KbmProfile, page: Int): String {
        require(page in 0..32)
        return "kbm map ${profile.wire} $page"
    }

    fun kbmMode(mode: KbmMode) = "kbm mode ${mode.wire}"
    fun kbmBind(profile: KbmProfile, source: KbmSource, destination: KbmDestination?) =
        "kbm bind ${profile.wire} ${source.wire} ${destination?.wire ?: "default"}"
    fun kbmReset(profile: KbmProfile) = "kbm reset ${profile.wire}"
    const val KBM_RESET_ALL = "kbm reset all"
    fun kbmMouse(field: KbmMouseField, value: Int) = "kbm mouse ${field.wire} $value"

    fun color(target: ColorTarget, color: RgbColor) = "${target.command} ${color.wire()}"
    fun amiiboBegin(size: Int, crc32: String) = "amiibo begin $size $crc32"
    fun amiiboChunk(offset: Int, bytes: ByteArray): String {
        require(offset >= 0 && bytes.isNotEmpty() && bytes.size <= ManagementProtocol.AMIIBO_CHUNK_BYTES)
        return "amiibo chunk $offset ${hex(bytes)}"
    }
    fun amiiboRead(offset: Int, count: Int): String {
        require(offset >= 0 && count in 1..ManagementProtocol.AMIIBO_CHUNK_BYTES)
        return "amiibo read $offset $count"
    }
    fun amiiboCommit(useSave2: Boolean) = if (useSave2) "amiibo commit save2" else "amiibo commit"
    const val AMIIBO_CANCEL = "amiibo cancel"
    const val AMIIBO_PERSIST = "amiibo persist"
    const val AMIIBO_DOWNLOADED = "amiibo downloaded"
    fun amiiboPresented(presented: Boolean) = if (presented) "amiibo present" else "amiibo eject"
    fun amiiboSelect(used: Boolean) = if (used) "amiibo select save2" else "amiibo select save1"
    const val AMIIBO_CLEAR = "amiibo clear"

    fun hex(data: ByteArray) = data.joinToString("") { "%02X".format(it.toInt() and 0xFF) }
}
