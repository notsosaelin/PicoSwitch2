package dev.picoswitch.management

import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import java.util.zip.CRC32

/**
 * Portable management workflows over an already-connected channel.
 *
 * This class owns command construction, reply parsing, pagination,
 * mutation/readback, and transactional adapter workflows. It owns no platform
 * connection objects and no presentation or local-library state.
 */
class ManagementClient(
    private val channel: ManagementChannel,
    private val nowMillis: () -> Long = System::currentTimeMillis,
) {
    suspend fun firmware() = exchange(ManagementCommands.INFO, ManagementProtocol::firmware)
    suspend fun controller() = exchange(ManagementCommands.DEVICE, ManagementProtocol::controller)
    suspend fun config() = exchange(ManagementCommands.GET_CONFIG, ManagementProtocol::config)
    suspend fun personality() = exchange(ManagementCommands.PERSONALITY, ManagementProtocol::personality)
    suspend fun amiiboStatus() = exchange(ManagementCommands.AMIIBO_STATUS, ManagementProtocol::amiibo)
    suspend fun inputSources() = exchange(ManagementCommands.INPUT_SOURCES, ManagementProtocol::inputSources)
    suspend fun managementEnabled() = exchange(ManagementCommands.MANAGEMENT_STATUS, ManagementProtocol::managementEnabled)
    suspend fun kbmStatus() = exchange(ManagementCommands.KBM_STATUS, ManagementProtocol::kbmStatus)
    suspend fun kbmMouse() = exchange(ManagementCommands.KBM_MOUSE, ManagementProtocol::kbmMouse)

    /** Initial portable state composition. Unsupported optional families remain explicit. */
    suspend fun refreshAll(previous: AdapterSnapshot = AdapterSnapshot()): ManagementRefresh {
        val firmware = firmware()
        val config = config()
        val controller = controller()
        val personality = optional { personality() }
        val amiibo = optional { amiiboStatus() }
        val management = optional { managementEnabled() }
        val bonds = optional { listBonds() }
        val input = optional { inputSources() }
        val kbmStatus = optional { kbmStatus() }
        val kbmMouse = if (kbmStatus.value != null) optional { kbmMouse() } else OptionalResult<KbmMouseConfig>(null, kbmStatus.state)
        val bondCapability = when {
            bonds.value == null -> bonds.state
            bonds.value.complete -> CapabilityState.Available
            else -> CapabilityState.Unknown
        }
        return ManagementRefresh(
            snapshot = AdapterSnapshot(
                firmware = firmware,
                controller = controller,
                personality = personality.value ?: previous.personality,
                config = config,
                amiibo = amiibo.value ?: previous.amiibo,
                managementEnabled = management.value ?: previous.managementEnabled,
                bonds = bonds.value?.entries ?: emptyList(),
                bondsComplete = bonds.value?.complete,
                bondsTotal = bonds.value?.total,
                input = input.value ?: previous.input,
                capabilities = AdapterCapabilities(
                    core = CapabilityState.Available,
                    personality = personality.state,
                    colors = CapabilityState.Available,
                    amiibo = amiibo.state,
                    managementGate = management.state,
                    bonds = bondCapability,
                    wake = previous.capabilities.wake,
                    activeInput = input.state,
                ),
                refreshedAtMillis = nowMillis(),
            ),
            kbmStatus = kbmStatus.value,
            kbmMouse = kbmMouse.value,
        )
    }

    suspend fun setActiveInput(sourceId: Long): AdapterInputState {
        acknowledge(ManagementCommands.inputActive(sourceId))
        return inputSources()
    }

    suspend fun setPersonality(personality: Personality): CommandAcknowledgement =
        acknowledge(ManagementCommands.personality(personality))

    /** Runtime color mutation, optional persistence request, then authoritative readback. */
    suspend fun setColor(
        target: ColorTarget,
        color: RgbColor,
        persist: Boolean = true,
    ): Pair<AdapterConfig, PersistenceAcknowledgement?> {
        acknowledge(ManagementCommands.color(target, color))
        val persistence = if (persist) save() else null
        return config() to persistence
    }

    suspend fun reenumerateUsb() {
        val acknowledgement = acknowledge(ManagementCommands.REENUMERATE)
        if (!acknowledgement.reenumerating) {
            throw ManagementProtocolException("Adapter did not confirm USB re-enumeration")
        }
    }

    suspend fun setManagementEnabled(enabled: Boolean): Boolean? =
        acknowledge(ManagementCommands.managementEnabled(enabled)).enabled

    suspend fun wakeConsole(): WakeStatus {
        acknowledge(ManagementCommands.WAKE)
        var status = WakeStatus(WakeResult.Unknown, false, false, 0)
        repeat(WAKE_STATUS_POLLS) {
            delay(WAKE_STATUS_POLL_MILLIS)
            status = try {
                exchange(ManagementCommands.WAKE_STATUS, ManagementProtocol::wakeStatus)
            } catch (error: AdapterCommandException) {
                if (error.isUnsupported()) return WakeStatus(WakeResult.Unknown, false, false, 0)
                throw error
            }
            if (status.result != WakeResult.Pending) return status
        }
        return status
    }

    suspend fun loadKbmMapping(profile: KbmProfile): KbmMapping {
        val bindings = mutableListOf<KbmBinding>()
        var pageNumber = 0
        var expectedTotal: Int? = null
        while (true) {
            val command = ManagementCommands.kbmMap(profile, pageNumber)
            val page = exchange(command, ManagementProtocol::kbmMapPage)
            if (page.profile != profile || page.page != pageNumber) {
                throw ManagementPaginationException("Adapter returned a different KB/M profile or page")
            }
            if (expectedTotal == null) expectedTotal = page.total
            if (expectedTotal != page.total || bindings.size + page.bindings.size > page.total) {
                throw ManagementPaginationException("Adapter changed the KB/M binding total during pagination")
            }
            bindings += page.bindings
            if (!page.more) break
            if (page.bindings.isEmpty() || ++pageNumber > MAX_KBM_MAP_PAGES) {
                throw ManagementPaginationException("Adapter returned a non-progressing KB/M binding list")
            }
        }
        if (bindings.size != expectedTotal) {
            throw ManagementPaginationException("Adapter returned an incomplete KB/M binding list")
        }
        return KbmMapping(profile, bindings, loaded = true)
    }

    suspend fun setKbmMode(mode: KbmMode): KbmStatus {
        acknowledge(ManagementCommands.kbmMode(mode))
        return kbmStatus()
    }

    suspend fun bindKbm(
        profile: KbmProfile,
        source: KbmSource,
        destination: KbmDestination?,
    ): KbmMapping {
        acknowledge(ManagementCommands.kbmBind(profile, source, destination))
        return loadKbmMapping(profile)
    }

    suspend fun resetKbmProfile(profile: KbmProfile): KbmMapping {
        acknowledge(ManagementCommands.kbmReset(profile))
        return loadKbmMapping(profile)
    }

    suspend fun resetAllKbm(): Pair<Pair<KbmStatus, KbmMouseConfig>, Map<KbmProfile, KbmMapping>> {
        acknowledge(ManagementCommands.KBM_RESET_ALL)
        val status = kbmStatus()
        val mouse = kbmMouse()
        val mappings = KbmProfile.entries.associateWith { loadKbmMapping(it) }
        return (status to mouse) to mappings
    }

    suspend fun setKbmMouse(field: KbmMouseField, value: Int): KbmMouseConfig {
        val command = ManagementCommands.kbmMouse(field, value)
        return exchange(command, ManagementProtocol::kbmMouse)
    }

    suspend fun listBonds(): BondEnumeration {
        val legacyCommand = "bonds list"
        val first = raw(legacyCommand)
        val versioned = try {
            ManagementProtocol.isVersionedBondResponse(legacyCommand, first)
        } catch (error: AdapterCommandException) {
            if (error.code == 413 || error.adapterMessage.contains("response_too_large", ignoreCase = true)) {
                return collectBondPages()
            }
            throw error
        }
        if (versioned) {
            return collectBondPages(first)
        }
        return ManagementProtocol.legacyBonds(legacyCommand, first)
    }

    suspend fun removeBond(index: Int): BondEnumeration {
        acknowledge(ManagementCommands.bondRemove(index))
        return listBonds()
    }

    suspend fun save(): PersistenceAcknowledgement {
        val acknowledgement = acknowledge(ManagementCommands.SAVE)
        return PersistenceAcknowledgement(
            if (acknowledgement.queued) PersistenceState.Queued else PersistenceState.Accepted,
        )
    }

    suspend fun uploadAmiibo(
        data: ByteArray,
        useSave2: Boolean = false,
        progress: (completed: Int, total: Int) -> Unit = { _, _ -> },
    ): AmiiboStatus {
        validateAmiiboSize(data.size)
        val current = amiiboStatus()
        if (current.dirty) throw ManagementException("Sync the modified Amiibo before replacing it")
        try {
            acknowledge(ManagementCommands.amiiboBegin(data.size, crc32(data)))
            data.asList().chunked(ManagementProtocol.AMIIBO_CHUNK_BYTES).forEachIndexed { index, values ->
                val offset = index * ManagementProtocol.AMIIBO_CHUNK_BYTES
                val chunk = values.toByteArray()
                acknowledge(ManagementCommands.amiiboChunk(offset, chunk))
                progress((offset + chunk.size).coerceAtMost(data.size), data.size)
            }
            acknowledge(ManagementCommands.amiiboCommit(useSave2))
            acknowledge(ManagementCommands.AMIIBO_PERSIST)
            awaitAmiibo { it.persisted && !it.persistPending }
        } catch (error: Throwable) {
            runCatching { acknowledge(ManagementCommands.AMIIBO_CANCEL) }
            throw error
        }
        return amiiboStatus()
    }

    suspend fun downloadAmiibo(
        progress: (completed: Int, total: Int) -> Unit = { _, _ -> },
    ): AmiiboDownload {
        val status = amiiboStatus()
        if (!status.loaded && !status.v3Loaded) throw ManagementException("No Amiibo is loaded on the adapter")
        if (status.size !in AMIIBO_SUPPORTED_SIZES) {
            throw ManagementException(
                "Adapter reported unsupported Amiibo size ${status.size}; no memory was allocated",
            )
        }
        val output = ByteArray(status.size)
        var offset = 0
        while (offset < output.size) {
            val count = minOf(ManagementProtocol.AMIIBO_CHUNK_BYTES, output.size - offset)
            val command = ManagementCommands.amiiboRead(offset, count)
            val bytes = exchange(command, ManagementProtocol::readData)
            if (bytes.size != count) {
                throw ManagementProtocolException("Adapter returned ${bytes.size} of $count requested bytes")
            }
            bytes.copyInto(output, offset)
            offset += count
            progress(offset, output.size)
        }
        val crc = crc32(output)
        val expectedCrc = status.payloadCrc.takeIf {
            status.v3Loaded || !it.equals(UNAVAILABLE_PAYLOAD_CRC, ignoreCase = true)
        }
        if (expectedCrc != null && !expectedCrc.equals(crc, ignoreCase = true)) {
            throw ManagementProtocolException("Synced Amiibo failed CRC verification ($expectedCrc != $crc)")
        }
        return AmiiboDownload(output, status.generation, expectedCrc)
    }

    suspend fun acknowledgeDownloadedAmiibo(download: AmiiboDownload): AmiiboStatus {
        val current = amiiboStatus()
        val crcChanged = download.payloadCrc != null && !current.payloadCrc.equals(download.payloadCrc, true)
        if (current.generation != download.generation || crcChanged) {
            throw ManagementException("Adapter Amiibo changed during Sync; acknowledge was refused")
        }
        acknowledge(ManagementCommands.AMIIBO_DOWNLOADED)
        acknowledge(ManagementCommands.AMIIBO_PERSIST)
        awaitAmiibo { it.persisted && !it.persistPending }
        return amiiboStatus()
    }

    suspend fun setAmiiboPresented(presented: Boolean): AmiiboStatus {
        acknowledge(ManagementCommands.amiiboPresented(presented))
        return amiiboStatus()
    }

    suspend fun selectAmiiboCopy(used: Boolean): AmiiboStatus {
        val current = amiiboStatus()
        if (!current.hasSave2 || current.v3Loaded) {
            throw ManagementException("This Amiibo does not expose a separate console-written copy")
        }
        acknowledge(ManagementCommands.amiiboSelect(used))
        return amiiboStatus()
    }

    suspend fun clearAmiibo(): AmiiboStatus {
        val current = amiiboStatus()
        if (current.dirty) throw ManagementException("Sync the modified Amiibo before clearing it")
        acknowledge(ManagementCommands.AMIIBO_CLEAR)
        return awaitAmiibo { !it.loaded && !it.v3Loaded && !it.persistPending }
    }

    private suspend fun collectBondPages(firstResponse: String? = null): BondEnumeration {
        val entries = mutableListOf<BondInfo>()
        val seen = mutableSetOf<Int>()
        var cursor = 0
        var expectedTotal: Int? = null
        var response = firstResponse
        var pages = 0
        while (true) {
            val command = if (response == null) ManagementCommands.bondsPage(cursor.takeIf { it != 0 }) else "bonds list"
            val page = if (response == null) {
                exchange(command, ManagementProtocol::bondsPage)
            } else {
                ManagementProtocol.bondsPage(command, response).also { response = null }
            }
            if (expectedTotal == null) expectedTotal = page.total
            if (expectedTotal != page.total) {
                throw ManagementPaginationException("Adapter changed the bond-list total during pagination")
            }
            page.entries.forEach { entry ->
                if (!seen.add(entry.index)) throw ManagementPaginationException("Adapter repeated a bond during pagination")
                entries += entry
            }
            val next = page.next ?: break
            if (next <= cursor || ++pages > MAX_BOND_PAGES) {
                throw ManagementPaginationException("Adapter returned a non-progressing bond-list cursor")
            }
            cursor = next
        }
        val total = expectedTotal ?: 0
        if (entries.size != total) throw ManagementPaginationException("Adapter returned an incomplete bond list")
        return BondEnumeration(entries, complete = true, total = total)
    }

    private suspend fun awaitAmiibo(predicate: (AmiiboStatus) -> Boolean): AmiiboStatus = try {
        withTimeout(AMIIBO_PERSIST_TIMEOUT_MILLIS) {
            while (true) {
                val status = amiiboStatus()
                if (predicate(status)) return@withTimeout status
                delay(AMIIBO_POLL_MILLIS)
            }
            @Suppress("UNREACHABLE_CODE")
            AmiiboStatus()
        }
    } catch (error: TimeoutCancellationException) {
        throw ManagementException("Adapter did not finish persistence within 6 seconds", error)
    }

    private suspend fun acknowledge(command: String): CommandAcknowledgement =
        ManagementProtocol.acknowledgement(command, raw(command))

    private suspend fun <T> exchange(command: String, parser: (String, String) -> T): T =
        parser(command, raw(command))

    private suspend fun raw(command: String): String {
        ManagementProtocol.frame(command)
        return channel.transact(command, ManagementChannel.DEFAULT_TIMEOUT_MILLIS)
    }

    private suspend fun <T> optional(block: suspend () -> T): OptionalResult<T> = try {
        OptionalResult(block(), CapabilityState.Available)
    } catch (error: AdapterCommandException) {
        if (error.isUnsupported()) OptionalResult(null, CapabilityState.Unsupported) else throw error
    } catch (_: ManagementException) {
        OptionalResult(null, CapabilityState.Unknown)
    }

    private fun validateAmiiboSize(size: Int) {
        if (size !in AMIIBO_SUPPORTED_SIZES) {
            throw ManagementException("Amiibo image size must be 540, 572, or 2048 bytes")
        }
    }

    private fun crc32(data: ByteArray): String =
        "%08X".format(CRC32().apply { update(data) }.value)

    private data class OptionalResult<T>(val value: T?, val state: CapabilityState)

    private companion object {
        const val MAX_KBM_MAP_PAGES = 32
        const val MAX_BOND_PAGES = 128
        const val WAKE_STATUS_POLLS = 6
        const val WAKE_STATUS_POLL_MILLIS = 150L
        const val AMIIBO_PERSIST_TIMEOUT_MILLIS = 6_000L
        const val AMIIBO_POLL_MILLIS = 200L
        const val UNAVAILABLE_PAYLOAD_CRC = "00000000"
        val AMIIBO_SUPPORTED_SIZES = setOf(540, 572, 2048)
    }
}
