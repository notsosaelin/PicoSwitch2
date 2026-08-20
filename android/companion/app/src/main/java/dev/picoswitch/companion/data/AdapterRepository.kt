package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.*
import dev.picoswitch.companion.protocol.*
import dev.picoswitch.management.ManagementClient
import dev.picoswitch.management.WakeStatus
import dev.picoswitch.management.isUnsupported
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.delay
import java.util.zip.CRC32

class AdapterRepository(private val transport: ManagementTransport) {
    private val client = ManagementClient(transport)
    val connection: StateFlow<ConnectionState> = transport.connection
    private val _snapshot = MutableStateFlow(AdapterSnapshot())
    val snapshot: StateFlow<AdapterSnapshot> = _snapshot.asStateFlow()
    /**
     * Keyboard/Mouse configuration is a separate flow from [snapshot] on
     * purpose: it is polled and mutated at a different rate, and its unsaved
     * flag belongs to the connection rather than to any one refresh.
     */
    private val _kbm = MutableStateFlow(KbmState())

    fun close() = transport.close()

    suspend fun connect() {
        transport.scanAndConnect()
        validateConnectedAdapter()
    }

    suspend fun connectKnown(
        address: String,
        context: ManagementConnectionContext = ManagementConnectionContext(),
    ) {
        var retriesUsed = 0
        var directFailure: Throwable
        while (true) {
            transport.prepareConnection(context.copy(retry = retriesUsed, priorGattRetired = retriesUsed > 0))
            val direct = runCatching {
                transport.connectKnown(address)
                validateConnectedAdapter()
            }
            if (direct.isSuccess) return
            directFailure = direct.exceptionOrNull()!!
            if (!dev.picoswitch.companion.transport.GattRecoveryPolicy.shouldRetry(directFailure, retriesUsed)) break
            retriesUsed += 1
            runCatching { transport.disconnect() }
            delay(dev.picoswitch.companion.transport.GattRecoveryPolicy.RETRY_BACKOFF_MILLIS)
        }

        // Retire the direct GATT completely before the one service-filtered fallback. Restrict the
        // scan to the saved address: discovering another valid Pico nearby is not permission to
        // silently replace the user's relationship.
        runCatching { transport.disconnect() }
        transport.prepareConnection(
            context.copy(reason = "scan-fallback", retry = retriesUsed, priorGattRetired = true),
        )
        try {
            transport.scanAndConnect(address)
            validateConnectedAdapter()
        } catch (fallbackFailure: Throwable) {
            fallbackFailure.addSuppressed(directFailure)
            throw fallbackFailure
        }
    }

    private suspend fun validateConnectedAdapter() {
        try {
            refreshAll()
            if (_snapshot.value.firmware.id != "picoswitch") throw ManagementException("The discovered Bluetooth device is not a PicoSwitch2 adapter")
            transport.markValidated()
        } catch (error: Throwable) {
            runCatching { transport.disconnect() }
            throw error
        }
    }

    suspend fun disconnect() {
        transport.disconnect()
        clearDisconnectedSnapshot()
    }

    fun clearDisconnectedSnapshot() {
        _snapshot.value = AdapterSnapshot()
        // The unsaved-changes flag describes one management session. A new
        // connection must start from the adapter's authoritative state rather
        // than inheriting a dirty marker whose changes may never have landed.
        clearKbm()
    }

    suspend fun refreshAll() {
        val old = _snapshot.value
        val refresh = client.refreshAll(old)
        _snapshot.value = refresh.snapshot
        val kbmStatus = refresh.kbmStatus
        val kbmMouse = refresh.kbmMouse
        if (kbmStatus != null && kbmMouse != null) {
            _kbm.value = _kbm.value.copy(
                status = kbmStatus,
                mouse = kbmMouse,
                available = CapabilityState.Available,
                loading = false,
            )
        } else {
            _kbm.value = _kbm.value.copy(available = CapabilityState.Unknown, loading = false)
        }
    }

    suspend fun refreshAmiibo(): AmiiboStatus {
        val status = client.amiiboStatus()
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
        return status
    }

    suspend fun refreshController(): ControllerInfo {
        val controller = client.controller()
        val input = optional("active input") { client.inputSources() }
        _snapshot.value = _snapshot.value.copy(
            controller = controller,
            input = input.value ?: _snapshot.value.input,
            capabilities = _snapshot.value.capabilities.copy(activeInput = input.state),
            refreshedAtMillis = System.currentTimeMillis(),
        )
        return controller
    }

    suspend fun setActiveInput(sourceId: Long) {
        val input = client.setActiveInput(sourceId)
        _snapshot.value = _snapshot.value.copy(
            input = input,
            capabilities = _snapshot.value.capabilities.copy(activeInput = CapabilityState.Available),
            refreshedAtMillis = System.currentTimeMillis(),
        )
    }

    suspend fun setPersonality(personality: Personality): Boolean {
        val switching = client.setPersonality(personality).switching
        // A switching acknowledgement precedes USB detach/re-enumeration; retain the last
        // verified identity until a reconnect proves the requested personality is active.
        if (!switching) _snapshot.value = _snapshot.value.copy(personality = _snapshot.value.personality.copy(current = personality))
        return switching
    }

    /** Returns true when modern firmware identified and completed the persistence request. */
    suspend fun setColor(target: ColorTarget, color: RgbColor, persist: Boolean = true): Boolean {
        val (config) = client.setColor(target, color, persist = false)
        _snapshot.value = _snapshot.value.copy(config = config)
        val persistence = if (persist) client.save() else null
        if (persistence?.requestId != null) client.awaitPersistence(persistence)
        return persistence?.requestId != null
    }

    suspend fun reenumerateUsb() = client.reenumerateUsb()

    /**
     * Request a console wake and report what the adapter ACTUALLY did.
     *
     * `wake` only acknowledges delivery -- the adapter latches the request on one
     * core and performs it on the other -- so this polls `wake status` for the
     * real outcome instead of treating transmission as success. An adapter too
     * old to know `wake status` returns an Unknown result,
     * which the UI must present as "sent, outcome unknown", never as success.
     */
    suspend fun wakeConsole(): WakeStatus {
        try {
            val status = client.wakeConsole()
            updateCapabilities { it.copy(wake = CapabilityState.Available) }
            return status
        } catch (error: AdapterCommandException) {
            if (error.isUnsupported()) updateCapabilities { it.copy(wake = CapabilityState.Unsupported) }
            throw error
        }
    }

    // -----------------------------------------------------------------------
    // Keyboard / Keyboard + Mouse
    // -----------------------------------------------------------------------
    // Every KB/M command is issued from here rather than from a screen, so the
    // page never assembles wire text and the accepted values stay with the
    // protocol. All of these apply to adapter RAM immediately; only
    // [saveConfiguration] writes flash.

    val kbm: StateFlow<KbmState> = _kbm.asStateFlow()

    /** Live roles, effective mode, and mouse translation settings. */
    suspend fun refreshKbm(): KbmState {
        _kbm.value = _kbm.value.copy(loading = true)
        return try {
            val status = client.kbmStatus()
            val mouse = client.kbmMouse()
            _kbm.value = _kbm.value.copy(
                status = status,
                mouse = mouse,
                available = CapabilityState.Available,
                loading = false,
            )
            _kbm.value
        } catch (error: AdapterCommandException) {
            // Firmware without the KB/M surface: report it as unsupported so the
            // page can say so, rather than showing an empty configuration that
            // looks like a connected keyboard with nothing bound.
            if (error.isUnsupported()) {
                _kbm.value = KbmState(available = CapabilityState.Unsupported)
                _kbm.value
            } else {
                _kbm.value = _kbm.value.copy(loading = false)
                throw error
            }
        } catch (error: Throwable) {
            _kbm.value = _kbm.value.copy(loading = false)
            throw error
        }
    }

    /**
     * Read one profile's complete effective binding set.
     *
     * `kbm map` is paginated because a full binding list does not fit the
     * 511-byte wireless reply limit. The cursor is the page index and the
     * adapter reports `total`, so the loop is bounded by both and refuses a
     * page whose shape contradicts the first one.
     */
    suspend fun loadKbmMapping(profile: KbmProfile): KbmMapping {
        val mapping = client.loadKbmMapping(profile)
        _kbm.value = _kbm.value.copy(mappings = _kbm.value.mappings + (profile to mapping))
        return mapping
    }

    suspend fun setKbmMode(mode: KbmMode) {
        val status = client.setKbmMode(mode)
        _kbm.value = _kbm.value.copy(status = status)
        markKbmDirty()
    }

    /**
     * Bind, unassign, or restore one input.
     *
     * `none` and `default` are genuinely different: `none` is an override that
     * says "this input does nothing", `default` removes the override so the
     * profile's canonical binding applies again.
     */
    suspend fun bindKbm(profile: KbmProfile, source: KbmSource, destination: KbmDestination?) {
        val mapping = client.bindKbm(profile, source, destination)
        _kbm.value = _kbm.value.copy(mappings = _kbm.value.mappings + (profile to mapping))
        markKbmDirty()
    }

    /** Restore one profile's canonical bindings. Other settings are untouched. */
    suspend fun resetKbmProfile(profile: KbmProfile) {
        val mapping = client.resetKbmProfile(profile)
        _kbm.value = _kbm.value.copy(mappings = _kbm.value.mappings + (profile to mapping))
        markKbmDirty()
    }

    /**
     * Restore every KB/M default the adapter owns.
     *
     * This is the only reset the firmware exposes that reaches the mouse
     * translation settings, and it necessarily resets both profiles' bindings
     * at the same time (`ns2_kbm_runtime_reset_all`). It is presented to the
     * user as exactly that rather than as a mouse-only reset.
     */
    suspend fun resetKbmAll() {
        val (current, mappings) = client.resetAllKbm()
        _kbm.value = _kbm.value.copy(
            status = current.first,
            mouse = current.second,
            mappings = mappings,
            available = CapabilityState.Available,
        )
        markKbmDirty()
    }

    /**
     * Apply one mouse translation field.
     *
     * The adapter validates the value against its own range and rejects rather
     * than clamps, so the reply is the authority on what is now in effect and
     * is used to replace the cached configuration wholesale.
     */
    suspend fun setKbmMouse(field: KbmMouseField, value: Int) {
        _kbm.value = _kbm.value.copy(mouse = client.setKbmMouse(field, value))
        markKbmDirty()
    }

    /**
     * Persist the adapter's current runtime configuration to flash.
     *
     * Over the wireless transport this is acknowledged as queued -- core1
     * performs the write at a safe point so the controller report loop is not
     * stalled -- so a successful ack means accepted, not yet written. The UI
     * says "Saved to adapter" only on that acceptance because the protocol
     * offers nothing stronger to wait for.
     */
    suspend fun saveConfiguration() {
        _kbm.value = _kbm.value.copy(saving = true)
        try {
            client.save()
            _kbm.value = _kbm.value.copy(dirty = false, saving = false)
        } catch (error: Throwable) {
            _kbm.value = _kbm.value.copy(saving = false)
            throw error
        }
    }

    private fun markKbmDirty() {
        _kbm.value = _kbm.value.copy(dirty = true)
    }

    private fun clearKbm() {
        _kbm.value = KbmState()
    }

    suspend fun setManagementEnabled(enabled: Boolean) {
        _snapshot.value = _snapshot.value.copy(managementEnabled = client.setManagementEnabled(enabled))
    }

    suspend fun listBonds(): List<BondInfo> {
        markBondsUnknown()
        val enumeration = client.listBonds()
        applyBondEnumeration(enumeration)
        return enumeration.entries
    }

    /**
     * Remove a stored phone bond.
     *
     * Returns true when this app's own session survived. Removing a bond can
     * revoke **this phone's** authorization, and Android does not expose our
     * Bluetooth address, so the entry cannot be compared against ourselves.
     * The session is therefore verified rather than assumed: leaving the UI
     * "Connected" after deleting the relationship that permits the connection is
     * exactly the stale state this guards against.
     */
    suspend fun removeBond(index: Int): Boolean {
        if (_snapshot.value.bondsComplete != true) {
            throw ManagementException("Bond list completeness is unknown; refresh on a versioned firmware before removing a bond")
        }
        // A timeout after a flash mutation is ambiguous; hide the previous
        // authoritative list until a fresh complete enumeration succeeds.
        markBondsUnknown()
        return try {
            val enumeration = client.removeBond(index)
            applyBondEnumeration(enumeration)
            true
        } catch (error: Throwable) {
            // The follow-up read is the probe: if the link or its authorization
            // is gone, this is where it shows, and the caller reconciles.
            false
        }
    }

    suspend fun uploadAmiibo(data: ByteArray, useSave2: Boolean = false, progress: (OperationProgress) -> Unit = {}) {
        AmiiboFiles.validate(data)
        val status = client.uploadAmiibo(data, useSave2) { completed, total ->
            progress(OperationProgress("Uploading Amiibo", completed, total))
        }
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
    }

    suspend fun downloadAmiibo(progress: (OperationProgress) -> Unit = {}): AmiiboDownload {
        val download = client.downloadAmiibo { completed, total ->
            progress(OperationProgress("Syncing Amiibo", completed, total))
        }
        AmiiboFiles.validate(download.bytes)
        return download
    }

    /** Call only after [AmiiboDownload.bytes] is durably stored in the private local library. */
    suspend fun acknowledgeDownloadedAmiibo(download: AmiiboDownload) {
        val status = client.acknowledgeDownloadedAmiibo(download)
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
    }

    suspend fun setPresented(presented: Boolean) {
        val status = client.setAmiiboPresented(presented)
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
    }

    suspend fun selectAmiiboCopy(used: Boolean) {
        val status = client.selectAmiiboCopy(used)
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
    }

    suspend fun clearAmiibo() {
        val status = client.clearAmiibo()
        _snapshot.value = _snapshot.value.copy(amiibo = status, refreshedAtMillis = System.currentTimeMillis())
    }

    private suspend fun <T> optional(@Suppress("UNUSED_PARAMETER") name: String, block: suspend () -> T): OptionalCapability<T> = try {
        OptionalCapability(block(), CapabilityState.Available)
    } catch (error: AdapterCommandException) {
        if (error.isUnsupported()) OptionalCapability(null, CapabilityState.Unsupported) else throw error
    } catch (_: ManagementReplyTooLargeException) {
        OptionalCapability(null, CapabilityState.Unknown)
    } catch (_: ManagementException) {
        OptionalCapability(null, CapabilityState.Unknown)
    }

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

}

typealias AmiiboDownload = dev.picoswitch.management.AmiiboDownload
typealias ColorTarget = dev.picoswitch.management.ColorTarget

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
