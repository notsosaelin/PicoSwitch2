package dev.picoswitch.companion.ui

import android.Manifest
import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.companion.CompanionDeviceManager
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.core.content.ContextCompat
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.viewModelScope
import dev.picoswitch.companion.BuildConfig
import dev.picoswitch.bridge.core.BridgeFormat
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerCandidates
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerLayoutResolver
import dev.picoswitch.bridge.core.ControllerState
import dev.picoswitch.bridge.core.ResolvedControllerLayout
import dev.picoswitch.bridge.protocol.BridgeContract
import dev.picoswitch.bridge.protocol.BridgeHidDescriptor
import dev.picoswitch.bridge.protocol.ControllerReportEncoder
import dev.picoswitch.bridge.session.BridgeHost
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.bridge.session.BridgeState
import dev.picoswitch.bridge.session.SessionResumePolicy
import dev.picoswitch.bridge.touch.TouchControlConfig
import dev.picoswitch.bridge.touch.TouchReleaseReason
import dev.picoswitch.companion.bridge.AndroidBridge
import dev.picoswitch.companion.bridge.AndroidBridgeHost
import dev.picoswitch.companion.data.*
import dev.picoswitch.companion.diagnostics.DiagnosticEntry
import dev.picoswitch.companion.diagnostics.DiagnosticLog
import dev.picoswitch.companion.diagnostics.DiagnosticSummary
import dev.picoswitch.companion.diagnostics.ManagementDiagnosticContext
import dev.picoswitch.companion.model.*
import dev.picoswitch.management.WakeResult
import dev.picoswitch.companion.transport.BleGattManagementTransport
import dev.picoswitch.companion.protocol.ManagementConnectionContext
import dev.picoswitch.companion.ui.touch.TouchBackgroundStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest

/**
 * The application's top-level destinations.
 *
 * Five, deliberately: the adapter itself, the two things it can be configured
 * to do (Keyboard & Mouse, Amiibo), using this handheld as the controller, and
 * settings. Personality and appearance moved into [Adapter] because they are
 * facts about the same physical device rather than a separate place to visit,
 * and Diagnostics is reached from Settings because it is a troubleshooting
 * instrument, not a daily destination.
 *
 * `label` is what the navigation bar shows and `title` is the page heading.
 * They are separate because a five-item compact bar gives an unselected item
 * roughly eight characters before Material ellipsises it -- and an ellipsised
 * destination name is a navigation the user has to guess at. Every label is
 * therefore kept short enough to survive that, plus a raised font scale.
 */
enum class AppSection(val label: String, val title: String, val subtitle: String = "") {
    Adapter("Adapter", "Adapter", "Status, mode, and appearance"),
    Keyboard("Keyboard", "Keyboard & Mouse", "Devices, mapping, and mouse feel"),
    Amiibo("Amiibo", "Amiibo", "Your private figure library"),
    Controller("Gamepad", "Gamepad", "Choose what drives the console"),
    Settings("Settings", "Settings", ""),
}

/**
 * A screen that is pushed over a section rather than being one of its own.
 *
 * Kept as explicit state instead of a navigation library because the app has
 * exactly two such screens and the section state is already saved and restored
 * here; adding a graph would move that ownership without removing any.
 */
enum class AppOverlay { None, Diagnostics, AmiiboSettings }

data class PlatformDiagnostics(
    val bluetoothAvailable: Boolean = false,
    val bluetoothEnabled: Boolean = false,
    val scanPermission: Boolean = false,
    val connectPermission: Boolean = false,
    val companionDeviceManager: Boolean = false,
)

data class CompanionUiState(
    val section: AppSection = AppSection.Adapter,
    val overlay: AppOverlay = AppOverlay.None,
    val kbm: KbmState = KbmState(),
    /**
     * A Keyboard & Mouse command is in flight. Deliberately separate from
     * [busy]: mapping and tuning changes are frequent and local, and covering
     * the whole application with the modal progress overlay for each one made
     * live mouse tuning unusable.
     */
    val kbmBusy: Boolean = false,
    val connection: ConnectionState = ConnectionState(),
    val snapshot: AdapterSnapshot = AdapterSnapshot(),
    val library: List<AmiiboLibraryItem> = emptyList(),
    val libraryWarnings: List<String> = emptyList(),
    val selectedAmiiboId: String? = null,
    /** Cache hits for library cards; this is metadata only and never raw tag data. */
    val amiiboCatalogEntries: Map<String, AmiiboCatalogEntry> = emptyMap(),
    val selectedAmiiboDetails: AmiiboDetails? = null,
    val selectedAmiiboCatalog: AmiiboCatalogEntry? = null,
    val selectedAmiiboTitleGame: String? = null,
    val amiiboCatalogLoading: Boolean = false,
    /** Catalog enrichment for the active adapter tag, even without a local backup. */
    val adapterAmiiboCatalog: AmiiboCatalogEntry? = null,
    val adapterAmiiboCatalogState: AmiiboCatalogState = AmiiboCatalogState.Idle,
    val nfcScan: NfcScanStatus = NfcScanStatus(),
    val amiiboKeysLoaded: Boolean = false,
    val operation: OperationProgress? = null,
    val busy: Boolean = false,
    val message: String? = null,
    val bridge: BridgeState = BridgeState(),
    val controllerState: ControllerState = ControllerState.Neutral,
    val sourceDevices: List<SourceDeviceUi> = emptyList(),
    /** True only when more than one usable controller exists. */
    val sourceChoiceRequired: Boolean = false,
    val excludedSources: List<ExcludedSourceUi> = emptyList(),
    val selectedSourceDescriptor: String? = null,
    val requestedFaceLayout: ControllerFaceLayout = ControllerFaceLayout.Auto,
    val resolvedFaceLayout: ResolvedControllerLayout = ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, null),
    /**
     * The on-screen controller is open and is the authoritative gameplay input.
     *
     * Deliberately not an [AppSection] or an [AppOverlay]: it is a full-screen
     * application MODE that owns its own presentation, hides the navigation
     * chrome, and has to be entered and left explicitly. Squeezing it into the
     * ordinary content column would give it the scaffold's insets and width
     * limits, which is the opposite of what a gameplay surface needs.
     */
    val touchGamepadActive: Boolean = false,
    val touchSettings: TouchGamepadSettings = TouchGamepadSettings.Default,
    /**
     * Face presentation for the DRAWN diamond. Separate persistence from the
     * per-device preference, same resolver; see [AndroidControllerLayoutStore].
     */
    val touchFaceLayout: ControllerFaceLayout = ControllerFaceLayout.Nintendo,
    val adapterRelationship: AdapterRelationship? = null,
    val relationshipStatus: AdapterRelationshipStatus = AdapterRelationshipStatus(AdapterRelationshipPhase.NoRelationship),
    val platform: PlatformDiagnostics = PlatformDiagnostics(),
    val diagnosticSummary: DiagnosticSummary = DiagnosticSummary(),
    val diagnosticEntries: List<DiagnosticEntry> = emptyList(),
    val identityRefreshPending: Boolean = false,
    /**
     * Whether the FLASHED adapter firmware implements the bridge contract this
     * app speaks. Never silently assumed compatible; see [BridgeContract].
     */
    val bridgeCompatibility: BridgeContract.Compatibility = BridgeContract.Compatibility.NotConnected,
)

data class SourceDeviceUi(val id: Int, val descriptor: String, val name: String, val vendorId: Int, val productId: Int)

/** An input Android offered that cannot serve as a controller, and why. */
data class ExcludedSourceUi(
    val name: String,
    val vendorId: Int,
    val productId: Int,
    val reason: String,
)

class CompanionViewModel(application: Application, private val savedState: SavedStateHandle) : AndroidViewModel(application) {
    val diagnostics = DiagnosticLog()

    /**
     * The Android platform backend and the shared bridge session it drives.
     *
     * The ViewModel is the application-facing interface (§ UI separation): it
     * observes [AndroidBridge.session] state and forwards user intent. It does not
     * encode reports, touch the HID profile, or handle sensors — all of that is
     * behind [bridge].
     */
    private val bridge = AndroidBridge(application, diagnostics)

    /** Raw input entry point for the Activity's key/motion dispatch. */
    val inputBackend get() = bridge.input
    private val session get() = bridge.session

    /**
     * The on-screen controller, for the surface that renders it.
     *
     * Exposed rather than proxied because the surface is the only thing that can
     * supply the two platform pieces it needs — a measured interaction rectangle
     * and a view to feel haptics through — and wrapping every contact in a
     * ViewModel call would put the report path behind a state holder for nothing.
     */
    val touchGamepad get() = bridge.touch

    // One management relationship per process, not per screen. Constructing this
    // here used to give every stacked Activity its own transport, GATT and
    // background poller -- five of them were live at once on 2026-08-23. See
    // ManagementOwner for the captured evidence.
    private val adapter = ManagementOwner.get(application, diagnostics)
    private val library = AmiiboLibrary(application)
    private val themeStore = ThemePreferenceStore(application)
    private val touchSettingsStore = TouchGamepadSettingsStore(application)
    private val relationshipStore = AdapterRelationshipStore(application)
    private val relationshipCoordinator = AdapterRelationshipCoordinator(relationshipStore.load())
    private data class PairingDevice(val generation: Long, val device: BluetoothDevice)
    @Volatile private var pairingDevice: PairingDevice? = null
    private var relationshipPairingJob: Job? = null
    private var relationshipConnectionJob: Job? = null
    private var relationshipRetirementJob: Job? = null
    // Non-zero only for the attempt whose bond is being provoked by the management GATT link
    // itself (the compatibility path). That connect has to hold Android's pairing dialog inside
    // its deadline, which an ordinary bonded connect never does.
    @Volatile private var gattInitiatedBondGeneration = 0L
    private val operationAdmission = OperationAdmissionGate()
    private val activeIdentityPolicy = ActiveControllerIdentityPolicy()
    private var autoReconnectAttempted = false
    private var automaticControllerResumeJob: Job? = null
    private var bridgeSourceReconciliationJob: Job? = null
    @Volatile private var personalityTransitionActive = false
    private val _theme = MutableStateFlow(themeStore.load())
    val theme: StateFlow<ThemeSelection> = _theme.asStateFlow()
    private val amiiboKeyStore = AmiiboKeyStore(File(application.filesDir, "amiibo-private"))
    private val amiiboCatalog = AmiiboCatalogStore(File(application.filesDir, "amiibo-private"))
    private var selectedDetailsJob: Job? = null
    private var adapterCatalogJob: Job? = null
    private var adapterCatalogFigureId: String? = null
    private var kbmMouseJob: Job? = null
    private var kbmMappingsLoaded = false
    private val initialSection = savedState.get<String>(KEY_SECTION)?.let { runCatching { AppSection.valueOf(it) }.getOrNull() } ?: AppSection.Adapter
    private val _ui = MutableStateFlow(
        CompanionUiState(
            section = initialSection,
            selectedAmiiboId = savedState[KEY_AMIIBO],
            amiiboKeysLoaded = amiiboKeyStore.read() != null,
            selectedSourceDescriptor = savedState[KEY_SOURCE],
            adapterRelationship = relationshipStore.load(),
            relationshipStatus = relationshipCoordinator.status,
            identityRefreshPending = savedState[KEY_IDENTITY_PENDING] ?: false,
            // Configuration only. Nothing about what is currently HELD survives a
            // process restart -- see TouchGamepadSettings.
            touchSettings = touchSettingsStore.load(),
            touchFaceLayout = bridge.layoutStore.loadTouch(),
        ),
    )
    val ui: StateFlow<CompanionUiState> = _ui.asStateFlow()

    init {
        diagnostics.event("app", "created", "version ${BuildConfig.VERSION_NAME}")
        viewModelScope.launch {
            adapter.connection.collect { value ->
                if (value.connected) {
                    // The lifecycle coordinator persists a relationship only after AdapterRepository
                    // has verified the management identity. A ready CCC subscription alone is not
                    // product-level success and must not create or replace a saved adapter.
                    _ui.update { it.copy(connection = value) }
                    refreshBridgeCompatibility()
                } else {
                    if (value.phase == ConnectionPhase.Idle ||
                        value.phase == ConnectionPhase.Reconnecting ||
                        value.phase == ConnectionPhase.Failed
                    ) {
                        relationshipCoordinator.connectionEnded(value.message)
                        adapter.clearDisconnectedSnapshot()
                        // Bindings belong to the adapter that was connected.
                        // Keeping them across a disconnect would let the next
                        // session open on another adapter's mapping.
                        kbmMappingsLoaded = false
                    }
                    _ui.update {
                        it.copy(connection = value, relationshipStatus = relationshipCoordinator.status)
                    }
                    refreshBridgeCompatibility()
                }
            }
        }
        viewModelScope.launch {
            adapter.snapshot.collect { value ->
                _ui.update { it.copy(snapshot = value) }
                refreshBridgeCompatibility()
                refreshAdapterAmiiboCatalog(value.amiibo)
            }
        }
        viewModelScope.launch {
            library.items.collect { value ->
                _ui.update { old ->
                    val selected = old.selectedAmiiboId?.takeIf { id -> value.any { it.id == id } } ?: value.firstOrNull()?.id
                    savedState[KEY_AMIIBO] = selected
                    old.copy(
                        library = value,
                        selectedAmiiboId = selected,
                        amiiboCatalogEntries = catalogEntriesFor(value),
                    )
                }
                refreshSelectedAmiiboDetails()
            }
        }
        viewModelScope.launch { adapter.kbm.collect { value -> _ui.update { it.copy(kbm = value) } } }
        viewModelScope.launch { library.warnings.collect { value -> _ui.update { it.copy(libraryWarnings = value) } } }
        viewModelScope.launch { session.state.collect { value -> _ui.update { it.copy(bridge = value) } } }
        viewModelScope.launch {
            session.state.map { it.phase }.distinctUntilChanged().collect { phase ->
                ManagementDiagnosticContext.setBridgePhase(phase)
                diagnostics.event("controller", "bridge.phase", phase.name)
                if (phase == BridgeLinkPhase.Playing) {
                    reconcileControllerLinkSource()
                } else {
                    bridgeSourceReconciliationJob?.cancel()
                    // The link is gone, so held input can no longer be cleared
                    // through it. Dropping it here is also what makes a reconnect
                    // start from neutral instead of replaying whatever was down
                    // when the link dropped.
                    bridge.releaseTouchInput(TouchReleaseReason.LinkEnded)
                }
            }
        }
        viewModelScope.launch { inputBackend.state.collect { value -> _ui.update { it.copy(controllerState = value) } } }
        viewModelScope.launch { diagnostics.summary.collect { value -> _ui.update { it.copy(diagnosticSummary = value) } } }
        viewModelScope.launch { diagnostics.entries.collect { value -> _ui.update { it.copy(diagnosticEntries = value) } } }
        viewModelScope.launch {
            adapter.connection.map { it.connected }.distinctUntilChanged().collectLatest { connected ->
                while (connected) {
                    delay(ADAPTER_POLL_MILLIS)
                    val state = _ui.value
                    if (!state.busy && !state.kbmBusy && !personalityTransitionActive &&
                        state.bridge.phase !in setOf(
                            BridgeLinkPhase.Preparing,
                            BridgeLinkPhase.Registering,
                            BridgeLinkPhase.Connecting,
                        )
                    ) {
                        // Periodic work only needs source-arbiter truth. The former full controller
                        // + Amiibo refresh kept this single-flight carrier almost permanently busy
                        // and could queue an unrelated `kbm` behind Controller Link startup.
                        runCatching {
                            ManagementDiagnosticContext.withWorkflow("background-input-poll") {
                                adapter.refreshInputSources()
                            }
                        }
                            .onSuccess { input ->
                                convergeActiveControllerIdentity(input.activeId)
                                if (_ui.value.bridge.phase == BridgeLinkPhase.Playing) {
                                    reconcileControllerLinkSource(input)
                                }
                            }
                            .onFailure { diagnostics.error("management", "background input poll", it) }
                    }
                }
            }
        }
        refreshPlatformDiagnostics()
        refreshSources()
    }

    /**
     * Compare the flashed firmware's bridge contract with the one this build
     * speaks, and log every transition.
     *
     * Logged rather than only shown, because the symptom this detects (battery,
     * motion and rumble all missing while buttons work) is one a user reports
     * without ever opening the diagnostics screen.
     */
    private fun refreshBridgeCompatibility() {
        val state = _ui.value
        val next = BridgeContract.evaluate(
            firmwareContract = state.snapshot.firmware.bridgeContract,
            connected = state.connection.connected,
            // The identity reply is what carries the contract. Until it lands we
            // have not asked yet, which is Pending -- not a finding about this
            // firmware. `version` is required by the protocol's own shape check,
            // so a non-blank version means the reply really did arrive.
            firmwareInfoAvailable = state.snapshot.firmware.version.isNotBlank(),
        )
        if (next == state.bridgeCompatibility) return
        _ui.update { it.copy(bridgeCompatibility = next) }
        // Pending is transient on every healthy connection; logging it is noise.
        if (next !is BridgeContract.Compatibility.NotConnected &&
            next !is BridgeContract.Compatibility.Pending
        ) {
            diagnostics.event("adapter", "bridge contract", next.summary)
        }
    }

    fun navigate(section: AppSection) {
        savedState[KEY_SECTION] = section.name
        _ui.update { it.copy(section = section, overlay = AppOverlay.None, message = null) }
        if (section == AppSection.Keyboard) ensureKbmMappings()
    }

    fun openOverlay(overlay: AppOverlay) {
        _ui.update { it.copy(overlay = overlay, message = null) }
    }

    fun closeOverlay() {
        _ui.update { it.copy(overlay = AppOverlay.None) }
    }
    fun setThemeMode(mode: ThemeMode) = updateTheme { it.copy(mode = mode) }
    fun setAccentPalette(palette: AccentPalette) = updateTheme { it.copy(palette = palette) }
    fun consumeMessage() { _ui.update { it.copy(message = null) } }
    fun selectAmiibo(id: String) {
        savedState[KEY_AMIIBO] = id
        _ui.update { it.copy(selectedAmiiboId = id) }
        refreshSelectedAmiiboDetails()
    }

    fun connect() = reconnectKnownAdapter(AdapterConnectReason.Manual)

    fun reconnectKnownAdapter(reason: AdapterConnectReason = AdapterConnectReason.Manual) {
        val relationship = relationshipStore.load() ?: return
        val decision = relationshipCoordinator.requestReconnect(relationship, reason, bondState(relationship.address))
        publishRelationshipStatus()
        executeLifecycleDecision(decision)
    }

    fun tryAutoReconnect() {
        if (autoReconnectAttempted || _ui.value.connection.connected || _ui.value.busy) return
        if (relationshipStore.load() == null) return
        autoReconnectAttempted = true
        reconnectKnownAdapter(AdapterConnectReason.ForegroundAuto)
    }

    fun beginForegroundSession() {
        autoReconnectAttempted = false
        tryAutoReconnect()
    }

    /** Discover one advertised management endpoint and feed its exact BLE device into the existing
     * generation coordinator before any bond or GATT decision is made. */
    @SuppressLint("MissingPermission")
    fun beginAdapterPairing(): Long {
        relationshipPairingJob?.cancel()
        relationshipConnectionJob?.cancel()
        pairingDevice = null
        val generation = relationshipCoordinator.beginAssociation()
        publishRelationshipStatus(ConnectionPhase.Associating)
        diagnostics.event("relationship", "pair.discovery_start", "attempt=$generation")
        if (relationshipRetirementJob?.isActive != true) {
            relationshipRetirementJob = viewModelScope.launch { runCatching { adapter.disconnect() } }
        }
        val retirement = relationshipRetirementJob
        relationshipPairingJob = viewModelScope.launch {
            try {
                retirement?.join()
                val peer = adapter.discoverForPairing(
                    ManagementConnectionContext(
                        logicalAttempt = generation,
                        reason = AdapterConnectReason.FirstPair.diagnosticName,
                        useDiscoveredPeer = true,
                    ),
                )
                val device = peer.device
                pairingDevice = PairingDevice(generation, device)
                val bond = device.bondState.toProductBondState()
                val relationship = AdapterRelationship(
                    address = device.address,
                    associationId = null,
                    displayName = peer.displayName.orEmpty().ifBlank { "PicoSwitch2" },
                )
                val decision = relationshipCoordinator.deviceDiscovered(generation, relationship, bond)
                publishRelationshipStatus()
                diagnostics.event(
                    "relationship",
                    "pair.device_discovered",
                    "attempt=$generation bond=$bond decision=${decision.javaClass.simpleName}",
                )
                when (decision) {
                    is AdapterLifecycleDecision.AwaitBond -> {
                        diagnostics.event(
                            "relationship",
                            if (decision.startBond) "bond.start" else "bond.wait",
                            "attempt=$generation bond=$bond",
                        )
                        if (!decision.startBond) {
                            awaitAdapterBond(generation, device, bond)
                        } else {
                            val started = AdapterBondStarter(androidBondPlatform(device)).start()
                            diagnostics.event(
                                "relationship", "bond.mechanism",
                                "attempt=$generation mechanism=${started.mechanism.diagnosticName} ${started.detail}",
                            )
                            when {
                                started.startedExplicitBond -> awaitAdapterBond(generation, device, bond)
                                // Compatibility path: Android owns no explicit LE bond call here, so
                                // the encrypted management GATT link provokes SMP instead. Ownership
                                // of the attempt moves to the connect job; the bond broadcast still
                                // arrives, just during the connect rather than before it.
                                started.delegatesToGatt -> {
                                    gattInitiatedBondGeneration = generation
                                    pairingDevice = null
                                    val delegated = relationshipCoordinator.bondDelegatedToGatt(generation)
                                    publishRelationshipStatus()
                                    executeLifecycleDecision(delegated)
                                }
                                else -> adapterBondStartFailed(device.address)
                            }
                        }
                    }
                    is AdapterLifecycleDecision.Connect -> {
                        if (bond == AndroidBondState.Bonded) notice("Adapter already paired")
                        pairingDevice = null
                        executeLifecycleDecision(decision)
                    }
                    else -> executeLifecycleDecision(decision)
                }
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                diagnostics.event("relationship", "pair.discovery_cancelled", "attempt=$generation")
                throw cancelled
            } catch (error: Throwable) {
                val message = "Couldn’t find the adapter. Make sure its pairing mode is active, then try again."
                relationshipCoordinator.associationFailed(generation, message)
                publishRelationshipStatus()
                diagnostics.error("relationship", "pair discovery attempt $generation", error)
                notice(message)
            } finally {
                if (relationshipPairingJob === coroutineContext[Job]) relationshipPairingJob = null
            }
        }
        return generation
    }

    fun adapterBondChanged(address: String, bond: AndroidBondState) {
        val decision = relationshipCoordinator.bondChanged(address, bond)
        publishRelationshipStatus()
        diagnostics.event(
            "relationship", "bond.state",
            "attempt=${relationshipCoordinator.status.generation} bond=$bond decision=${decision.javaClass.simpleName}",
        )
        if (decision is AdapterLifecycleDecision.Connect || decision is AdapterLifecycleDecision.RepairRequired) {
            pairingDevice = null
        }
        executeLifecycleDecision(decision)
    }

    /** Recover an authoritative bond completion that occurred while the Activity receiver was stopped. */
    @SuppressLint("MissingPermission")
    fun resumePendingAdapterBond() {
        if (relationshipCoordinator.status.phase != AdapterRelationshipPhase.Bonding) return
        val candidate = pairingDevice?.takeIf { it.generation == relationshipCoordinator.status.generation } ?: return
        val device = candidate.device
        adapterBondChanged(device.address, device.bondState.toProductBondState())
    }

    fun adapterBondStartFailed(address: String) {
        adapterBondChanged(address, AndroidBondState.None)
    }

    /**
     * The one and only compatibility seam that touches a non-SDK Bluetooth entry point.
     *
     * `BluetoothDevice.createBond(int transport)` is absent from `android.jar` through API 36 and
     * public from API 37, so on every currently shipping Android the transport-specific bond this
     * product requires can only be reached by name. `getMethod` IS the runtime feature detection:
     * a platform that hides or blocks it throws, this returns null, and [AdapterBondStarter] routes
     * to the public GATT-initiated LE path instead. On API 37+ the very same method is resolved,
     * now as public API, so this seam becomes a plain call without a behaviour change.
     *
     * Nothing else in the app uses hidden API. TRANSPORT_AUTO is deliberately not offered here.
     */
    @SuppressLint("MissingPermission")
    private fun androidBondPlatform(device: BluetoothDevice) = object : AdapterBondStarter.Platform {
        override fun createBondOnLe(): Boolean? = runCatching {
            BluetoothDevice::class.java
                .getMethod("createBond", Int::class.javaPrimitiveType)
                .invoke(device, BluetoothDevice.TRANSPORT_LE) as? Boolean
        }.getOrNull()

        override fun cachedDeviceTypeName(): String = when (device.type) {
            BluetoothDevice.DEVICE_TYPE_CLASSIC -> "classic"
            BluetoothDevice.DEVICE_TYPE_LE -> "le"
            BluetoothDevice.DEVICE_TYPE_DUAL -> "dual"
            else -> "unknown"
        }
    }

    /**
     * Bluetooth's bond broadcast is advisory wake-up here, not the sole progression mechanism.
     * Some Android builds deliver it from a privileged package that an Activity receiver can miss;
     * retaining and sampling the exact scan-result device keeps bond and GATT in one generation.
     */
    @SuppressLint("MissingPermission")
    private suspend fun awaitAdapterBond(
        generation: Long,
        device: BluetoothDevice,
        initialBond: AndroidBondState,
    ) {
        val policy = AdapterBondWaitPolicy(initialBond)
        val completed = withTimeoutOrNull<Boolean>(ADAPTER_BOND_TIMEOUT_MS) {
            while (true) {
                val candidate = pairingDevice
                if (candidate == null || candidate.generation != generation ||
                    relationshipCoordinator.status.generation != generation ||
                    relationshipCoordinator.status.phase != AdapterRelationshipPhase.Bonding
                ) return@withTimeoutOrNull true

                val state = runCatching { device.bondState.toProductBondState() }
                    .getOrDefault(AndroidBondState.Unknown)
                when (policy.observe(state)) {
                    AdapterBondWaitOutcome.Bonded -> {
                        adapterBondChanged(device.address, AndroidBondState.Bonded)
                        return@withTimeoutOrNull true
                    }
                    AdapterBondWaitOutcome.Rejected -> {
                        adapterBondChanged(device.address, AndroidBondState.None)
                        return@withTimeoutOrNull true
                    }
                    AdapterBondWaitOutcome.Continue -> delay(ADAPTER_BOND_POLL_MS)
                }
            }
            @Suppress("UNREACHABLE_CODE")
            false
        } ?: false

        if (!completed && pairingDevice?.generation == generation &&
            relationshipCoordinator.status.generation == generation &&
            relationshipCoordinator.status.phase == AdapterRelationshipPhase.Bonding
        ) {
            diagnostics.event("relationship", "bond.timeout", "attempt=$generation")
            adapterBondChanged(device.address, AndroidBondState.None)
        }
    }

    fun systemAssociationQueryFailed(error: Throwable) {
        // CDM is optional metadata. Its absence or an OEM service failure cannot block startup,
        // BLE discovery, bonding, or management GATT.
        diagnostics.error("relationship", "association.query_optional", error)
    }

    fun reconcileAdapterRelationships(associations: List<SystemCompanionAssociation>) {
        if (relationshipCoordinator.status.attemptActive) {
            diagnostics.event("relationship", "association.reconcile", "deferred while attempt is active")
            return
        }
        // HID-device hosts are consoles paired to the phone's Controller Bridge, not management
        // adapters. Never use that unrelated Bluetooth truth to reconstruct this relationship.
        val result = AdapterRelationshipReconciler.reconcile(relationshipStore.load(), associations)
        result.relationship?.let(relationshipStore::save)
        relationshipCoordinator.restore(
            result.relationship,
            result.associationState,
            result.relationship?.let { bondState(it.address) } ?: AndroidBondState.Unknown,
        )
        _ui.update { it.copy(adapterRelationship = result.relationship, relationshipStatus = relationshipCoordinator.status) }
        diagnostics.event(
            "relationship", "association.reconciled",
            "source=${result.source} records=${associations.size} association=${result.relationship?.associationId ?: "none"} state=${result.associationState}",
        )
    }

    fun forgetAdapterRelationship() {
        // Historical builds created CDM records only through a PicoSwitch2 name-filtered chooser.
        // Explicit Forget is the safe lifecycle point to remove those stale app-owned records as
        // well as the selected relationship, while retaining platform/Pico bonds.
        clearOwnedRelationship(relationshipStore.load(), removeAllCompanionAssociations = true)
    }

    fun prepareRepairPairing(onReady: (needsAndroidSettings: Boolean) -> Unit) {
        val relationship = relationshipStore.load()
        val ambiguousAssociations = relationshipCoordinator.status.companionAssociation == CompanionAssociationState.Ambiguous
        val needsSettings = relationship?.let { bondState(it.address) == AndroidBondState.Bonded } == true
        diagnostics.event(
            "relationship",
            "repair.started",
            "association=${relationship?.associationId ?: "none"} ambiguous=$ambiguousAssociations platformBond=$needsSettings",
        )
        clearOwnedRelationship(
            relationship,
            removeAllCompanionAssociations = ambiguousAssociations,
            onComplete = { onReady(needsSettings) },
        )
    }

    fun disconnect() {
        relationshipPairingJob?.cancel()
        relationshipConnectionJob?.cancel()
        pairingDevice = null
        relationshipCoordinator.cancelAndRetainRelationship()
        publishRelationshipStatus(ConnectionPhase.Disconnecting)
        diagnostics.event("relationship", "disconnect.user", "management only")
        if (relationshipRetirementJob?.isActive != true) {
            relationshipRetirementJob = viewModelScope.launch {
                runCatching { adapter.disconnect() }
                    .onFailure { diagnostics.error("management", "disconnect.user", it) }
                publishRelationshipStatus(ConnectionPhase.Idle)
            }
        }
    }
    fun refresh() = launch("Refreshing adapter") { adapter.refreshAll() }
    // Report what the adapter actually did, never merely that the command was
    // transmitted. Each outcome is distinct and actionable: "advertised" is the
    // only one that means a wake was really attempted on the radio, and even that
    // does not promise the console obeyed, so it is not phrased as success.
    fun wake() = launch("Requesting console wake") {
        when (adapter.wakeConsole().result) {
            WakeResult.Advertised ->
                notice("Wake signal broadcast. If the console stays asleep, press a button on a paired controller.")
            WakeResult.ConsoleAwake ->
                notice("Console is already awake.")
            WakeResult.NoIdentity ->
                notice("Cannot wake: the adapter has no saved console pairing. Connect it to the console once while the console is on.")
            WakeResult.RadioBusy ->
                notice("Wake could not run: the adapter's radio was busy. Try again in a moment.")
            WakeResult.Pending ->
                notice("Wake request sent; the adapter did not report an outcome.")
            WakeResult.Unknown ->
                notice("Wake request sent. This adapter firmware cannot report whether it ran.")
        }
    }

    fun switchPersonality(personality: Personality) = launch("Switching adapter mode") {
        personalityTransitionActive = true
        ManagementDiagnosticContext.setPersonalityPhase("mutation")
        try {
            val reenumerating = adapter.setPersonality(personality)
            if (!reenumerating) {
                notice("Already using ${personality.title}")
                return@launch
            }
            markIdentityRefreshPending("personality switch")
            ManagementDiagnosticContext.setPersonalityPhase("readback")
            val confirmedOnCurrentGatt = confirmPersonalityOnCurrentGatt(personality)

            // Source review shows USB re-enumeration should not tear down Bluetooth. If an OEM or
            // runtime transition nevertheless does, make it an expected, owned generation change
            // and prove identity + requested mode again before reporting success.
            val disconnectedDuringTransition = if (confirmedOnCurrentGatt) {
                withTimeoutOrNull(PERSONALITY_DISCONNECT_GRACE_MS) {
                    adapter.connection.first { !it.connected }
                    true
                } ?: false
            } else !adapter.connection.value.connected

            if (disconnectedDuringTransition) {
                recoverManagementAfterPersonality(personality)
            } else if (!confirmedOnCurrentGatt) {
                throw dev.picoswitch.companion.protocol.ManagementException(
                    "Adapter mode changed, but management readback did not complete",
                )
            }
            savedState[KEY_IDENTITY_PENDING] = false
            _ui.update { it.copy(identityRefreshPending = false) }
            notice("Mode changed to ${personality.title}. USB re-enumerated and management is ready.")
        } finally {
            personalityTransitionActive = false
            ManagementDiagnosticContext.setPersonalityPhase("idle")
        }
    }

    private suspend fun confirmPersonalityOnCurrentGatt(expected: Personality): Boolean {
        repeat(PERSONALITY_READBACK_ATTEMPTS) { attempt ->
            if (!adapter.connection.value.connected) return false
            val observed = runCatching { adapter.refreshPersonality() }.getOrNull()
            if (observed == expected) {
                diagnostics.event("adapter", "personality.confirmed", "mode=${expected.wireName} attempt=${attempt + 1}")
                return true
            }
            delay(PERSONALITY_READBACK_RETRY_MS)
        }
        return false
    }

    private suspend fun recoverManagementAfterPersonality(expected: Personality) {
        ManagementDiagnosticContext.setPersonalityPhase("reconnect")
        val ready = withTimeoutOrNull(PERSONALITY_RECONNECT_TIMEOUT_MS) {
            while (relationshipCoordinator.status.attemptActive ||
                relationshipCoordinator.status.phase == AdapterRelationshipPhase.Connected
            ) {
                delay(PERSONALITY_READBACK_RETRY_MS)
            }
            reconnectKnownAdapter(AdapterConnectReason.AfterPersonality)
            adapter.connection.first { it.connected }
            true
        } ?: false
        if (!ready) {
            throw dev.picoswitch.companion.protocol.ManagementException(
                "Mode changed, but management could not reconnect",
            )
        }
        ManagementDiagnosticContext.setPersonalityPhase("reconnected-readback")
        if (adapter.refreshPersonality() != expected) {
            throw dev.picoswitch.companion.protocol.ManagementException(
                "Management reconnected, but the requested adapter mode was not active",
            )
        }
        diagnostics.event("adapter", "personality.recovered", "mode=${expected.wireName}")
    }

    fun saveColor(target: ColorTarget, color: RgbColor) = launch("Saving color") {
        val persistenceVerified = adapter.setColor(target, color)
        markIdentityRefreshPending("identity color saved")
        runCatching { adapter.reenumerateUsb() }
            .onSuccess {
                savedState[KEY_IDENTITY_PENDING] = false
                _ui.update { it.copy(identityRefreshPending = false) }
                diagnostics.event(
                    "adapter", "identity changes applied",
                    "color persistence=${if (persistenceVerified) "verified" else "legacy-accepted"}",
                )
                notice("Color saved and applied. The console controller may pause briefly while USB reconnects.")
            }
            .onFailure { error ->
                diagnostics.error("adapter", "automatic identity refresh", error)
                notice("Color saved; USB identity refresh still needs to be applied.")
            }
    }

    fun clearIdentityRefreshPending() {
        savedState[KEY_IDENTITY_PENDING] = false
        _ui.update { it.copy(identityRefreshPending = false) }
        diagnostics.event("adapter", "identity refresh acknowledged")
    }

    fun applyIdentityChanges() = launch("Applying identity changes") {
        adapter.reenumerateUsb()
        savedState[KEY_IDENTITY_PENDING] = false
        _ui.update { it.copy(identityRefreshPending = false) }
        diagnostics.event("adapter", "identity changes applied")
        notice("USB identity refreshed. The console controller may pause briefly while it reconnects.")
    }

    // -----------------------------------------------------------------------
    // Keyboard & Mouse
    // -----------------------------------------------------------------------
    // These run outside [launch] on purpose. That helper raises the modal
    // progress overlay and refuses to start while any other operation is in
    // flight, which is right for uploading an Amiibo and wrong for dragging a
    // sensitivity slider: the page has to stay interactive and a rejected
    // command must not simply vanish.

    /**
     * Fetch the binding lists the page needs, once per connection.
     *
     * Each profile costs several paginated round trips, so this is not part of
     * the ordinary refresh; it runs when the page is first opened and after a
     * reconnect (the flag is cleared with the rest of the session state).
     */
    fun ensureKbmMappings(force: Boolean = false) {
        if (!_ui.value.connection.connected) return
        if (_ui.value.kbm.available == CapabilityState.Unsupported) return
        if (kbmMappingsLoaded && !force) return
        kbmMappingsLoaded = true
        kbmOperation("Reading keyboard mapping") {
            if (_ui.value.kbm.available != CapabilityState.Available) adapter.refreshKbm()
            KbmProfile.entries.forEach { adapter.loadKbmMapping(it) }
        }
    }

    fun refreshKbm() = kbmOperation("Refreshing keyboard and mouse") {
        adapter.refreshKbm()
        KbmProfile.entries.forEach { adapter.loadKbmMapping(it) }
    }

    fun setKbmMode(mode: KbmMode) = kbmOperation("Changing input mode") {
        adapter.setKbmMode(mode)
    }

    /** `null` destination restores this input's canonical binding. */
    fun bindKbm(profile: KbmProfile, source: KbmSource, destination: KbmDestination?) =
        kbmOperation("Updating binding") {
            adapter.bindKbm(profile, source, destination)
        }

    fun resetKbmProfile(profile: KbmProfile) = kbmOperation("Restoring ${profile.title} defaults") {
        adapter.resetKbmProfile(profile)
        notice("${profile.title} mapping restored to defaults")
    }

    fun resetKbmAll() = kbmOperation("Restoring keyboard and mouse defaults") {
        adapter.resetKbmAll()
        notice("Keyboard, mouse, and mapping defaults restored")
    }

    /**
     * Apply a mouse setting while the control is still being dragged.
     *
     * Debounced rather than sent per pixel: the management link carries one
     * command at a time, and a slider produces far more values than it can
     * absorb. The delay is short enough that the change still reads as live.
     * [commitMouseField] then sends the released value unconditionally so the
     * adapter always ends on exactly what the user chose.
     */
    fun previewMouseField(field: KbmMouseField, value: Int) {
        kbmMouseJob?.cancel()
        kbmMouseJob = viewModelScope.launch {
            delay(MOUSE_APPLY_DEBOUNCE_MS)
            runCatching {
                ManagementDiagnosticContext.withWorkflow("mouse-${field.wire}-preview") {
                    adapter.setKbmMouse(field, value)
                }
            }
                .onFailure { diagnostics.error("kbm", "mouse ${field.wire} preview", it) }
        }
    }

    fun commitMouseField(field: KbmMouseField, value: Int) {
        kbmMouseJob?.cancel()
        kbmMouseJob = viewModelScope.launch {
            _ui.update { it.copy(kbmBusy = true) }
            runCatching {
                ManagementDiagnosticContext.withWorkflow("mouse-${field.wire}-commit") {
                    adapter.setKbmMouse(field, value)
                }
            }
                .onFailure { error ->
                    diagnostics.error("kbm", "mouse ${field.wire}", error)
                    notice(error.message ?: "The adapter rejected that value")
                    // The adapter is the authority on what is in effect; re-read
                    // rather than leaving the control showing a value it refused.
                    runCatching {
                        ManagementDiagnosticContext.withWorkflow("mouse-recovery-readback") {
                            adapter.refreshKbm()
                        }
                    }
                }
            _ui.update { it.copy(kbmBusy = false) }
        }
    }

    fun saveAdapterConfiguration() = kbmOperation("Saving to adapter") {
        adapter.saveConfiguration()
        notice("Saved to adapter")
    }

    private fun kbmOperation(label: String, action: suspend () -> Unit): Job? {
        if (_ui.value.kbmBusy) return null
        return viewModelScope.launch {
            _ui.update { it.copy(kbmBusy = true) }
            runCatching {
                ManagementDiagnosticContext.withWorkflow(label) { action() }
            }.onFailure { error ->
                diagnostics.error("kbm", label, error)
                notice(error.message ?: "$label failed")
            }
            _ui.update { it.copy(kbmBusy = false) }
        }
    }

    fun setManagement(enabled: Boolean) = launch("Updating management access") {
        adapter.setManagementEnabled(enabled)
        notice(if (enabled) "Wireless management enabled for this boot" else "Wireless management disabled; this connection may close")
    }

    fun setActiveInput(sourceId: Long) = launch("Switching active controller") {
        adapter.setActiveInput(sourceId)
        // Ownership handover neutralizes the console slot until the new owner's first fresh
        // report, so this first read often returns no identity. That is the transitional state,
        // not the answer; the background poll converges it as soon as the report lands.
        convergeActiveControllerIdentity(sourceId)
        val source = _ui.value.snapshot.input.sources.firstOrNull { it.id == sourceId }
        notice(if (sourceId == 0L) "Console input paused" else "Active controller switched to ${source?.name ?: "selected source"}")
    }

    /**
     * Keep the Adapter page's Controller row on the adapter's canonical slot-0 identity.
     *
     * Nothing outside the manual Refresh button ever re-read that identity, so switching the
     * active console input left the row showing whatever the last Refresh happened to see
     * (hardware-confirmed 2026-08-21). This is the only place that re-reads it, the adapter stays
     * the single source of truth, and [ActiveControllerIdentityPolicy] bounds how often it asks.
     */
    private suspend fun convergeActiveControllerIdentity(activeSourceId: Long) {
        if (!activeIdentityPolicy.shouldRefresh(activeSourceId)) return
        val identity = runCatching {
            ManagementDiagnosticContext.withWorkflow("active-identity-converge") {
                adapter.refreshControllerIdentity()
            }
        }.getOrElse {
            diagnostics.error("controller", "active identity refresh", it)
            return
        }
        activeIdentityPolicy.identityRead(activeSourceId, identity.attached)
        diagnostics.event(
            "controller", "identity.observed",
            "active=$activeSourceId attached=${identity.attached}",
        )
    }

    /**
     * Hold or release a console button that the handheld has no physical key for.
     *
     * Not routed through [launch]: this is live input on the HID report path, so it
     * must not queue behind a management command or be blocked by the busy flag.
     */
    fun setConsoleButton(button: ControllerButton, pressed: Boolean) {
        inputBackend.setVirtualButton(button, pressed)
    }

    fun importAmiibo(uri: Uri, displayName: String) = launch("Importing Amiibo") {
        val resolver = getApplication<Application>().contentResolver
        resolver.openAssetFileDescriptor(uri, "r")?.use { descriptor ->
            if (descriptor.length > MAX_IMPORT_BYTES) error("Selected file is too large to be an Amiibo backup")
        }
        val bytes = resolver.openInputStream(uri)?.use { stream ->
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(1024)
            while (true) {
                val count = stream.read(buffer)
                if (count < 0) break
                if (output.size() + count > MAX_IMPORT_BYTES) error("Selected file is too large to be an Amiibo backup")
                output.write(buffer, 0, count)
            }
            output.toByteArray()
        } ?: error("Could not read selected file")
        val name = uri.lastPathSegment?.substringAfterLast('/') ?: "amiibo.bin"
        val result = library.import(displayName, name, bytes)
        _ui.update { it.copy(selectedAmiiboId = result.item.id, section = AppSection.Amiibo) }
        savedState[KEY_AMIIBO] = result.item.id
        refreshSelectedAmiiboDetails()
        notice(if (result.duplicate) "That exact backup is already in the library" else "Imported ${result.item.displayName}")
    }

    fun exportAmiiboArchive(uri: Uri) = launch("Exporting Amiibo library") {
        val resolver = getApplication<Application>().contentResolver
        val bytes = library.exportArchive(_ui.value.selectedAmiiboId)
        resolver.openOutputStream(uri)?.use { stream ->
            stream.write(bytes)
            stream.flush()
        } ?: error("Could not create Amiibo library archive")
        notice("Exported ${_ui.value.library.size} Amiibo backups as a private ZIP")
    }

    fun importAmiiboArchive(uri: Uri) = launch("Importing Amiibo library") {
        val resolver = getApplication<Application>().contentResolver
        resolver.openAssetFileDescriptor(uri, "r")?.use { descriptor ->
            if (descriptor.length > MAX_LIBRARY_ARCHIVE_BYTES)
                error("Selected library archive is too large")
        }
        val bytes = resolver.openInputStream(uri)?.use { stream ->
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (true) {
                val count = stream.read(buffer)
                if (count < 0) break
                if (output.size() + count > MAX_LIBRARY_ARCHIVE_BYTES)
                    error("Selected library archive is too large")
                output.write(buffer, 0, count)
            }
            output.toByteArray()
        } ?: error("Could not read Amiibo library archive")
        val result = library.importArchive(bytes)
        val selected = result.selectedId ?: result.items.firstOrNull()?.id
        _ui.update { it.copy(selectedAmiiboId = selected, section = AppSection.Amiibo) }
        savedState[KEY_AMIIBO] = selected
        refreshSelectedAmiiboDetails()
        notice("Imported ${result.items.size} Amiibo backups from a private ZIP")
    }

    fun setNfcReaderAvailable(available: Boolean) {
        _ui.update {
            it.copy(
                nfcScan = if (available) {
                    NfcScanStatus(NfcScanPhase.Idle, "Tap Scan tag, then hold an ordinary Amiibo to the back of this phone.")
                } else {
                    NfcScanStatus(NfcScanPhase.Unavailable, "This phone does not expose an NFC-A reader.")
                },
            )
        }
    }

    fun armNfcScan() {
        if (_ui.value.nfcScan.phase == NfcScanPhase.Unavailable || _ui.value.busy) return
        _ui.update {
            it.copy(nfcScan = NfcScanStatus(NfcScanPhase.Armed, "Hold an ordinary NTAG215 Amiibo to the back of this phone."))
        }
    }

    fun nfcScanReading() {
        _ui.update { it.copy(nfcScan = NfcScanStatus(NfcScanPhase.Reading, "Reading the complete NTAG215 image…")) }
    }

    fun nfcScanDisarmed() {
        _ui.update { state ->
            if (state.nfcScan.phase == NfcScanPhase.Armed || state.nfcScan.phase == NfcScanPhase.Reading) {
                state.copy(nfcScan = NfcScanStatus(NfcScanPhase.Idle, "NFC scan canceled when the app left the foreground."))
            } else state
        }
    }

    fun nfcReaderUnavailable(message: String) {
        _ui.update { it.copy(nfcScan = NfcScanStatus(NfcScanPhase.Unavailable, message)) }
        notice(message)
    }

    fun nfcReaderError(message: String) {
        _ui.update { it.copy(nfcScan = NfcScanStatus(NfcScanPhase.Rejected, message)) }
        notice(message)
    }

    fun nfcScanRejected(reason: Ntag215Rejection) {
        val message = "NFC scan rejected: ${reason.description}"
        _ui.update { it.copy(nfcScan = NfcScanStatus(NfcScanPhase.Rejected, message)) }
        notice(message)
    }

    /** Persist only a complete, already strict-validated phone NFC result. */
    fun importNfcAmiibo(result: Ntag215ReadResult.Success) {
        if (_ui.value.busy) {
            nfcScanRejected(Ntag215Rejection.TRANSPORT_ERROR)
            return
        }
        viewModelScope.launch {
            _ui.update {
                it.copy(
                    busy = true,
                    operation = OperationProgress("Saving phone NFC backup", 0, result.bytes.size),
                    nfcScan = NfcScanStatus(NfcScanPhase.Saving, "Saving the complete ${result.bytes.size}-byte backup…"),
                    message = null,
                )
            }
            try {
                // Keep this guard immediately before the library call.  The
                // library also serves file imports, and its compatibility
                // normalizer must never get a chance to repair a phone read.
                val rejection = Ntag215Protocol.validateImage(result.bytes)
                require(rejection == null) { rejection?.description ?: "Invalid NTAG215 image" }
                val imported = library.import("Phone NFC Amiibo", "phone NFC", result.bytes)
                _ui.update {
                    it.copy(
                        selectedAmiiboId = imported.item.id,
                        section = AppSection.Amiibo,
                        operation = OperationProgress("Saving phone NFC backup", result.bytes.size, result.bytes.size),
                        nfcScan = NfcScanStatus(
                            NfcScanPhase.Saved,
                            if (imported.duplicate) {
                                "That exact ${result.bytes.size}-byte phone NFC backup is already in the library."
                            } else {
                                "Saved ${result.bytes.size}-byte phone NFC backup${if (result.signatureIncluded) " with READ_SIG" else " without READ_SIG"}."
                            },
                        ),
                    )
                }
                savedState[KEY_AMIIBO] = imported.item.id
                refreshSelectedAmiiboDetails()
                notice(_ui.value.nfcScan.message)
            } catch (error: Throwable) {
                val message = error.message?.take(180) ?: "Could not save the NFC backup"
                _ui.update { it.copy(nfcScan = NfcScanStatus(NfcScanPhase.Rejected, "NFC backup was not saved: $message")) }
                notice("NFC backup was not saved: $message")
            } finally {
                _ui.update { it.copy(busy = false, operation = null) }
            }
    }
    }

    fun importAmiiboKeys(uri: Uri) = launch("Importing Amiibo keys") {
        val resolver = getApplication<Application>().contentResolver
        resolver.openAssetFileDescriptor(uri, "r")?.use { descriptor ->
            if (descriptor.length > RETAIL_KEY_BYTES) error("key_retail.bin must be exactly 160 bytes")
        }
        val bytes = resolver.openInputStream(uri)?.use { stream ->
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(256)
            while (true) {
                val count = stream.read(buffer)
                if (count < 0) break
                if (output.size() + count > RETAIL_KEY_BYTES) error("key_retail.bin must be exactly 160 bytes")
                output.write(buffer, 0, count)
            }
            output.toByteArray()
        } ?: error("Could not read key_retail.bin")
        amiiboKeyStore.import(bytes)
        _ui.update { it.copy(amiiboKeysLoaded = true) }
        refreshSelectedAmiiboDetails()
        notice("Amiibo keys imported to this phone only; they are never sent to the adapter or included in diagnostics.")
    }

    fun loadSelectedAmiibo() = launch("Uploading Amiibo") {
        val id = requireNotNull(_ui.value.selectedAmiiboId) { "Select an Amiibo first" }
        adapter.uploadAmiibo(library.bytes(id)) { progress -> _ui.update { it.copy(operation = progress) } }
        notice("Amiibo loaded and saved on the adapter")
    }

    fun initializeSelectedAmiibo() = launch("Initializing Amiibo") {
        val id = requireNotNull(_ui.value.selectedAmiiboId) { "Select an Amiibo first" }
        val keys = amiiboKeyStore.read() ?: error("Import your own key_retail.bin before initializing an Amiibo")
        val original = library.bytes(id)
        val initialized = AmiiboCrypto.initialize(original, keys)
        // updateFromAdapter is the same atomic file/index replacement used by
        // Sync, but this path never sends the result to the adapter.
        val item = library.updateFromAdapter(id, initialized)
        _ui.update { it.copy(selectedAmiiboId = item.id, amiiboKeysLoaded = true) }
        savedState[KEY_AMIIBO] = item.id
        refreshSelectedAmiiboDetails()
        notice("${item.displayName} initialized locally; the adapter was not changed")
    }

    fun syncSelectedAmiibo() = launch("Syncing Amiibo") {
        val download = adapter.downloadAmiibo { progress -> _ui.update { it.copy(operation = progress) } }
        // Dirty protection is acknowledged only after the private file and index are durable.
        val item = library.updateFromAdapter(_ui.value.selectedAmiiboId, download.bytes)
        adapter.acknowledgeDownloadedAmiibo(download)
        _ui.update { it.copy(selectedAmiiboId = item.id) }
        savedState[KEY_AMIIBO] = item.id
        refreshSelectedAmiiboDetails()
        notice("Synced console-written data into ${item.displayName}")
    }

    fun setPresented(value: Boolean) = launch(if (value) "Presenting Amiibo" else "Ejecting Amiibo") { adapter.setPresented(value) }
    fun selectCopy(used: Boolean) = launch("Selecting Amiibo copy") { adapter.selectAmiiboCopy(used) }
    fun clearAdapterAmiibo() = launch("Clearing adapter Amiibo") { adapter.clearAmiibo(); notice("Adapter Amiibo cleared") }
    fun deleteSelectedAmiibo() = launch("Deleting local Amiibo") {
        val id = _ui.value.selectedAmiiboId ?: return@launch
        library.delete(id)
        notice("Local backup deleted; the adapter was not changed")
    }

    fun renameSelectedAmiibo(name: String) = launch("Renaming local Amiibo") {
        val id = _ui.value.selectedAmiiboId ?: return@launch
        library.rename(id, name)
    }

    private fun refreshSelectedAmiiboDetails() {
        selectedDetailsJob?.cancel()
        val id = _ui.value.selectedAmiiboId
        if (id == null) {
            _ui.update { it.copy(selectedAmiiboDetails = null, selectedAmiiboCatalog = null, selectedAmiiboTitleGame = null, amiiboCatalogLoading = false) }
            return
        }
        selectedDetailsJob = viewModelScope.launch {
            val bytes = runCatching { library.bytes(id) }.getOrNull()
            val keys = amiiboKeyStore.read()
            val details = bytes?.let { runCatching { AmiiboCrypto.readDetails(it, keys) }.getOrNull() }
            val libraryItem = _ui.value.library.firstOrNull { it.id == id }
            val figureId = details?.figureId ?: libraryItem?.figureId.orEmpty()
            val cachedCatalog = figureId.takeIf { it.isNotBlank() }?.let(amiiboCatalog::find)
            val cachedTitleGame = details?.titleId?.let(amiiboCatalog::gameNameForTitleId)
            _ui.update { state ->
                if (state.selectedAmiiboId == id) state.copy(
                    amiiboCatalogEntries = catalogEntriesFor(state.library),
                    selectedAmiiboDetails = details,
                    selectedAmiiboCatalog = cachedCatalog,
                    selectedAmiiboTitleGame = cachedTitleGame,
                    amiiboCatalogLoading = cachedCatalog == null,
                ) else state
            }
            if (libraryItem != null && cachedCatalog == null) {
                amiiboCatalog.ensureLoaded()
                val refreshedCatalog = figureId.takeIf { it.isNotBlank() }?.let(amiiboCatalog::find)
                val refreshedTitleGame = details?.titleId?.let(amiiboCatalog::gameNameForTitleId)
                _ui.update { state ->
                    if (state.selectedAmiiboId == id) state.copy(
                        amiiboCatalogEntries = catalogEntriesFor(state.library),
                        selectedAmiiboCatalog = refreshedCatalog,
                        selectedAmiiboTitleGame = refreshedTitleGame,
                        amiiboCatalogLoading = false,
                    ) else state
                }
                refreshAdapterAmiiboCatalogFromCache()
            }
        }
    }

    private fun catalogEntriesFor(items: List<AmiiboLibraryItem>): Map<String, AmiiboCatalogEntry> =
        items.mapNotNull { item -> amiiboCatalog.find(item.figureId)?.let { item.id to it } }.toMap()

    /**
     * Adapter state is not a local-library row. Keep its figure ID as a first
     * class catalog key so a tag loaded by the adapter can render its friendly
     * identity before the user chooses to download a phone backup.
     */
    private fun refreshAdapterAmiiboCatalog(status: AmiiboStatus) {
        val loaded = status.loaded || status.v3Loaded
        val figureId = normalizeFigureId(status.figureId)
        if (!loaded || figureId == null) {
            adapterCatalogJob?.cancel()
            adapterCatalogJob = null
            adapterCatalogFigureId = null
            _ui.update {
                it.copy(
                    adapterAmiiboCatalog = null,
                    adapterAmiiboCatalogState = if (loaded) AmiiboCatalogState.Unmatched else AmiiboCatalogState.Idle,
                )
            }
            return
        }
        if (figureId == adapterCatalogFigureId &&
            (_ui.value.adapterAmiiboCatalog != null || adapterCatalogJob?.isActive == true ||
                _ui.value.adapterAmiiboCatalogState == AmiiboCatalogState.Unmatched ||
                _ui.value.adapterAmiiboCatalogState == AmiiboCatalogState.Offline)
        ) return

        adapterCatalogFigureId = figureId
        adapterCatalogJob?.cancel()
        val cached = amiiboCatalog.find(figureId)
        _ui.update { state ->
            state.copy(
                amiiboCatalogEntries = catalogEntriesFor(state.library),
                adapterAmiiboCatalog = cached,
                adapterAmiiboCatalogState = if (cached != null) AmiiboCatalogState.Available else AmiiboCatalogState.Loading,
            )
        }
        if (cached != null) return
        adapterCatalogJob = viewModelScope.launch {
            val catalogAvailable = amiiboCatalog.ensureLoaded()
            val refreshed = amiiboCatalog.find(figureId)
            val lookupState = resolveAmiiboCatalogState(refreshed != null, catalogAvailable)
            _ui.update { state ->
                val currentId = normalizeFigureId(state.snapshot.amiibo.figureId)
                if (currentId == figureId && (state.snapshot.amiibo.loaded || state.snapshot.amiibo.v3Loaded)) {
                    state.copy(
                        amiiboCatalogEntries = catalogEntriesFor(state.library),
                        adapterAmiiboCatalog = refreshed,
                        adapterAmiiboCatalogState = lookupState,
                    )
                } else state
            }
        }
    }

    private fun refreshAdapterAmiiboCatalogFromCache() {
        val state = _ui.value
        val status = state.snapshot.amiibo
        val figureId = normalizeFigureId(status.figureId) ?: return
        if (!status.loaded && !status.v3Loaded) return
        if (figureId != adapterCatalogFigureId) return
        val catalog = amiiboCatalog.find(figureId) ?: return
        _ui.update {
            it.copy(
                amiiboCatalogEntries = catalogEntriesFor(it.library),
                adapterAmiiboCatalog = catalog,
                adapterAmiiboCatalogState = AmiiboCatalogState.Available,
            )
        }
    }

    private fun normalizeFigureId(value: String): String? = value.trim().uppercase().takeIf { FIGURE_ID.matches(it) }

    fun removeBond(index: Int) = launch("Removing management bond") {
        // Removing a bond may revoke THIS phone's own authorization. Reconcile
        // the connection state immediately rather than leaving a stale
        // "Connected" UI behind the relationship that permitted it.
        val sessionSurvived = adapter.removeBond(index)
        if (sessionSurvived) {
            notice("Management bond removed")
        } else {
            adapter.disconnect()
            diagnostics.event("adapter", "bond removal ended this session")
            notice("Bond removed. This phone's access ended, so the adapter disconnected. Pair again to reconnect.")
        }
    }

    fun refreshSources() {
        val devices = inputBackend.eligibleDevices()
        val desired = savedState.get<String>(KEY_SOURCE)
        // Resolve the source without making the user choose the only option.
        // Priority: an existing valid selection, then the saved preference, then
        // the single usable controller. With two or more, nothing is guessed and
        // the picker becomes visible instead.
        val candidates = inputBackend.candidateDevices().map { it.second }
        val current = inputBackend.selectedDescriptor
        val preferred = ControllerCandidates.resolveSelection(candidates, current ?: desired)
        if (preferred?.descriptor != current) {
            // One call: input selection and output binding must never disagree.
            bridge.selectSource(preferred?.descriptor)
            val chosen = inputBackend.selectedSource
            if (chosen != null && current == null) {
                diagnostics.event("controller", "auto-selected", chosen.name)
            }
        }
        _ui.update { state ->
            state.copy(
                sourceDevices = devices.map { SourceDeviceUi(it.id, it.descriptor, it.name.take(120), it.vendorId, it.productId) },
                // Selection UI only earns its space when there is a real choice.
                sourceChoiceRequired = ControllerCandidates.needsUserChoice(candidates),
                excludedSources = ControllerCandidates.excluded(candidates)
                    .map { ExcludedSourceUi(it.name, it.vendorId, it.productId, it.exclusionReason.orEmpty()) },
                selectedSourceDescriptor = inputBackend.selectedDescriptor,
                requestedFaceLayout = inputBackend.requestedFaceLayout,
                resolvedFaceLayout = inputBackend.resolvedFaceLayout,
            )
        }
    }

    fun selectSource(descriptor: String) {
        bridge.selectSource(descriptor)
        savedState[KEY_SOURCE] = inputBackend.selectedDescriptor
        _ui.update {
            it.copy(
                selectedSourceDescriptor = inputBackend.selectedDescriptor,
                requestedFaceLayout = inputBackend.requestedFaceLayout,
                resolvedFaceLayout = inputBackend.resolvedFaceLayout,
            )
        }
    }

    fun setControllerFaceLayout(layout: ControllerFaceLayout) {
        if (inputBackend.selectedDescriptor == null) return
        bridge.setFaceLayout(layout)
        _ui.update {
            it.copy(
                controllerState = ControllerState.Neutral,
                requestedFaceLayout = inputBackend.requestedFaceLayout,
                resolvedFaceLayout = inputBackend.resolvedFaceLayout,
            )
        }
        diagnostics.event("controller", "face layout", "${layout.key}/${inputBackend.resolvedFaceLayout.layout.key}")
        notice("Controller layout set to ${layout.title}; held input was cleared")
    }

    /**
     * The saved adapter, as a platform-neutral [BridgeHost].
     *
     * The Bluetooth device object stops here: everything above this line — the
     * UI, and the bridge itself — sees only an address and a name, which is all
     * either of them ever needed.
     */
    @SuppressLint("MissingPermission")
    fun knownControllerHost(): BridgeHost? {
        val relationship = relationshipStore.load() ?: return null
        val adapter = getApplication<Application>().getSystemService(BluetoothManager::class.java)?.adapter ?: return null
        return runCatching { adapter.getRemoteDevice(relationship.address) }
            .getOrNull()?.takeIf { it.bondState == BluetoothDevice.BOND_BONDED }
            ?.let(::AndroidBridgeHost)
    }

    fun acquireControllerBridge() = session.start(knownControllerHost())

    /**
     * Restore the handheld controller session after a foreground resume, but only after a fresh
     * management read proves that no controller currently owns the adapter. This deliberately
     * avoids stealing input from a physical controller and consumes at most one attempt per
     * resume. Android still owns the foreground HID registration/connection callbacks.
     */
    fun requestAutomaticControllerResume() {
        automaticControllerResumeJob?.cancel()
        val hasRelationship = relationshipStore.load() != null
        // A touchscreen-only host has no physical descriptor and is still a
        // complete controller source, so the on-screen controller being open
        // satisfies the same precondition a selected pad does.
        val hasInputSource = _ui.value.selectedSourceDescriptor != null || bridge.touch.active
        if (!hasInputSource || !hasRelationship) return
        automaticControllerResumeJob = viewModelScope.launch {
            withTimeoutOrNull(AUTOMATIC_CONTROLLER_RESUME_TIMEOUT_MS) {
                while (true) {
                    val state = _ui.value
                    if (state.bridge.phase in setOf(
                            BridgeLinkPhase.Preparing,
                            BridgeLinkPhase.Registering,
                            BridgeLinkPhase.Connecting,
                            BridgeLinkPhase.Playing,
                        )
                    ) return@withTimeoutOrNull
                    if (SessionResumePolicy.canQueryAdapter(
                            hasSelectedSource = state.selectedSourceDescriptor != null || bridge.touch.active,
                            hasRelationship = relationshipStore.load() != null,
                            managementConnected = state.connection.connected,
                            busy = state.busy,
                            phase = state.bridge.phase,
                        )
                    ) {
                        val input = runCatching {
                            ManagementDiagnosticContext.withWorkflow("automatic-controller-resume") {
                                adapter.refreshInputSources()
                            }
                        }.getOrNull()
                        if (input == null) {
                            delay(AUTOMATIC_CONTROLLER_RESUME_RETRY_MS)
                            continue
                        }
                        val host = knownControllerHost()
                        if (!SessionResumePolicy.shouldAcquire(input.activeId, host != null)) {
                            if (input.activeId == 0L) return@withTimeoutOrNull
                            diagnostics.event(
                                "controller",
                                "automatic resume skipped",
                                "adapter active source=${input.activeId}",
                            )
                            return@withTimeoutOrNull
                        }
                        diagnostics.event(
                            "controller",
                            "automatic resume",
                            "adapter idle; restoring saved handheld",
                        )
                        session.start(requireNotNull(host))
                        return@withTimeoutOrNull
                    }
                    delay(AUTOMATIC_CONTROLLER_RESUME_RETRY_MS)
                }
            }
            automaticControllerResumeJob = null
        }
    }

    fun cancelAutomaticControllerResume() {
        automaticControllerResumeJob?.cancel()
        automaticControllerResumeJob = null
    }

    fun pairedControllerHosts(): List<BridgeHost> = session.knownHosts()
    fun connectControllerHost(host: BridgeHost) = session.connect(host)

    fun stopControllerBridge() {
        bridge.releaseTouchInput(TouchReleaseReason.LinkEnded)
        session.stop()
    }

    /**
     * Clear held input and push a neutral report.
     *
     * Releases the on-screen controller FIRST. Neutralizing only the state
     * machine would leave the engine believing a control is still down, so the
     * next contact event would republish it — which is precisely the "the console
     * kept walking after I switched apps" failure this call exists to prevent.
     */
    fun neutralizeController() {
        bridge.releaseTouchInput(TouchReleaseReason.HostInactive)
        session.neutralize()
    }

    // ------------------------------------------------------- on-screen controller

    /**
     * Open the on-screen controller and give it gameplay input.
     *
     * Everything about authority, actuator binding and face presentation happens
     * inside [AndroidBridge.enterTouchMode]; this adds only the user-visible
     * state and the tuning the settings screen owns.
     */
    fun enterTouchGamepad() {
        bridge.enterTouchMode()
        applyTouchSettings(_ui.value.touchSettings)
        _ui.update {
            it.copy(
                touchGamepadActive = true,
                touchFaceLayout = inputBackend.requestedFaceLayout,
                overlay = AppOverlay.None,
                message = null,
            )
        }
        diagnostics.event("controller", "touch gamepad", "opened")
    }

    /** Put the on-screen controller away and hand input back to the physical pad. */
    fun exitTouchGamepad() {
        if (!_ui.value.touchGamepadActive) return
        bridge.exitTouchMode()
        _ui.update {
            it.copy(
                touchGamepadActive = false,
                selectedSourceDescriptor = inputBackend.selectedDescriptor,
                requestedFaceLayout = inputBackend.requestedFaceLayout,
                resolvedFaceLayout = inputBackend.resolvedFaceLayout,
                controllerState = ControllerState.Neutral,
            )
        }
        diagnostics.event("controller", "touch gamepad", "closed")
    }

    /** Drop every held on-screen control at a boundary, without leaving the mode. */
    fun releaseTouchInput(reason: TouchReleaseReason) = bridge.releaseTouchInput(reason)

    fun setTouchSettings(settings: TouchGamepadSettings) {
        touchSettingsStore.save(settings)
        applyTouchSettings(settings)
        _ui.update { it.copy(touchSettings = settings) }
    }

    /**
     * Adopt a picked image as the controller background.
     *
     * The picture is downsampled and COPIED into the app's own files, off the
     * main thread, before anything is stored. A reference into somebody's photo
     * library is a thing that stops working later — a lapsed grant, a deleted
     * picture — for reasons the app cannot see, and the copy is a few hundred
     * kilobytes. Nothing leaves the device.
     */
    fun adoptTouchBackground(uri: Uri) {
        viewModelScope.launch {
            val application = getApplication<Application>()
            val stored = withContext(Dispatchers.IO) { TouchBackgroundStore.adopt(application, uri) }
            if (stored == null) {
                notice("That picture could not be read")
                return@launch
            }
            setTouchBackground(stored)
        }
    }

    /** Point at a stored copy, or drop back to the default dark background. */
    fun setTouchBackground(reference: String?) {
        if (reference == null) TouchBackgroundStore.remove(getApplication())
        setTouchSettings(_ui.value.touchSettings.copy(backgroundImage = reference))
        diagnostics.event(
            "controller", "touch background",
            if (reference == null) "cleared" else "stored a private copy",
        )
    }

    /** The drawn diamond's presentation. Same resolver as the physical path. */
    fun setTouchFaceLayout(layout: ControllerFaceLayout) {
        bridge.setFaceLayout(layout)
        _ui.update {
            it.copy(
                touchFaceLayout = inputBackend.requestedFaceLayout,
                controllerState = ControllerState.Neutral,
            )
        }
        diagnostics.event("controller", "touch face layout", layout.key)
    }

    private fun applyTouchSettings(settings: TouchGamepadSettings) {
        bridge.touch.setConfig(TouchControlConfig.Default.copy(stickDeadzone = settings.stickDeadzone))
    }

    /**
     * One line that localizes an on-screen control that "does not work".
     *
     * Read left to right: contacts arriving at all, contacts that landed on
     * nothing, contacts that hit an already-owned control, how many controls are
     * held now, and why the last global release happened. A zero in the first
     * position and a non-zero in the second are completely different faults with
     * the same symptom.
     */
    private fun describeTouchGamepad(): String {
        val touch = bridge.touch
        val snapshot = touch.diagnostics()
        return buildString {
            append(if (touch.active) "active" else "inactive")
            append(" claimed=").append(snapshot.contactsClaimed)
            append(" unclaimed=").append(snapshot.contactsUnclaimed)
            append(" contested=").append(snapshot.contactsContested)
            append(" cancelled=").append(snapshot.contactsCancelled)
            append(" held=").append(snapshot.ownedControls)
            append(" releaseAll=").append(snapshot.releaseAllCount)
            append("/").append(snapshot.lastReleaseReason?.name ?: "none")
            append(" layoutFits=").append(touch.engine.resolvedLayout.fits)
        }
    }

    fun recordLifecycle(event: String) {
        diagnostics.event("app", "lifecycle", event)
        refreshPlatformDiagnostics()
    }

    fun refreshPlatformDiagnostics() {
        val app = getApplication<Application>()
        val manager = app.getSystemService(BluetoothManager::class.java)
        val scanGranted = Build.VERSION.SDK_INT < 31 || ContextCompat.checkSelfPermission(app, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
        val connectGranted = Build.VERSION.SDK_INT < 31 || ContextCompat.checkSelfPermission(app, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        val enabled = if (connectGranted) runCatching { manager?.adapter?.isEnabled == true }.getOrDefault(false) else false
        _ui.update {
            it.copy(platform = PlatformDiagnostics(
                bluetoothAvailable = manager?.adapter != null,
                bluetoothEnabled = enabled,
                scanPermission = scanGranted,
                connectPermission = connectGranted,
                companionDeviceManager = app.getSystemService(CompanionDeviceManager::class.java) != null,
            ))
        }
    }

    fun exportDiagnostics(): File {
        // Pull the live motion sample text now, on demand. It is deliberately NOT
        // refreshed on the report path: doing that wrote the observable state at
        // 125 Hz and starved the sensors it was reporting on.
        session.refreshMotionDiagnostics()
        session.refreshOutputStatus()
        val ui = _ui.value
        val directory = File(getApplication<Application>().cacheDir, "diagnostics").apply { mkdirs() }
        val file = File(directory, "picoswitch-companion-diagnostics.txt")
        val descriptorHash = MessageDigest.getInstance("SHA-256").digest(BridgeHidDescriptor.bytes)
            .joinToString("") { "%02x".format(it) }
        val report = diagnostics.export(linkedMapOf(
            "App" to BuildConfig.VERSION_NAME,
            "Android" to "${Build.VERSION.RELEASE} API ${Build.VERSION.SDK_INT}",
            "Device" to "${Build.MANUFACTURER} ${Build.MODEL}",
            "Bluetooth available/enabled" to "${ui.platform.bluetoothAvailable}/${ui.platform.bluetoothEnabled}",
            "Permissions scan/connect" to "${ui.platform.scanPermission}/${ui.platform.connectPermission}",
            "CompanionDeviceManager" to ui.platform.companionDeviceManager.toString(),
            "Management state" to ui.connection.phase.name,
            "Adapter relationship" to if (ui.adapterRelationship == null) "none" else "saved",
            "Relationship lifecycle" to ui.relationshipStatus.toString(),
            "Firmware" to ui.snapshot.firmware.version.ifBlank { "unknown" },
            // The single line that would have ended the 2026-08-15 investigation
            // in one read instead of several hours.
            "Firmware build" to ui.snapshot.firmware.build.ifBlank { "not reported" },
            "Bridge contract" to ("app expects ${BridgeContract.VERSION}; " +
                "adapter reports ${ui.snapshot.firmware.bridgeContract.takeIf { it > 0 } ?: "nothing"}"),
            "Bridge compatibility" to ui.bridgeCompatibility.summary,
            "Capabilities" to ui.snapshot.capabilities.toString(),
            "Bridge phase/registered" to "${ui.bridge.phase}/${ui.bridge.registered}",
            "Bridge descriptor" to "${BridgeHidDescriptor.bytes.size} bytes sha256=$descriptorHash",
            "Saved HID hosts" to pairedControllerHosts().size.toString(),
            "Reports" to "${ui.bridge.reportCount}; last=${ui.bridge.lastReportAtMillis}",
            "Source capabilities" to ui.bridge.capabilities.toString(),
            // The three layers, deliberately separate. A platform reading that
            // disagrees with the normalized one localizes the fault to the
            // backend; a normalized reading that disagrees with the wire bytes
            // localizes it to the protocol layer.
            "Normalized input" to BridgeFormat.describeNormalized(ui.controllerState),
            "Wire report" to BridgeFormat.hex(ControllerReportEncoder.encode(ui.controllerState)),
            "Platform motion" to ui.bridge.motion.platformRaw,
            "Canonical motion" to ui.bridge.motion.canonical,
            "Motion frame" to (
                if (ui.bridge.motion.frameRotationMeasured) "${ui.bridge.motion.frameRotationDegrees} deg"
                else "unreadable; assuming 0"
                ),
            // Ordered boundary counters. The first stage reading 0 while the one
            // above it is non-zero IS the fault; nothing below it is worth reading.
            "Bridge counters" to bridge.countersLine(),
            "Bridge wiring" to bridge.wiringReport(),
            "Output route" to ui.bridge.output.route,
            "Output warning" to (ui.bridge.output.warning ?: "none"),
            "Controller face layout" to "${ui.requestedFaceLayout.key}/${ui.resolvedFaceLayout.layout.key}",
            // The on-screen controller's own boundary chain, in the order it has
            // to be read: did contacts arrive, did one claim a control, is touch
            // authoritative, and is it holding something the authority is
            // discarding. Counters and last-known values only -- a diagnostic
            // that sampled at the contact rate would be the stall it was
            // reporting on.
            "Host input authority" to inputBackend.controller.authority.name,
            "Touch gamepad" to describeTouchGamepad(),
            "Touch contribution" to inputBackend.controller.touchContribution.toString(),
            "Adapter active input" to ui.snapshot.input.toString(),
            "Identity refresh pending" to ui.identityRefreshPending.toString(),
        ))
        file.writeText(report)
        diagnostics.event("app", "diagnostics exported")
        return file
    }

    private fun reconcileControllerLinkSource(observed: dev.picoswitch.management.AdapterInputState? = null) {
        if (_ui.value.bridge.phase != BridgeLinkPhase.Playing || !_ui.value.connection.connected) return
        if (bridgeSourceReconciliationJob?.isActive == true) return
        bridgeSourceReconciliationJob = viewModelScope.launch {
            withTimeoutOrNull(CONTROLLER_SOURCE_RECONCILE_TIMEOUT_MS) {
                var input = observed
                while (_ui.value.bridge.phase == BridgeLinkPhase.Playing && _ui.value.connection.connected) {
                    if (_ui.value.busy || _ui.value.kbmBusy || personalityTransitionActive) {
                        delay(CONTROLLER_SOURCE_RECONCILE_RETRY_MS)
                        continue
                    }
                    input = input ?: runCatching {
                        ManagementDiagnosticContext.withWorkflow("controller-source-reconcile") {
                            adapter.refreshInputSources()
                        }
                    }.getOrNull()
                    if (input == null) {
                        delay(CONTROLLER_SOURCE_RECONCILE_RETRY_MS)
                        continue
                    }
                    if (input.activeId != 0L) {
                        diagnostics.event(
                            "controller", "source.auto_skipped", "active=${input.activeId}; existing owner retained",
                        )
                        return@withTimeoutOrNull
                    }
                    val sourceId = SessionResumePolicy.soleSourceToActivate(
                        input.activeId,
                        input.sources.map { it.id },
                    )
                    if (sourceId != null) {
                        // Read once more immediately before the mutation so a physical controller
                        // that won the race is not displaced by stale source-list state.
                        val current = runCatching {
                            ManagementDiagnosticContext.withWorkflow("controller-source-confirm") {
                                adapter.refreshInputSources()
                            }
                        }.getOrNull()
                        if (current != null && SessionResumePolicy.soleSourceToActivate(
                                current.activeId,
                                current.sources.map { it.id },
                            ) == sourceId
                        ) {
                            ManagementDiagnosticContext.withWorkflow("controller-source-auto-select") {
                                adapter.setActiveInput(sourceId)
                            }
                            diagnostics.event("controller", "source.auto_selected", "source=$sourceId")
                        }
                        return@withTimeoutOrNull
                    }
                    if (input.sources.size > 1) {
                        diagnostics.event("controller", "source.auto_skipped", "multiple ready sources")
                        return@withTimeoutOrNull
                    }
                    input = null
                    delay(CONTROLLER_SOURCE_RECONCILE_RETRY_MS)
                }
            }
            bridgeSourceReconciliationJob = null
        }
    }

    private fun executeLifecycleDecision(decision: AdapterLifecycleDecision) {
        when (decision) {
            AdapterLifecycleDecision.Ignored -> Unit
            is AdapterLifecycleDecision.AwaitBond -> Unit
            is AdapterLifecycleDecision.Connect -> startVerifiedManagementConnection(decision.attempt)
            is AdapterLifecycleDecision.RelationshipMetadataUpdated -> {
                relationshipStore.save(decision.relationship)
                _ui.update { it.copy(adapterRelationship = decision.relationship) }
                diagnostics.event(
                    "relationship",
                    "association.metadata",
                    "association=${decision.relationship.associationId ?: "legacy"}",
                )
            }
            is AdapterLifecycleDecision.RepairRequired -> {
                diagnostics.event("relationship", "repair.required", decision.message)
                notice(decision.message)
            }
        }
    }

    private fun startVerifiedManagementConnection(attempt: AdapterConnectionAttempt) {
        if (relationshipConnectionJob?.isActive == true) return
        diagnostics.event(
            "relationship", "connect.request",
            "attempt=${attempt.generation} reason=${attempt.reason.diagnosticName} " +
                "association=${attempt.relationship.associationId ?: "none"} bond=${relationshipCoordinator.status.bond}",
        )
        relationshipConnectionJob = viewModelScope.launch {
            try {
                relationshipRetirementJob?.join()
                ManagementDiagnosticContext.withWorkflow("connect-identity") {
                    adapter.connectKnown(
                        attempt.relationship.address,
                        ManagementConnectionContext(
                            logicalAttempt = attempt.generation,
                            reason = attempt.reason.diagnosticName,
                            associationId = attempt.relationship.associationId,
                            bondState = relationshipCoordinator.status.bond.name,
                            useDiscoveredPeer = attempt.reason == AdapterConnectReason.FirstPair,
                            expectsBonding = attempt.generation == gattInitiatedBondGeneration,
                        ),
                    )
                }
                val verified = relationshipCoordinator.connectionSucceeded(attempt.generation) ?: return@launch
                val previous = relationshipStore.load()
                relationshipStore.save(verified)
                if (previous?.associationId != null && previous.associationId != verified.associationId) {
                    disassociate(previous)
                }
                _ui.update {
                    it.copy(
                        adapterRelationship = verified,
                        relationshipStatus = relationshipCoordinator.status,
                    )
                }
                diagnostics.event(
                    "relationship", "connect.verified",
                    "attempt=${attempt.generation} reason=${attempt.reason.diagnosticName} association=${verified.associationId ?: "legacy"}",
                )
                // A fresh BLE management bond is not proof that the Classic HID-device path is
                // ready. Exercise it immediately while the user's Pico pairing window is still
                // open; the ownership read prevents stealing an already-active controller.
                requestAutomaticControllerResume()
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                diagnostics.event("relationship", "connect.cancelled", "attempt=${attempt.generation}")
                throw cancelled
            } catch (error: Throwable) {
                relationshipCoordinator.connectionFailed(
                    attempt.generation,
                    error.message ?: "The adapter connection could not be verified.",
                )
                publishRelationshipStatus()
                diagnostics.error("relationship", "connect attempt ${attempt.generation}", error)
                notice(error.message ?: "The adapter connection could not be verified.")
            } finally {
                // A cancelled generation may finish after its replacement has already been
                // assigned. It must not clear ownership of that newer job.
                if (relationshipConnectionJob === coroutineContext[Job]) {
                    relationshipConnectionJob = null
                }
            }
        }
    }

    private fun publishRelationshipStatus(connectionOverride: ConnectionPhase? = null) {
        val relationship = relationshipCoordinator.status
        val phase = connectionOverride ?: when (relationship.phase) {
            AdapterRelationshipPhase.NoRelationship, AdapterRelationshipPhase.Idle -> ConnectionPhase.Idle
            AdapterRelationshipPhase.Associating -> ConnectionPhase.Associating
            AdapterRelationshipPhase.Bonding -> ConnectionPhase.Bonding
            AdapterRelationshipPhase.Connecting -> ConnectionPhase.Connecting
            AdapterRelationshipPhase.Connected -> ConnectionPhase.Connected
            AdapterRelationshipPhase.Failed -> ConnectionPhase.Failed
            AdapterRelationshipPhase.RepairRequired -> ConnectionPhase.RepairRequired
        }
        _ui.update { state ->
            val connection = if (phase == ConnectionPhase.Connected && state.connection.connected) {
                state.connection
            } else state.connection.copy(phase = phase, message = relationship.message, attempt = relationship.generation.toInt())
            state.copy(relationshipStatus = relationship, connection = connection)
        }
    }

    @SuppressLint("MissingPermission")
    private fun bondState(address: String): AndroidBondState = runCatching {
        val manager = getApplication<Application>().getSystemService(BluetoothManager::class.java)
        when (manager.adapter.getRemoteDevice(address).bondState) {
            BluetoothDevice.BOND_NONE -> AndroidBondState.None
            BluetoothDevice.BOND_BONDING -> AndroidBondState.Bonding
            BluetoothDevice.BOND_BONDED -> AndroidBondState.Bonded
            else -> AndroidBondState.Unknown
        }
    }.getOrDefault(AndroidBondState.Unknown)

    private fun Int.toProductBondState(): AndroidBondState = when (this) {
        BluetoothDevice.BOND_NONE -> AndroidBondState.None
        BluetoothDevice.BOND_BONDING -> AndroidBondState.Bonding
        BluetoothDevice.BOND_BONDED -> AndroidBondState.Bonded
        else -> AndroidBondState.Unknown
    }

    private fun clearOwnedRelationship(
        relationship: AdapterRelationship?,
        removeAllCompanionAssociations: Boolean = false,
        onComplete: (() -> Unit)? = null,
    ) {
        relationshipPairingJob?.cancel()
        relationshipConnectionJob?.cancel()
        pairingDevice = null
        autoReconnectAttempted = false
        relationshipStore.clear()
        relationshipCoordinator.forget()
        _ui.update {
            it.copy(
                adapterRelationship = null,
                relationshipStatus = relationshipCoordinator.status,
                connection = ConnectionState(),
            )
        }
        diagnostics.event(
            "relationship", "relationship.clear",
            "app record cleared; Android bond and adapter bonds retained",
        )
        val priorRetirement = relationshipRetirementJob
        relationshipRetirementJob = viewModelScope.launch {
            if (priorRetirement?.isActive == true) {
                priorRetirement.join()
            } else {
                runCatching { adapter.disconnect() }
                    .onFailure { diagnostics.error("management", "relationship clear disconnect", it) }
            }
            if (removeAllCompanionAssociations) disassociateAllCompanionAssociations()
            else relationship?.let(::disassociate)
            onComplete?.invoke()
        }
    }

    @SuppressLint("MissingPermission")
    private fun disassociate(relationship: AdapterRelationship) {
        val manager = getApplication<Application>().getSystemService(CompanionDeviceManager::class.java) ?: return
        runCatching {
            if (Build.VERSION.SDK_INT >= 33) {
                val associationId = relationship.associationId ?: manager.myAssociations
                    .singleOrNull { it.deviceMacAddress?.toString().equals(relationship.address, true) }
                    ?.id
                if (associationId != null) manager.disassociate(associationId)
            } else {
                @Suppress("DEPRECATION")
                manager.disassociate(relationship.address)
            }
        }.onSuccess {
            diagnostics.event(
                "relationship", "association.removed",
                "association=${relationship.associationId ?: "legacy"}; Android bond retained",
            )
        }.onFailure { diagnostics.error("relationship", "association.remove", it) }
    }

    @SuppressLint("MissingPermission")
    private fun disassociateAllCompanionAssociations() {
        val manager = getApplication<Application>().getSystemService(CompanionDeviceManager::class.java) ?: return
        runCatching {
            // This app has one CDM association call site and it is name-filtered to PicoSwitch2's
            // current/legacy identities. Clearing all app-owned records is reserved for the
            // explicit ambiguous Repair flow; ordinary Forget removes only the saved relationship.
            val count = if (Build.VERSION.SDK_INT >= 33) {
                manager.myAssociations.onEach { manager.disassociate(it.id) }.size
            } else {
                @Suppress("DEPRECATION")
                manager.associations.onEach { manager.disassociate(it) }.size
            }
            diagnostics.event("relationship", "association.repair_clear", "removed=$count; Android bonds retained")
        }.onFailure { diagnostics.error("relationship", "association.repair_clear", it) }
    }

    private fun launch(label: String, action: suspend () -> Unit) {
        // Admission happens synchronously, before the coroutine can be queued.
        // The UI busy flag is presentation state and can lag one dispatcher
        // turn behind a burst of taps, so it cannot be the concurrency lock.
        if (!operationAdmission.tryAcquire()) return
        viewModelScope.launch {
            try {
                _ui.update { it.copy(busy = true, operation = OperationProgress(label, 0, 0), message = null) }
                runCatching {
                    ManagementDiagnosticContext.withWorkflow(label) { action() }
                }.onFailure { error -> diagnostics.error("app", label, error); notice(error.message ?: "Operation failed") }
            } finally {
                _ui.update { it.copy(busy = false, operation = null) }
                operationAdmission.release()
            }
        }
    }

    private fun markIdentityRefreshPending(reason: String) {
        savedState[KEY_IDENTITY_PENDING] = true
        _ui.update { it.copy(identityRefreshPending = true) }
        diagnostics.event("adapter", "re-enumeration required", reason)
    }

    private fun notice(message: String) { _ui.update { it.copy(message = message) } }

    /**
     * Replace the UI state wholesale, for the debug-only layout lab.
     *
     * Visual inspection is part of acceptance for a UI change, and most of this
     * application's screens only exist in their interesting form while an
     * adapter with a keyboard, a mouse and an Amiibo library is attached. The
     * lab (`app/src/debug`) renders those states synthetically so every screen
     * can be checked at every window shape without hardware.
     *
     * No product code path calls this, and the lab activity it serves is not
     * part of the release variant. It is a seam for inspection, never a way to
     * fabricate adapter state the user could see.
     */
    internal fun applyLayoutLabState(transform: (CompanionUiState) -> CompanionUiState) {
        _ui.update(transform)
    }

    private fun updateTheme(update: (ThemeSelection) -> ThemeSelection) {
        val next = update(_theme.value)
        if (next == _theme.value) return
        themeStore.save(next)
        _theme.value = next
        diagnostics.event("app", "appearance changed", "${next.mode.key}/${next.palette.key}")
    }

    override fun onCleared() {
        bridge.close()
        // Retire the session, do not close the transport: it is process-scoped
        // now, and close() cancels its lifecycle scope permanently. See
        // ManagementOwner.releaseSession().
        ManagementOwner.releaseSession()
        super.onCleared()
    }

    companion object {
        private const val MAX_IMPORT_BYTES = 2048
        private const val MAX_LIBRARY_ARCHIVE_BYTES = AmiiboLibraryArchive.MAX_ARCHIVE_BYTES
        private const val RETAIL_KEY_BYTES = 160
        private const val ADAPTER_POLL_MILLIS = 5_000L
        // Long enough that a slider drag issues a handful of commands rather
        // than hundreds, short enough that the change still reads as live while
        // the finger is down.
        private const val MOUSE_APPLY_DEBOUNCE_MS = 140L
        private const val AUTOMATIC_CONTROLLER_RESUME_TIMEOUT_MS = 20_000L
        private const val AUTOMATIC_CONTROLLER_RESUME_RETRY_MS = 250L
        private const val CONTROLLER_SOURCE_RECONCILE_TIMEOUT_MS = 8_000L
        private const val CONTROLLER_SOURCE_RECONCILE_RETRY_MS = 250L
        private const val PERSONALITY_READBACK_ATTEMPTS = 3
        private const val PERSONALITY_READBACK_RETRY_MS = 200L
        private const val PERSONALITY_DISCONNECT_GRACE_MS = 1_500L
        private const val PERSONALITY_RECONNECT_TIMEOUT_MS = 40_000L
        private const val ADAPTER_BOND_POLL_MS = 250L
        private const val ADAPTER_BOND_TIMEOUT_MS = 60_000L
        private const val KEY_SECTION = "section"
        private const val KEY_AMIIBO = "selectedAmiibo"
        private const val KEY_SOURCE = "selectedSource"
        private const val KEY_IDENTITY_PENDING = "identityRefreshPending"
        private val FIGURE_ID = Regex("[0-9A-F]{16}")
    }
}
