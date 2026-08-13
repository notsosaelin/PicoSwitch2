package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.*
import kotlinx.serialization.json.*

/** Exact newline-JSON management contract shared with config.c and the Web Portal. */
object ManagementProtocol {
    const val SERVICE_UUID = "7c5ad4ed-2731-417c-b316-058505c7c083"
    const val RX_UUID = "5252186a-817f-489f-ad75-94c3bd444769"
    const val TX_UUID = "81462706-8e64-407a-bc3d-d303529fbe1c"
    const val MAX_COMMAND_BYTES = 127
    const val MAX_REPLY_BYTES = 512
    const val MIN_GATT_PAYLOAD = 20
    const val AMIIBO_CHUNK_BYTES = 32

    private val json = Json { ignoreUnknownKeys = true }

    fun frame(command: String): ByteArray {
        require(command.isNotBlank()) { "Command cannot be blank" }
        require(!command.contains('\n') && !command.contains('\r')) { "Command must be one line" }
        val bytes = "$command\n".encodeToByteArray()
        require(bytes.size <= MAX_COMMAND_BYTES) { "Command exceeds $MAX_COMMAND_BYTES bytes" }
        return bytes
    }

    fun chunks(command: String, payloadBytes: Int): List<ByteArray> {
        require(payloadBytes >= MIN_GATT_PAYLOAD)
        return frame(command).toList().chunked(payloadBytes).map { it.toByteArray() }
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
    )

    fun controller(value: JsonObject) = ControllerInfo(
        name = value.string("name", "No controller"),
        vid = value.int("vid"), pid = value.int("pid"),
        batteryValid = value.boolInt("batteryValid"),
        batteryPercent = value.int("battery"), charging = value.boolInt("charging"),
    )

    fun personality(value: JsonObject) = PersonalityState(
        current = Personality.fromWire(value.string("current")),
        available = value["available"]?.jsonArray?.map { Personality.fromWire(it.jsonPrimitive.content) }
            ?: emptyList(),
    )

    fun config(value: JsonObject): AdapterConfig {
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
    )

    fun managementEnabled(value: JsonObject) = value["enabled"]?.jsonPrimitive?.booleanOrNull

    fun readData(value: JsonObject): ByteArray {
        val hex = value.string("data")
        require(hex.length % 2 == 0) { "Adapter returned odd-length hex data" }
        return ByteArray(hex.length / 2) { index -> hex.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
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
}
