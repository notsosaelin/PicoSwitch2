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
    /**
     * KB/M status, with the ingress counters merged in.
     *
     * Two commands, because the combined reply outgrew the wireless response
     * slot and the bridge answered `response_too_large` — which failed the whole
     * Keyboard & Mouse read rather than one field. The split is a wire detail, so
     * it is hidden here and every caller is unchanged.
     *
     * `kbm counters` is REQUIRED, not probed. This companion targets one firmware
     * contract; an adapter that does not answer it is running firmware from
     * before the profile system, and the honest response is to say so rather than
     * to synthesize zeroed counters that read as a healthy adapter receiving no
     * input — which is precisely the condition this display exists to diagnose.
     */
    suspend fun kbmStatus(): KbmStatus {
        val status = exchange(ManagementCommands.KBM_STATUS, ManagementProtocol::kbmStatus)
        val counters = exchange(ManagementCommands.KBM_COUNTERS, ManagementProtocol::kbmCounters)
        return status.copy(
            keyboardReports = counters.keyboardReports,
            mouseReports = counters.mouseReports,
            rejectedMode = counters.rejectedMode,
            rejectedDuplicate = counters.rejectedDuplicate,
            rejectedNotOwner = counters.rejectedNotOwner,
            rejectedNoPeerKey = counters.rejectedNoPeerKey,
            rejectedUnclassified = counters.rejectedUnclassified,
            rejectedNoRole = counters.rejectedNoRole,
            undecodedReports = counters.undecodedReports,
            rollover = counters.rollover,
            roleLosses = counters.roleLosses,
            mapGeneration = counters.mapGeneration,
            publishes = counters.publishes,
            recenters = counters.recenters,
        )
    }
    suspend fun kbmMouse() = exchange(ManagementCommands.KBM_MOUSE, ManagementProtocol::kbmMouse)
    suspend fun persistenceStatus() =
        exchange(ManagementCommands.SAVE_STATUS, ManagementProtocol::persistenceStatus)

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
                    kbm = kbmStatus.state,
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
        var status = WakeStatus(WakeResult.Unknown, false, false, 0, 0)
        repeat(WAKE_STATUS_POLLS) {
            delay(WAKE_STATUS_POLL_MILLIS)
            status = try {
                exchange(ManagementCommands.WAKE_STATUS, ManagementProtocol::wakeStatus)
            } catch (error: AdapterCommandException) {
                if (error.isUnsupported()) return WakeStatus(WakeResult.Unknown, false, false, 0, 0)
                throw error
            }
            if (status.result != WakeResult.Pending) return status
        }
        return status
    }

    suspend fun loadKbmMapping(profile: KbmProfile): KbmMapping =
        walkKbmMapping(profile, "the realized ${profile.wire} mapping") { cursor ->
            ManagementCommands.kbmMap(profile, cursor)
        }

    /**
     * One STORED profile's mapping, as opposed to the layout's realized one.
     *
     * Shares the walk below by construction rather than by resemblance: the two
     * had duplicate loops, and the pagination defect was present in both.
     */
    suspend fun loadKbmProfileMapping(profile: KbmProfileInfo): KbmMapping =
        walkKbmMapping(profile.layout, "stored profile ${profile.id} (${profile.name})") { cursor ->
            ManagementCommands.kbmProfileMap(profile.id, cursor)
        }

    /**
     * Walks a mapping to completion under the cursor contract, and reports what
     * is actually wrong when it cannot.
     *
     * The failure this replaces said only "incomplete", which was true and
     * useless: it named neither the layout, nor how far the walk got, nor which
     * items never arrived. That cost a full diagnostic round trip on hardware to
     * establish something the client already knew.
     */
    private suspend fun walkKbmMapping(
        layout: KbmProfile,
        what: String,
        command: (Int) -> String,
    ): KbmMapping {
        val bindings = mutableListOf<KbmBinding>()
        val visited = mutableListOf<Int>()
        var cursor = 0
        var expectedTotal: Int? = null

        while (true) {
            val page = exchange(command(cursor), ManagementProtocol::kbmMapPage)
            visited += cursor

            // The reply must be about the mapping that was asked for, or the
            // other layout's bindings would be assembled under this one's name.
            if (page.profile != layout) {
                throw ManagementPaginationException(
                    "Adapter answered for layout ${page.profile.wire} when $what was requested",
                )
            }
            if (page.cursor != cursor) {
                throw ManagementPaginationException(
                    "Adapter answered cursor ${page.cursor} for $what when $cursor was requested",
                )
            }
            if (expectedTotal == null) expectedTotal = page.total
            if (expectedTotal != page.total) {
                throw ManagementPaginationException(
                    "Adapter changed the item count of $what mid-walk: $expectedTotal then ${page.total}",
                )
            }
            // The cursor is a logical index, so the accumulator length and the
            // cursor must stay equal. They diverge only if a reply was parsed
            // into fewer rows than the adapter counted -- the exact shape of the
            // defect that shipped, seen from this side.
            if (bindings.size != cursor) {
                throw ManagementPaginationException(
                    "Reconstruction of $what desynchronized: ${bindings.size} bindings held at cursor $cursor",
                )
            }

            bindings += page.bindings
            val next = page.next ?: break
            if (next <= cursor || visited.size > KbmLimits.MAX_MAPPING_ITEMS) {
                throw ManagementPaginationException(
                    "Adapter did not advance while reading $what: cursors ${visited.joinToString("->")}, next $next",
                )
            }
            cursor = next
        }

        if (bindings.size != expectedTotal) {
            throw ManagementPaginationException(
                "Adapter returned an incomplete KB/M binding list for $what: " +
                    "${bindings.size} of $expectedTotal bindings after ${visited.size} replies " +
                    "(cursors ${visited.joinToString("->")})",
            )
        }
        return KbmMapping(layout, bindings, loaded = true)
    }

    /**
     * The profile library and both realized mappings.
     *
     * The library is paged: six rows do not fit one 512-byte reply, and
     * formatting them into the adapter's larger local buffer is exactly how
     * `kbm status` came to be refused over Bluetooth. Guarded the same way as the
     * mapping pages — a non-progressing or total-shifting adapter fails loudly
     * rather than looping.
     */
    suspend fun kbmProfiles(): KbmProfiles {
        val profiles = mutableListOf<KbmProfileInfo>()
        val visited = mutableListOf<Int>()
        var cursor = 0
        var expectedTotal: Int? = null
        var max = 0
        while (true) {
            val page = exchange(ManagementCommands.kbmProfilePage(cursor),
                                ManagementProtocol::kbmProfilePage)
            visited += cursor
            if (page.cursor != cursor) {
                throw ManagementPaginationException(
                    "Adapter answered profile cursor ${page.cursor} when $cursor was requested",
                )
            }
            if (expectedTotal == null) expectedTotal = page.total
            if (expectedTotal != page.total) {
                throw ManagementPaginationException(
                    "Adapter changed the profile count mid-walk: $expectedTotal then ${page.total}",
                )
            }
            max = page.max
            profiles += page.profiles
            val next = page.next ?: break
            if (next <= cursor || visited.size > KbmLimits.MAX_PROFILES + 1) {
                throw ManagementPaginationException(
                    "Adapter did not advance while reading the profile library: " +
                        "cursors ${visited.joinToString("->")}, next $next",
                )
            }
            cursor = next
        }
        val active = exchange(ManagementCommands.KBM_ACTIVE, ManagementProtocol::kbmActive)
        return KbmProfiles(profiles, active, max = max)
    }

    /**
     * APPLY. The only call that changes what the console is doing.
     *
     * Re-reads afterwards rather than trusting the acknowledgement: a mutation
     * returning `ok` is not evidence the adapter realized what was asked.
     */
    suspend fun applyKbmProfile(layout: KbmProfile, id: Int): KbmProfiles {
        acknowledge(ManagementCommands.kbmApply(layout, id))
        return kbmProfiles()
    }

    /**
     * SAVE. Writes a complete profile in one staged transaction, then reads the
     * stored revision back.
     *
     * This deliberately does NOT change what the console is running. On any
     * failure the draft is aborted, so a half-transferred mapping can never be
     * left staged for the next caller to commit by accident.
     */
    suspend fun saveKbmProfile(draft: KbmDraft): Pair<Int, Int> {
        acknowledge(
            ManagementCommands.kbmDraftBegin(
                draft.layout,
                // A draft on the built-in template becomes a NEW profile: the
                // Default is a template and is never written into.
                if (draft.isBuiltin) KbmProfileIds.NONE else draft.profileId,
                if (draft.isBuiltin) 0 else draft.baseRevision,
                draft.name,
            ),
        )
        try {
            draft.bindings.forEach {
                acknowledge(ManagementCommands.kbmDraftBind(it.source, it.destination))
            }
            KbmMouseField.profileOwned(draft.mouse).forEach { (field, value) ->
                acknowledge(ManagementCommands.kbmDraftMouse(field, value))
            }
            val reply = raw(ManagementCommands.KBM_DRAFT_COMMIT)
            return ManagementProtocol.kbmDraftResult(ManagementCommands.KBM_DRAFT_COMMIT, reply)
        } catch (error: Throwable) {
            // Best effort: the adapter also discards staging on disconnect and on
            // the next begin, so a failed abort cannot strand anything.
            runCatching { acknowledge(ManagementCommands.KBM_DRAFT_ABORT) }
            throw error
        }
    }

    suspend fun renameKbmProfile(id: Int, name: String): KbmProfiles {
        acknowledge(ManagementCommands.kbmProfileRename(id, name))
        return kbmProfiles()
    }

    suspend fun duplicateKbmProfile(id: Int, name: String): KbmProfiles {
        acknowledge(ManagementCommands.kbmProfileDuplicate(id, name))
        return kbmProfiles()
    }

    suspend fun deleteKbmProfile(id: Int): KbmProfiles {
        acknowledge(ManagementCommands.kbmProfileDelete(id))
        return kbmProfiles()
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

    /**
     * Read the adapter's complete logical peer inventory.
     *
     * All-or-nothing by design. A partially read inventory is worse than none:
     * the missing row is a device the user cannot see, and on this page that
     * means a saved controller they would conclude is already gone. Any
     * pagination inconsistency is therefore an exception, not a shorter list.
     */
    suspend fun listPeers(): PeerInventory {
        val entries = mutableListOf<PeerInfo>()
        val seen = mutableSetOf<String>()
        var cursor = 0
        var expectedTotal: Int? = null
        var pages = 0
        while (true) {
            val page = exchange(
                ManagementCommands.peersPage(cursor.takeIf { it != 0 }),
                ManagementProtocol::peersPage,
            )
            if (expectedTotal == null) expectedTotal = page.total
            if (expectedTotal != page.total) {
                throw ManagementPaginationException("Adapter changed the peer-list total during pagination")
            }
            page.entries.forEach { entry ->
                if (!seen.add(entry.id)) {
                    throw ManagementPaginationException("Adapter repeated a peer during pagination")
                }
                entries += entry
            }
            val next = page.next ?: break
            if (next <= cursor || ++pages > MAX_PEER_PAGES) {
                throw ManagementPaginationException("Adapter returned a non-progressing peer-list cursor")
            }
            cursor = next
        }
        val total = expectedTotal ?: 0
        if (entries.size != total) {
            throw ManagementPaginationException("Adapter returned an incomplete peer list")
        }
        return PeerInventory(entries, complete = true, total = total)
    }

    /**
     * Forget one peer, and read the adapter's verified answer.
     *
     * One command, not a disconnect-then-delete pair: the adapter sequences the
     * whole operation internally so nothing can race between the steps. The
     * caller is still expected to re-read the inventory afterwards -- the
     * firmware is authoritative about what remains, and this reply describes one
     * peer rather than the whole picture.
     */
    suspend fun forgetPeer(peerId: String): PeerForgetOutcome = exchange(
        ManagementCommands.peersForget(peerId),
        ManagementProtocol::peersForget,
    )

    /**
     * Is selective forget available on this firmware?
     *
     * Probed with a well-formed id that no adapter can hold — the peer id space
     * is a hash of an identity address, and this one addresses nothing — so a
     * firmware that HAS the command answers `already_absent` and deletes
     * nothing, while one that lacks it answers `unknown command`. There is no
     * read-only form of a mutation to probe with, so the probe is made harmless
     * instead.
     */
    suspend fun probePeerForget(): CapabilityState =
        optional { forgetPeer(UNADDRESSABLE_PEER_ID) }.state

    /**
     * Ask the adapter to open its controller pairing window.
     *
     * The adapter owns the deadline, so the app is never responsible for
     * closing it: losing this session, or the phone, cannot leave the adapter
     * discoverable. [cancelPairing] is a courtesy, not a safety mechanism.
     */
    suspend fun startPairing(): PairingStatus =
        exchange(ManagementCommands.PAIRING_START, ManagementProtocol::pairingStatus)

    /**
     * Probe remote pairing without starting one.
     *
     * `pairing status` is read-only and answers `unknown command` on firmware
     * that predates Phase 6, which is exactly the signal the optional-family
     * probe already turns into `Unsupported`. Probing with `start` instead
     * would open a real pairing window just to find out whether it exists.
     */
    suspend fun probeRemotePairing(): CapabilityState =
        optional { pairingStatus() }.state

    suspend fun pairingStatus(): PairingStatus =
        exchange(ManagementCommands.PAIRING_STATUS, ManagementProtocol::pairingStatus)

    /** Idempotent: cancelling when idle succeeds and reports idle. */
    suspend fun cancelPairing(): PairingStatus =
        exchange(ManagementCommands.PAIRING_CANCEL, ManagementProtocol::pairingStatus)

    suspend fun save(): PersistenceAcknowledgement {
        val acknowledgement = acknowledge(ManagementCommands.SAVE)
        return PersistenceAcknowledgement(
            if (acknowledgement.queued) PersistenceState.Queued else PersistenceState.Accepted,
            acknowledgement.requested,
        )
    }

    suspend fun saveAndAwait(timeoutMillis: Long = PERSIST_TIMEOUT_MILLIS): PersistenceStatus {
        val acknowledgement = save()
        return awaitPersistence(acknowledgement, timeoutMillis)
    }

    suspend fun awaitPersistence(
        acknowledgement: PersistenceAcknowledgement,
        timeoutMillis: Long = PERSIST_TIMEOUT_MILLIS,
    ): PersistenceStatus {
        require(timeoutMillis > 0) { "Persistence timeout must be positive" }
        val requestId = acknowledgement.requestId
            ?: throw ManagementProtocolException("Adapter did not identify the persistence request")
        return try {
            withTimeout(timeoutMillis) {
                while (true) {
                    val status = persistenceStatus()
                    if (requestReached(status.completed, requestId)) return@withTimeout status
                    delay(PERSIST_POLL_MILLIS)
                }
                @Suppress("UNREACHABLE_CODE")
                PersistenceStatus(false, 0, 0)
            }
        } catch (error: TimeoutCancellationException) {
            throw ManagementException("Adapter did not finish settings persistence in time", error)
        }
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
    }

    private fun requestReached(completed: Long, requestId: Long): Boolean {
        val difference = (completed - requestId) and UINT32_MASK
        return difference < UINT32_HALF_RANGE
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
        // A syntactically valid peer id that cannot name a real peer: ids are a
        // hash of an identity address and this is the all-zero hash, which no
        // address produces. Used only to probe whether `peers forget` exists.
        const val UNADDRESSABLE_PEER_ID = "p_00000000"
        const val MAX_KBM_MAP_PAGES = 32

        /**
         * Custom profile slots the adapter offers. Read from the wire would be
         * better, but the list reply's `max` is only present when the library is
         * supported at all, and this value is what distinguishes "no library"
         * from "empty library".
         */
        const val KBM_PROFILE_CAPACITY = 6
        const val MAX_BOND_PAGES = 128
        // 32 possible peers and at least one per page, so this bound can only be
        // reached by an adapter that is misbehaving.
        const val MAX_PEER_PAGES = 64
        const val WAKE_STATUS_POLLS = 6
        const val WAKE_STATUS_POLL_MILLIS = 150L
        const val AMIIBO_PERSIST_TIMEOUT_MILLIS = 6_000L
        const val AMIIBO_POLL_MILLIS = 200L
        const val PERSIST_TIMEOUT_MILLIS = 6_000L
        const val PERSIST_POLL_MILLIS = 100L
        const val UINT32_MASK = 0xFFFF_FFFFL
        const val UINT32_HALF_RANGE = 0x8000_0000L
        const val UNAVAILABLE_PAYLOAD_CRC = "00000000"
        val AMIIBO_SUPPORTED_SIZES = setOf(540, 572, 2048)
    }
}
