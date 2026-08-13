package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.*
import dev.picoswitch.companion.protocol.*
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.intOrNull
import java.util.zip.CRC32

class AdapterRepository(private val transport: ManagementTransport) {
    val connection: StateFlow<ConnectionState> = transport.connection
    private val _snapshot = MutableStateFlow(AdapterSnapshot())
    val snapshot: StateFlow<AdapterSnapshot> = _snapshot.asStateFlow()

    fun close() = transport.close()

    suspend fun connect() {
        transport.scanAndConnect()
        validateConnectedAdapter()
    }

    suspend fun connectKnown(address: String) {
        val direct = runCatching { transport.connectKnown(address) }
        if (direct.isFailure) {
            runCatching { transport.disconnect() }
            transport.scanAndConnect()
        }
        validateConnectedAdapter()
    }

    private suspend fun validateConnectedAdapter() {
        try {
            refreshAll()
            if (_snapshot.value.firmware.id != "picoswitch") throw ManagementException("The discovered Bluetooth device is not a PicoSwitch2 adapter")
        } catch (error: Throwable) {
            runCatching { transport.disconnect() }
            throw error
        }
    }

    suspend fun disconnect() {
        transport.disconnect()
        _snapshot.value = _snapshot.value.copy(controller = ControllerInfo())
    }

    suspend fun refreshAll() {
        val firmware = parse("info", ManagementProtocol::firmware)
        val config = parse("get", ManagementProtocol::config)
        val controller = parse("device", ManagementProtocol::controller)
        val old = _snapshot.value
        val personality = optional("personality") { parse("personality", ManagementProtocol::personality) }
        val amiibo = optional("amiibo") { parse("amiibo status", ManagementProtocol::amiibo) }
        val management = optional("management gate") { parse("mgmt status", ManagementProtocol::managementEnabled) }
        val bonds = optional("bond management") { listBondsRaw() }
        val input = optional("active input") { parse("input sources", ManagementProtocol::inputSources) }
        val bondCapability = when {
            bonds.value == null -> bonds.state
            bonds.value.complete -> CapabilityState.Available
            else -> CapabilityState.Unknown
        }
        _snapshot.value = AdapterSnapshot(
            firmware = firmware,
            controller = controller,
            personality = personality.value ?: old.personality,
            config = config,
            amiibo = amiibo.value ?: old.amiibo,
            managementEnabled = management.value ?: old.managementEnabled,
            bonds = bonds.value?.entries ?: emptyList(),
            bondsComplete = bonds.value?.complete,
            bondsTotal = bonds.value?.total,
            input = input.value ?: old.input,
            capabilities = AdapterCapabilities(
                core = CapabilityState.Available,
                personality = personality.state,
                colors = CapabilityState.Available,
                amiibo = amiibo.state,
                managementGate = management.state,
                bonds = bondCapability,
                wake = old.capabilities.wake,
                activeInput = input.state,
            ),
            refreshedAtMillis = System.currentTimeMillis(),
        )
    }

    suspend fun refreshAmiibo(): AmiiboStatus {
        val status = parse("amiibo status", ManagementProtocol::amiibo)
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
        return status
    }

    suspend fun refreshController(): ControllerInfo {
        val controller = parse("device", ManagementProtocol::controller)
        val input = optional("active input") { parse("input sources", ManagementProtocol::inputSources) }
        _snapshot.value = _snapshot.value.copy(
            controller = controller,
            input = input.value ?: _snapshot.value.input,
            capabilities = _snapshot.value.capabilities.copy(activeInput = input.state),
            refreshedAtMillis = System.currentTimeMillis(),
        )
        return controller
    }

    suspend fun setActiveInput(sourceId: Long) {
        require(sourceId in 0..0xFFFF_FFFFL)
        val argument = if (sourceId == 0L) "none" else sourceId.toString()
        ack("input active $argument")
        val input = parse("input sources", ManagementProtocol::inputSources)
        _snapshot.value = _snapshot.value.copy(
            input = input,
            capabilities = _snapshot.value.capabilities.copy(activeInput = CapabilityState.Available),
            refreshedAtMillis = System.currentTimeMillis(),
        )
    }

    suspend fun setPersonality(personality: Personality): Boolean {
        require(personality in listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight))
        val value = ack("personality ${personality.wireName}")
        val switching = value["switching"]?.jsonPrimitive?.content?.toBooleanStrictOrNull() == true
        // A switching acknowledgement precedes USB detach/re-enumeration; retain the last
        // verified identity until a reconnect proves the requested personality is active.
        if (!switching) _snapshot.value = _snapshot.value.copy(personality = _snapshot.value.personality.copy(current = personality))
        return switching
    }

    suspend fun setColor(target: ColorTarget, color: RgbColor, persist: Boolean = true) {
        ack("${target.command} ${color.wire()}")
        if (persist) ack("save")
        val old = _snapshot.value.config
        val next = when (target) {
            ColorTarget.Body -> old.copy(bodyColor = color)
            ColorTarget.LeftAccent -> old.copy(leftAccent = color)
            ColorTarget.RightAccent -> old.copy(rightAccent = color)
        }
        _snapshot.value = _snapshot.value.copy(config = next)
    }

    suspend fun reenumerateUsb() {
        val result = ack("reenumerate")
        if (result["reenumerating"]?.jsonPrimitive?.content?.toBooleanStrictOrNull() != true) {
            throw ManagementException("Adapter did not confirm USB re-enumeration")
        }
    }

    suspend fun wakeConsole() {
        try {
            ack("wake")
            updateCapabilities { it.copy(wake = CapabilityState.Available) }
        } catch (error: AdapterCommandException) {
            if (error.isUnsupported()) updateCapabilities { it.copy(wake = CapabilityState.Unsupported) }
            throw error
        }
    }

    suspend fun setManagementEnabled(enabled: Boolean) {
        val result = ack(if (enabled) "mgmt on" else "mgmt off")
        _snapshot.value = _snapshot.value.copy(managementEnabled = ManagementProtocol.managementEnabled(result))
    }

    suspend fun listBonds(): List<BondInfo> {
        markBondsUnknown()
        val enumeration = listBondsRaw()
        applyBondEnumeration(enumeration)
        return enumeration.entries
    }

    private suspend fun listBondsRaw(): BondEnumeration {
        val first = try {
            command("bonds list")
        } catch (error: AdapterCommandException) {
            if (error.code == 413 || error.message?.contains("response_too_large", true) == true) {
                return listBondsV2()
            }
            throw error
        }
        if (first["v"]?.jsonPrimitive?.intOrNull == ManagementProtocol.BONDS_PROTOCOL_VERSION)
            return collectBondPages(first)

        // Older firmware returns only the historical `bonds` array. Preserve
        // the entries for diagnostics, but mark them incomplete so the app
        // never presents an unbounded legacy result as authoritative.
        val array = first["bonds"]?.jsonArray
            ?: throw ManagementException("Adapter returned an incomplete bond list")
        return BondEnumeration(array.mapIndexed { position, element ->
            val item = element.jsonObject
            BondInfo(
                index = item["i"]?.jsonPrimitive?.content?.toIntOrNull()
                    ?: item["index"]?.jsonPrimitive?.content?.toIntOrNull() ?: position,
                address = item["addr"]?.jsonPrimitive?.content
                    ?: item["address"]?.jsonPrimitive?.content.orEmpty(),
                name = item["name"]?.jsonPrimitive?.content,
                type = item["type"]?.jsonPrimitive?.content?.toIntOrNull(),
            )
        }, complete = false, total = null)
    }

    private suspend fun listBondsV2(): BondEnumeration =
        collectBondPages(command("bonds list v2"))

    private suspend fun collectBondPages(first: JsonObject): BondEnumeration {
        val entries = mutableListOf<BondInfo>()
        val seen = mutableSetOf<Int>()
        var page = first
        var cursor = 0
        var expectedTotal: Int? = null
        var pageCount = 0
        while (true) {
            val parsed = ManagementProtocol.bondsPage(page)
            if (expectedTotal == null) expectedTotal = parsed.total
            if (expectedTotal != parsed.total) {
                throw ManagementException("Adapter changed the bond-list total during pagination")
            }
            parsed.entries.forEach { entry ->
                if (!seen.add(entry.index)) {
                    throw ManagementException("Adapter repeated a bond during pagination")
                }
                entries += entry
            }
            val next = parsed.next ?: break
            if (next <= cursor || ++pageCount > 128) {
                throw ManagementException("Adapter returned a non-progressing bond-list cursor")
            }
            cursor = next
            page = command("bonds list v2 $next")
        }
        val total = expectedTotal ?: 0
        if (entries.size != total) {
            throw ManagementException("Adapter returned an incomplete paginated bond list")
        }
        return BondEnumeration(entries, complete = true, total = total)
    }

    suspend fun removeBond(index: Int) {
        if (_snapshot.value.bondsComplete != true) {
            throw ManagementException("Bond list completeness is unknown; refresh on a versioned firmware before removing a bond")
        }
        // A timeout after a flash mutation is ambiguous; hide the previous
        // authoritative list until a fresh complete enumeration succeeds.
        markBondsUnknown()
        ack("bonds remove $index")
        val enumeration = listBondsRaw()
        applyBondEnumeration(enumeration)
    }

    suspend fun uploadAmiibo(data: ByteArray, useSave2: Boolean = false, progress: (OperationProgress) -> Unit = {}) {
        AmiiboFiles.validate(data)
        val current = refreshAmiibo()
        if (current.dirty) throw ManagementException("Sync the modified Amiibo before replacing it")
        val crc = AmiiboFiles.crc32(data)
        try {
            ack("amiibo begin ${data.size} $crc")
            data.asList().chunked(ManagementProtocol.AMIIBO_CHUNK_BYTES).forEachIndexed { index, values ->
                val offset = index * ManagementProtocol.AMIIBO_CHUNK_BYTES
                val chunk = values.toByteArray()
                ack("amiibo chunk $offset ${ManagementProtocol.hex(chunk)}")
                progress(OperationProgress("Uploading Amiibo", (offset + chunk.size).coerceAtMost(data.size), data.size))
            }
            ack(if (useSave2) "amiibo commit save2" else "amiibo commit")
            ack("amiibo persist")
            awaitPersisted()
        } catch (error: Throwable) {
            runCatching { ack("amiibo cancel") }
            throw error
        }
        refreshAmiibo()
    }

    suspend fun downloadAmiibo(progress: (OperationProgress) -> Unit = {}): AmiiboDownload {
        val status = refreshAmiibo()
        if (!status.loaded && !status.v3Loaded) throw ManagementException("No Amiibo is loaded on the adapter")
        if (status.size !in AmiiboFiles.supportedSizes) {
            throw ManagementException("Adapter reported unsupported Amiibo size ${status.size}; no memory was allocated")
        }
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
        // Current firmware exposes a whole-image CRC for v3 but leaves ordinary
        // NTAG215 payloadCrc at its zero-initialized sentinel. The portal treats
        // that field as unavailable too. Preserve validation and generation-race
        // protection rather than rejecting every ordinary adapter backup.
        val expectedCrc = status.payloadCrc.takeIf {
            status.v3Loaded || !it.equals(UNAVAILABLE_PAYLOAD_CRC, ignoreCase = true)
        }
        if (expectedCrc != null && !expectedCrc.equals(crc, ignoreCase = true)) {
            throw ManagementException("Synced Amiibo failed CRC verification ($expectedCrc != $crc)")
        }
        return AmiiboDownload(output, status.generation, expectedCrc)
    }

    /** Call only after [AmiiboDownload.bytes] is durably stored in the private local library. */
    suspend fun acknowledgeDownloadedAmiibo(download: AmiiboDownload) {
        val current = refreshAmiibo()
        val crcChanged = download.payloadCrc != null &&
            !current.payloadCrc.equals(download.payloadCrc, true)
        if (current.generation != download.generation || crcChanged) {
            throw ManagementException("Adapter Amiibo changed during Sync; the saved local copy was not acknowledged. Sync again.")
        }
        ack("amiibo downloaded")
        ack("amiibo persist")
        awaitPersisted()
        refreshAmiibo() // Required cache/state invalidation after console-mutated data is downloaded.
    }

    suspend fun setPresented(presented: Boolean) {
        ack(if (presented) "amiibo present" else "amiibo eject")
        refreshAmiibo()
    }

    suspend fun selectAmiiboCopy(used: Boolean) {
        val current = refreshAmiibo()
        if (!current.hasSave2 || current.v3Loaded) throw ManagementException("This Amiibo does not expose a separate console-written copy")
        ack(if (used) "amiibo select save2" else "amiibo select save1")
        refreshAmiibo()
    }

    suspend fun clearAmiibo() {
        val status = refreshAmiibo()
        if (status.dirty) throw ManagementException("Sync the modified Amiibo before clearing it")
        ack("amiibo clear")
        awaitCondition { !it.loaded && !it.v3Loaded && !it.persistPending }
    }

    private suspend fun awaitPersisted(): AmiiboStatus = awaitCondition { it.persisted && !it.persistPending }

    private suspend fun awaitCondition(predicate: (AmiiboStatus) -> Boolean): AmiiboStatus {
        try {
            return withTimeout(6_000) {
                while (true) {
                    val status = refreshAmiibo()
                    if (predicate(status)) return@withTimeout status
                    delay(200)
                }
                @Suppress("UNREACHABLE_CODE") AmiiboStatus()
            }
        } catch (error: TimeoutCancellationException) {
            throw ManagementException("Adapter did not finish saving within 6 seconds", error)
        }
    }

    private suspend fun command(command: String): JsonObject {
        val response = transport.transact(command)
        return ManagementProtocol.objectOrThrow(command, response)
    }

    private suspend fun ack(command: String): JsonObject = ManagementProtocol.requireOk(command, command(command))

    private suspend fun <T> parse(command: String, parser: (JsonObject) -> T): T = parser(command(command))

    private suspend fun <T> optional(name: String, block: suspend () -> T): OptionalCapability<T> = try {
        OptionalCapability(block(), CapabilityState.Available)
    } catch (error: AdapterCommandException) {
        if (error.isUnsupported()) OptionalCapability(null, CapabilityState.Unsupported) else throw error
    } catch (_: ManagementReplyTooLargeException) {
        OptionalCapability(null, CapabilityState.Unknown)
    } catch (_: ManagementException) {
        OptionalCapability(null, CapabilityState.Unknown)
    }

    private fun AdapterCommandException.isUnsupported(): Boolean =
        message?.contains("unknown command", true) == true || message?.contains("unavailable", true) == true

    private fun updateCapabilities(transform: (AdapterCapabilities) -> AdapterCapabilities) {
        _snapshot.value = _snapshot.value.copy(capabilities = transform(_snapshot.value.capabilities))
    }

    private fun markBondsUnknown() {
        _snapshot.value = _snapshot.value.copy(
            bonds = emptyList(),
            bondsComplete = null,
            bondsTotal = null,
            capabilities = _snapshot.value.capabilities.copy(bonds = CapabilityState.Unknown),
        )
    }

    private fun applyBondEnumeration(enumeration: BondEnumeration) {
        _snapshot.value = _snapshot.value.copy(
            bonds = enumeration.entries,
            bondsComplete = enumeration.complete,
            bondsTotal = enumeration.total,
            capabilities = _snapshot.value.capabilities.copy(
                bonds = if (enumeration.complete) CapabilityState.Available else CapabilityState.Unknown,
            ),
        )
    }

    private data class OptionalCapability<T>(val value: T?, val state: CapabilityState)

    private companion object {
        const val UNAVAILABLE_PAYLOAD_CRC = "00000000"
    }
}

data class AmiiboDownload(val bytes: ByteArray, val generation: Long, val payloadCrc: String?)

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
