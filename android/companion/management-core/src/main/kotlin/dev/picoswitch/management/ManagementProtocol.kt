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
import kotlinx.serialization.json.longOrNull

/** Logical newline/JSON contract implemented by firmware `src/config.c`. */
object ManagementProtocol {
    const val MAX_COMMAND_BYTES = 127
    const val AMIIBO_CHUNK_BYTES = 32
    const val BONDS_PROTOCOL_VERSION = 2
    private const val UINT32_MAX = 0xFFFF_FFFFL

    private val json = Json { ignoreUnknownKeys = true }

    fun frame(command: String): ByteArray {
        require(command.isNotBlank()) { "Command cannot be blank" }
        require(!command.contains('\n') && !command.contains('\r')) { "Command must be one line" }
        val commandBytes = command.encodeToByteArray()
        require(commandBytes.size <= MAX_COMMAND_BYTES) { "Command exceeds $MAX_COMMAND_BYTES bytes" }
        return commandBytes + '\n'.code.toByte()
    }

    fun firmware(command: String, response: String): FirmwareInfo = decode(command, response) { value ->
        FirmwareInfo(
            id = value.string("id"),
            product = value.string("product"),
            version = value.string("version"),
            bridgeContract = value.int("bridge_contract"),
            build = value.string("build"),
        ).also { requireShape(it.id.isNotBlank() && it.version.isNotBlank(), command) }
    }

    fun controller(command: String, response: String): ControllerInfo = decode(command, response) { value ->
        ControllerInfo(
            name = value.string("name", "No controller").ifBlank { "No controller" },
            vid = value.int("vid"),
            pid = value.int("pid"),
            batteryValid = value.boolInt("batteryValid"),
            batteryPercent = value.int("battery"),
            charging = value.boolInt("charging"),
        )
    }

    fun personality(command: String, response: String): PersonalityState = decode(command, response) { value ->
        PersonalityState(
            current = Personality.fromWire(value.string("current")),
            available = value["available"]?.jsonArray?.map { element ->
                val primitive = element as? JsonPrimitive
                Personality.fromWire(
                    primitive?.takeIf { it.isString }?.contentOrNull
                        ?: throw IllegalArgumentException("'available' entries must be strings"),
                )
            }
                ?: emptyList(),
        ).also {
            requireShape(it.current != Personality.Unknown && it.available.isNotEmpty(), command)
        }
    }

    fun config(command: String, response: String): AdapterConfig = decode(command, response) { value ->
        requireShape(
            value.containsKey("body_color") && value.containsKey("joycon2_left_accent") &&
                value.containsKey("joycon2_right_accent"),
            command,
        )

        fun color(key: String): RgbColor {
            val array = value[key] as? JsonArray
                ?: throw IllegalArgumentException("'$key' must be an RGB array")
            require(array.size == 3) { "'$key' must contain three RGB components" }
            val components = array.map { element ->
                (element as? JsonPrimitive)?.takeUnless { it.isString }?.intOrNull
                    ?: throw IllegalArgumentException("'$key' components must be integers")
            }
            return RgbColor(components[0], components[1], components[2])
        }

        AdapterConfig(color("body_color"), color("joycon2_left_accent"), color("joycon2_right_accent"))
    }

    fun amiibo(command: String, response: String): AmiiboStatus = decode(command, response) { value ->
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

    fun wakeStatus(command: String, response: String): WakeStatus = decode(command, response) { value ->
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
            lastAttemptMs = value.long("lastAttemptMs"),
        )
    }

    fun persistenceStatus(command: String, response: String): PersistenceStatus = decode(command, response) { value ->
        requireShape(
            value.containsKey("pending") && value.containsKey("requested") && value.containsKey("completed"),
            command,
        )
        val requested = value.long("requested")
        val completed = value.long("completed")
        val pending = value.bool("pending")
        requireShape(
            requested in 0..UINT32_MAX && completed in 0..UINT32_MAX && pending == (requested != completed),
            command,
        )
        PersistenceStatus(pending, requested, completed)
    }

    fun managementEnabled(command: String, response: String): Boolean? = decode(command, response) { value ->
        value.primitive("enabled")?.let { primitive ->
            primitive.takeUnless { it.isString }?.booleanOrNull
                ?: throw IllegalArgumentException("'enabled' must be a boolean")
        }
    }

    fun kbmStatus(command: String, response: String): KbmStatus = decode(command, response) { value ->
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

    fun kbmMapPage(command: String, response: String): KbmMapPage = decode(command, response) { value ->
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

    fun kbmMouse(command: String, response: String): KbmMouseConfig = decode(command, response) { value ->
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

    fun inputSources(command: String, response: String): AdapterInputState = decode(command, response) { value ->
        val sources = value["sources"]?.jsonArray?.map { element ->
            val source = element.jsonObject
            val id = source.requiredLong("id")
            val connection = source.requiredInt("conn")
            val transport = source.requiredInt("transport")
            val generation = source.requiredLong("generation")
            val name = source.string("name")
            requireShape(
                id in 1..0xFFFF_FFFFL && connection in 0..255 && transport in 0..255 &&
                    generation in 0..0xFFFF_FFFFL,
                command,
            )
            AdapterInputSource(id, connection, transport, generation, name.ifBlank { "Controller" })
        } ?: throw incomplete(command)
        val active = value.requiredLong("active")
        val pending = value.requiredLong("pending")
        val transitions = value.requiredLong("transitions")
        requireShape(
            active in 0..0xFFFF_FFFFL && pending in 0..0xFFFF_FFFFL &&
                transitions in 0..0xFFFF_FFFFL && sources.map { it.id }.distinct().size == sources.size,
            command,
        )
        AdapterInputState(
            activeId = active,
            pendingId = pending,
            explicit = value.requiredBoolean("explicit"),
            awaitingFresh = value.requiredBoolean("fresh"),
            transitions = transitions,
            sources = sources,
            truncated = value.requiredBoolean("more"),
        )
    }

    fun bondsPage(command: String, response: String): BondPage = decode(command, response) { value ->
        requireShape(value.requiredInt("v") == BONDS_PROTOCOL_VERSION, command)
        val total = value.requiredInt("total")
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
                index = item.optionalInt("i") ?: item.optionalInt("index") ?: position,
                address = when {
                    item.containsKey("addr") -> item.string("addr")
                    item.containsKey("address") -> item.string("address")
                    else -> ""
                },
                name = item.optionalString("name"),
                type = item.optionalInt("type"),
            )
        }
        requireShape(entries.size <= total && (next == null || entries.isNotEmpty()), command)
        BondPage(entries, total, next)
    }

    fun legacyBonds(command: String, response: String): BondEnumeration = decode(command, response) { value ->
        val array = value["bonds"]?.jsonArray ?: throw incomplete(command)
        BondEnumeration(
            entries = array.mapIndexed { position, element ->
                val item = element.jsonObject
                BondInfo(
                    index = item.optionalInt("i") ?: item.optionalInt("index") ?: position,
                    address = when {
                        item.containsKey("addr") -> item.string("addr")
                        item.containsKey("address") -> item.string("address")
                        else -> ""
                    },
                    name = item.optionalString("name"),
                    type = item.optionalInt("type"),
                )
            },
            complete = false,
            total = null,
        )
    }

    fun isVersionedBondResponse(command: String, response: String): Boolean = decode(command, response) { value ->
        value.primitive("v")?.let { primitive ->
            primitive.takeUnless { it.isString }?.intOrNull
                ?: throw IllegalArgumentException("'v' must be an integer")
        } == BONDS_PROTOCOL_VERSION
    }

    fun readData(command: String, response: String): ByteArray = decode(command, response) { value ->
        val hex = value.string("data")
        if (hex.length % 2 != 0) throw ManagementProtocolException("Adapter returned odd-length Amiibo data")
        try {
            ByteArray(hex.length / 2) { index -> hex.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
        } catch (error: NumberFormatException) {
            throw ManagementProtocolException("Adapter returned non-hex Amiibo data", error)
        }
    }

    fun acknowledgement(command: String, response: String): CommandAcknowledgement =
        decode(command, response) { value ->
            if (!value.bool("ok")) {
                throw ManagementProtocolException("Adapter returned an unexpected response for '$command'")
            }
            val requested = value.optionalLong("requested")
            requireShape(requested == null || requested in 0..UINT32_MAX, command)
            CommandAcknowledgement(
                queued = value.bool("queued"),
                switching = value.bool("switching"),
                unchanged = value.bool("unchanged"),
                reenumerating = value.bool("reenumerating"),
                enabled = value.optionalBoolean("enabled"),
                requested = requested,
            )
        }

    private inline fun <T> decode(
        command: String,
        response: String,
        block: (JsonObject) -> T,
    ): T {
        val root = objectOrThrow(command, response)
        return try {
            block(root)
        } catch (error: ManagementException) {
            throw error
        } catch (error: Exception) {
            throw ManagementProtocolException(
                "Adapter returned an incomplete response for '$command'",
                error,
            )
        }
    }

    private fun objectOrThrow(command: String, response: String): JsonObject {
        val root = try {
            json.parseToJsonElement(response.trim()).jsonObject
        } catch (error: Exception) {
            throw ManagementProtocolException("Adapter returned malformed JSON for '$command'", error)
        }
        root["error"]?.let { errorElement ->
            val message = (errorElement as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull
                ?: throw ManagementProtocolException("Adapter returned a malformed error for '$command'")
            val code = root["code"]?.let { codeElement ->
                (codeElement as? JsonPrimitive)?.takeUnless { it.isString }?.intOrNull
                    ?: throw ManagementProtocolException("Adapter returned a malformed error code for '$command'")
            }
            throw AdapterCommandException(command, code, message)
        }
        return root
    }

    private fun JsonObject.primitive(key: String): JsonPrimitive? {
        val element = this[key] ?: return null
        return element as? JsonPrimitive
            ?: throw IllegalArgumentException("'$key' must be a JSON primitive")
    }

    private fun JsonObject.string(key: String, fallback: String = ""): String {
        val primitive = primitive(key) ?: return fallback
        return primitive.takeIf { it.isString }?.contentOrNull
            ?: throw IllegalArgumentException("'$key' must be a string")
    }
    private fun JsonObject.int(key: String): Int {
        val primitive = primitive(key) ?: return 0
        return primitive.takeUnless { it.isString }?.intOrNull
            ?: throw IllegalArgumentException("'$key' must be an integer")
    }
    private fun JsonObject.long(key: String): Long {
        val primitive = primitive(key) ?: return 0L
        return primitive.takeUnless { it.isString }?.longOrNull
            ?: throw IllegalArgumentException("'$key' must be an integer")
    }
    private fun JsonObject.bool(key: String): Boolean {
        val primitive = primitive(key) ?: return false
        return primitive.takeUnless { it.isString }?.booleanOrNull
            ?: throw IllegalArgumentException("'$key' must be a boolean")
    }
    private fun JsonObject.boolInt(key: String): Boolean {
        val primitive = primitive(key) ?: return false
        if (primitive.isString) throw IllegalArgumentException("'$key' must be a boolean or integer")
        return primitive.booleanOrNull
            ?: primitive.intOrNull?.let { it != 0 }
            ?: throw IllegalArgumentException("'$key' must be a boolean or integer")
    }

    private fun JsonObject.requiredInt(key: String): Int {
        require(containsKey(key)) { "Missing '$key'" }
        return int(key)
    }

    private fun JsonObject.requiredLong(key: String): Long {
        require(containsKey(key)) { "Missing '$key'" }
        return long(key)
    }

    private fun JsonObject.requiredBoolean(key: String): Boolean {
        require(containsKey(key)) { "Missing '$key'" }
        return bool(key)
    }

    private fun JsonObject.optionalInt(key: String): Int? {
        val element = this[key] ?: return null
        if (element === JsonNull) return null
        return (element as? JsonPrimitive)?.takeUnless { it.isString }?.intOrNull
            ?: throw IllegalArgumentException("'$key' must be an integer or null")
    }

    private fun JsonObject.optionalLong(key: String): Long? {
        val element = this[key] ?: return null
        if (element === JsonNull) return null
        return (element as? JsonPrimitive)?.takeUnless { it.isString }?.longOrNull
            ?: throw IllegalArgumentException("'$key' must be an integer or null")
    }

    private fun JsonObject.optionalString(key: String): String? {
        val element = this[key] ?: return null
        if (element === JsonNull) return null
        return (element as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull
            ?: throw IllegalArgumentException("'$key' must be a string or null")
    }

    private fun JsonObject.optionalBoolean(key: String): Boolean? {
        val element = this[key] ?: return null
        if (element === JsonNull) return null
        return (element as? JsonPrimitive)?.takeUnless { it.isString }?.booleanOrNull
            ?: throw IllegalArgumentException("'$key' must be a boolean or null")
    }

    private fun requireShape(valid: Boolean, command: String) {
        if (!valid) throw incomplete(command)
    }

    private fun incomplete(command: String) =
        ManagementProtocolException("Adapter returned an incomplete response for '$command'")
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
    const val SAVE_STATUS = "save status"
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
    fun bondsPage(cursor: Int? = null): String {
        require(cursor == null || cursor >= 0) { "Bond cursor cannot be negative" }
        return "bonds list v2" + (cursor?.let { " $it" } ?: "")
    }
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
    fun amiiboBegin(size: Int, crc32: String): String {
        require(size in setOf(540, 572, 2048)) { "Unsupported Amiibo image size" }
        require(crc32.matches(Regex("[0-9A-Fa-f]{8}"))) { "CRC32 must contain exactly eight hex digits" }
        return "amiibo begin $size ${crc32.uppercase()}"
    }
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
