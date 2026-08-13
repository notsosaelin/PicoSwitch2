package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.*
import dev.picoswitch.companion.protocol.*
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.util.zip.CRC32

class AdapterRepository(private val transport: ManagementTransport) {
    val connection: StateFlow<ConnectionState> = transport.connection
    private val _snapshot = MutableStateFlow(AdapterSnapshot())
    val snapshot: StateFlow<AdapterSnapshot> = _snapshot.asStateFlow()

    suspend fun connect() {
        transport.scanAndConnect()
        refreshAll()
    }

    suspend fun disconnect() = transport.disconnect()

    suspend fun refreshAll() {
        val firmware = parse("info", ManagementProtocol::firmware)
        val config = parse("get", ManagementProtocol::config)
        val controller = parse("device", ManagementProtocol::controller)
        val personality = parse("personality", ManagementProtocol::personality)
        val amiibo = parse("amiibo status", ManagementProtocol::amiibo)
        val management = runCatching { parse("mgmt status", ManagementProtocol::managementEnabled) }.getOrNull()
        val bonds = runCatching { listBonds() }.getOrDefault(emptyList())
        _snapshot.value = AdapterSnapshot(firmware, controller, personality, config, amiibo, management, bonds, System.currentTimeMillis())
    }

    suspend fun refreshAmiibo(): AmiiboStatus {
        val status = parse("amiibo status", ManagementProtocol::amiibo)
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
        return status
    }

    suspend fun refreshController(): ControllerInfo {
        val controller = parse("device", ManagementProtocol::controller)
        _snapshot.value = _snapshot.value.copy(controller = controller, refreshedAtMillis = System.currentTimeMillis())
        return controller
    }

    suspend fun setPersonality(personality: Personality): Boolean {
        require(personality in listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight))
        val value = command("personality ${personality.wireName}")
        val switching = value["switching"]?.jsonPrimitive?.content?.toBooleanStrictOrNull() == true
        _snapshot.value = _snapshot.value.copy(personality = _snapshot.value.personality.copy(current = personality))
        return switching
    }

    suspend fun setColor(target: ColorTarget, color: RgbColor, persist: Boolean = true) {
        command("${target.command} ${color.wire()}")
        if (persist) command("save")
        val old = _snapshot.value.config
        val next = when (target) {
            ColorTarget.Body -> old.copy(bodyColor = color)
            ColorTarget.LeftAccent -> old.copy(leftAccent = color)
            ColorTarget.RightAccent -> old.copy(rightAccent = color)
        }
        _snapshot.value = _snapshot.value.copy(config = next)
    }

    suspend fun wakeConsole() { command("wake") }

    suspend fun setManagementEnabled(enabled: Boolean) {
        val result = command(if (enabled) "mgmt on" else "mgmt off")
        _snapshot.value = _snapshot.value.copy(managementEnabled = ManagementProtocol.managementEnabled(result))
    }

    suspend fun listBonds(): List<BondInfo> {
        val result = command("bonds list")
        val array = result["bonds"]?.jsonArray ?: return emptyList()
        return array.mapIndexed { position, element ->
            val item = element.jsonObject
            BondInfo(
                index = item["i"]?.jsonPrimitive?.content?.toIntOrNull()
                    ?: item["index"]?.jsonPrimitive?.content?.toIntOrNull() ?: position,
                address = item["addr"]?.jsonPrimitive?.content
                    ?: item["address"]?.jsonPrimitive?.content.orEmpty(),
                name = item["name"]?.jsonPrimitive?.content,
            )
        }
    }

    suspend fun removeBond(index: Int) {
        command("bonds remove $index")
        _snapshot.value = _snapshot.value.copy(bonds = listBonds())
    }

    suspend fun uploadAmiibo(data: ByteArray, useSave2: Boolean = false, progress: (OperationProgress) -> Unit = {}) {
        AmiiboFiles.validate(data)
        val current = refreshAmiibo()
        if (current.dirty) throw ManagementException("Sync the modified Amiibo before replacing it")
        val crc = AmiiboFiles.crc32(data)
        command("amiibo begin ${data.size} $crc")
        try {
            data.asList().chunked(ManagementProtocol.AMIIBO_CHUNK_BYTES).forEachIndexed { index, values ->
                val offset = index * ManagementProtocol.AMIIBO_CHUNK_BYTES
                val chunk = values.toByteArray()
                command("amiibo chunk $offset ${ManagementProtocol.hex(chunk)}")
                progress(OperationProgress("Uploading Amiibo", (offset + chunk.size).coerceAtMost(data.size), data.size))
            }
            command(if (useSave2) "amiibo commit save2" else "amiibo commit")
            command("amiibo persist")
            awaitPersisted()
        } catch (error: Throwable) {
            runCatching { command("amiibo cancel") }
            throw error
        }
        refreshAmiibo()
    }

    suspend fun syncAmiibo(progress: (OperationProgress) -> Unit = {}): ByteArray {
        val status = refreshAmiibo()
        if ((!status.loaded && !status.v3Loaded) || status.size <= 0) throw ManagementException("No Amiibo is loaded on the adapter")
        val output = ByteArray(status.size)
        var offset = 0
        while (offset < output.size) {
            val count = minOf(ManagementProtocol.AMIIBO_CHUNK_BYTES, output.size - offset)
            val bytes = ManagementProtocol.readData(command("amiibo read $offset $count"))
            if (bytes.size != count) throw ManagementException("Adapter returned ${bytes.size} of $count requested bytes")
            bytes.copyInto(output, offset)
            offset += count
            progress(OperationProgress("Syncing Amiibo", offset, output.size))
        }
        AmiiboFiles.validate(output)
        val crc = AmiiboFiles.crc32(output)
        if (!status.payloadCrc.equals(crc, ignoreCase = true)) {
            throw ManagementException("Synced Amiibo failed CRC verification (${status.payloadCrc} != $crc)")
        }
        command("amiibo downloaded")
        command("amiibo persist")
        awaitPersisted()
        refreshAmiibo() // Required cache/state invalidation after console-mutated data is downloaded.
        return output
    }

    suspend fun setPresented(presented: Boolean) {
        command(if (presented) "amiibo present" else "amiibo eject")
        refreshAmiibo()
    }

    suspend fun selectAmiiboCopy(used: Boolean) {
        command(if (used) "amiibo select save2" else "amiibo select save1")
        refreshAmiibo()
    }

    suspend fun clearAmiibo() {
        val status = refreshAmiibo()
        if (status.dirty) throw ManagementException("Sync the modified Amiibo before clearing it")
        command("amiibo clear")
        awaitCondition { !it.loaded && !it.v3Loaded && !it.persistPending }
    }

    private suspend fun awaitPersisted(): AmiiboStatus = awaitCondition { it.persisted && !it.persistPending }

    private suspend fun awaitCondition(predicate: (AmiiboStatus) -> Boolean): AmiiboStatus {
        repeat(30) {
            val status = refreshAmiibo()
            if (predicate(status)) return status
            delay(200)
        }
        throw ManagementException("Adapter did not finish saving within 6 seconds")
    }

    private suspend fun command(command: String): JsonObject {
        val response = transport.transact(command)
        return ManagementProtocol.objectOrThrow(command, response)
    }

    private suspend fun <T> parse(command: String, parser: (JsonObject) -> T): T = parser(command(command))
}

enum class ColorTarget(val command: String) { Body("body"), LeftAccent("jcl"), RightAccent("jcr") }

object AmiiboFiles {
    val supportedSizes = setOf(540, 572, 2048)

    fun normalizeImport(raw: ByteArray): ByteArray {
        require(raw.size in supportedSizes) { "Amiibo backups must be exactly 540, 572, or 2048 bytes" }
        val data = raw.copyOf()
        if (data.size != 2048) {
            // Pure UID checksum bytes are commonly left as placeholders by dump exporters.
            data[3] = (0x88 xor data[0].u8() xor data[1].u8() xor data[2].u8()).toByte()
            data[8] = (data[4].u8() xor data[5].u8() xor data[6].u8() xor data[7].u8()).toByte()
        }
        validate(data)
        return data
    }

    fun validate(data: ByteArray) {
        require(data.size in supportedSizes) { "Amiibo backups must be 540, 572, or 2048 bytes" }
        require(data.size > 0x5B) { "Amiibo backup is missing its identity block" }
        require(data[0].u8() == 0x04) { "Amiibo UID must start with Nintendo/NXP manufacturer byte 04" }
        if (data.size == 2048) {
            require(data[7].u8() == 0x00 && data[8].u8() == 0x44) { "Figure v3 image is missing its 00/44 internal marker" }
        } else {
            val bcc0 = 0x88 xor data[0].u8() xor data[1].u8() xor data[2].u8()
            val bcc1 = data[4].u8() xor data[5].u8() xor data[6].u8() xor data[7].u8()
            require(data[3].u8() == bcc0 && data[8].u8() == bcc1) { "Amiibo UID checksum is invalid" }
        }
    }

    fun crc32(data: ByteArray): String {
        val crc = CRC32().apply { update(data) }.value
        return "%08X".format(crc)
    }

    fun uid(data: ByteArray): String {
        val offsets = if (data.size == 2048) intArrayOf(0, 1, 2, 3, 4, 5, 6) else intArrayOf(0, 1, 2, 4, 5, 6, 7)
        return offsets.joinToString("") { "%02X".format(data[it].toInt() and 0xFF) }
    }

    fun figureId(data: ByteArray) = data.copyOfRange(0x54, 0x5C).joinToString("") { "%02X".format(it.toInt() and 0xFF) }

    private fun Byte.u8() = toInt() and 0xFF
}
