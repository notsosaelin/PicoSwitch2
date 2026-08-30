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
import dev.picoswitch.bridge.touch.TouchLayoutComposer
import dev.picoswitch.bridge.touch.TouchLayoutDocument
import dev.picoswitch.bridge.touch.TouchProfileCatalog
import dev.picoswitch.bridge.touch.TouchProfileEdit
import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.bridge.touch.TouchProfileLibrary
import dev.picoswitch.bridge.touch.TouchProfileLibraryEditor
import dev.picoswitch.bridge.touch.TouchProfileLibraryJsonCodec
import dev.picoswitch.bridge.touch.TouchProfileDecodeResult
import dev.picoswitch.bridge.touch.TouchReleaseReason
import dev.picoswitch.companion.bridge.AndroidBridge
import dev.picoswitch.companion.bridge.AndroidBridgeHost
import dev.picoswitch.companion.bridge.TouchProfileSelector
import dev.picoswitch.companion.data.*
import dev.picoswitch.companion.diagnostics.DiagnosticEntry
import dev.picoswitch.companion.diagnostics.DiagnosticLog
import dev.picoswitch.companion.diagnostics.DiagnosticSummary
import dev.picoswitch.companion.diagnostics.ManagementDiagnosticContext
import dev.picoswitch.companion.model.*
import dev.picoswitch.management.WakeResult
import dev.picoswitch.companion.transport.AdapterResetSignature
import dev.picoswitch.companion.transport.BleGattManagementTransport
import dev.picoswitch.companion.protocol.ManagementConnectionContext
import dev.picoswitch.companion.ui.touch.TouchBackgroundStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
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
     * Face presentation for the DRAWN diamond.
     *
     * No longer user-selectable: the Touch Gamepad menu offers the real
     * controller personality instead, and the drawn diamond follows whichever
     * controller the adapter is actually presenting. Kept because the renderer
     * still has to resolve label positions through the shared resolver, and
     * pinned to the Nintendo arrangement because every personality this surface
     * can draw IS a Nintendo controller.
     */
    val touchFaceLayout: ControllerFaceLayout = ControllerFaceLayout.Nintendo,
    /** Confirmed console personality; null keeps the surface neutral and disabled. */
    val touchProfileId: TouchProfileId? = null,
    /**
     * Every layout profile available for [touchProfileId], and which is active.
     *
     * The immutable factory profile is a synthesized member of this value rather
     * than something storage has to supply, so it is present even when nothing
     * has ever been saved. Null only while no gameplay personality is confirmed.
     */
    val touchProfiles: TouchProfileLibrary? = null,
    /**
     * The active profile's layout document, never live gameplay state.
     *
     * Derived from [touchProfiles]; kept as its own field because the surface
     * composes against it on every geometry change and should not have to know
     * how profile selection works.
     */
    val touchLayoutDocument: TouchLayoutDocument? = null,
    /** Non-blocking persistence/fallback explanation shown in the Touch Gamepad menu. */
    val touchLayoutWarning: String? = null,
    /**
     * The active adapter as the connection lifecycle sees it, or null when the
     * user has not selected one. Derived from [adapters] and [activeAdapterId];
     * kept as its own field because most screens only ever ask "is there an
     * adapter, and what is it called".
     */
    val adapterRelationship: AdapterRelationship? = null,
    /**
     * The adapter's controller-pairing operation, when one is running.
     *
     * Null means "not pairing", which is different from an idle status the app
     * happens to be holding: a switch or a disconnect clears this outright
     * rather than leaving a stale countdown on screen.
     */
    val pairing: PairingStatus? = null,
    /** Every adapter this app knows, in registry order. */
    val adapters: List<AdapterRecord> = emptyList(),
    /**
     * The active adapter's controllers, as Connected / Saved / Recent.
     *
     * Derived from the adapter's own peer inventory plus this app's per-adapter
     * history. Kept as its own field because the derivation has to happen when
     * EITHER source changes, and a screen should not have to know that.
     */
    val controllerInventory: ControllerInventoryView = ControllerInventoryView(),
    val activeAdapterId: AdapterId? = null,
    /** A switch between adapters is in progress; the active adapter is chosen but not yet reached. */
    val adapterSwitchInProgress: Boolean = false,
    /**
     * Why the active adapter could not be reached, if it could not.
     *
     * Present specifically so a failed switch reads as "the adapter you chose is
     * not connected" rather than being hidden behind a fallback to the previous
     * adapter, which this design does not do.
     */
    val adapterSwitchFailure: String? = null,
    /**
     * Per-adapter CompanionDeviceManager state. Keyed by adapter because one
     * adapter losing its association says nothing about the others.
     */
    val associationStates: Map<AdapterId, CompanionAssociationState> = emptyMap(),
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
    private val touchProfileStore = AndroidTouchProfileStore(application)
    private val registryStore = AdapterRegistryStore(application)
    /**
     * Every adapter this app knows. Held here rather than read from disk on
     * demand because it is consulted on nearly every lifecycle decision; every
     * mutation goes through [updateRegistry], which persists and republishes.
     */
    @Volatile private var registry: AdapterRegistry = registryStore.load()
    private val peerHistoryStore = PeerHistoryStore(application)
    /**
     * What each adapter's peers have been, per adapter.
     *
     * The adapter is authoritative about which peers exist; this is the only
     * place that remembers what they WERE, because the adapter's role
     * classification is live evidence only and forgets across a reboot. Every
     * mutation goes through [updatePeerHistory], which persists and republishes.
     */
    @Volatile private var peerHistory: PeerHistoryBook = peerHistoryStore.load()
    private val relationshipCoordinator = AdapterRelationshipCoordinator(activeRelationship())
    /**
     * Which adapter is active, and the ordered handover between two of them.
     *
     * Sits above [relationshipCoordinator], which still owns one attempt at one
     * relationship. Separate counters on purpose: merged, a connection retry
     * would be indistinguishable from a change of adapter.
     */
    private val activeAdapter = ActiveAdapterCoordinator(registry.activeId)
    private val adapterSwitch = AdapterSwitch(activeAdapter, AdapterSwitchExecutor())
    /** One switch at a time. Overlapping switches are also generation-guarded; this keeps them cheap. */
    private val switchLock = Mutex()
    private var adapterSwitchJob: Job? = null
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
            adapterRelationship = activeRelationship(),
            adapters = registry.records,
            activeAdapterId = registry.activeId,
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
                // One authority decides whether this event may speak for the
                // active adapter. Retirement is awaited before activation, so a
                // rejection here is defence in depth rather than the primary
                // guarantee -- but it is the guarantee that survives someone
                // later reordering the switch.
                if (!activeAdapter.accepts(value.address)) {
                    diagnostics.event(
                        "relationship", "connection.ignored",
                        "address=${value.address?.takeLast(5) ?: "none"} phase=${value.phase} " +
                            "active=${activeAdapter.state.activeId?.shortLabel ?: "none"} " +
                            "switch=${activeAdapter.state.phase}",
                    )
                    return@collect
                }
                if (!value.connected && activeAdapter.markDisconnected()) publishAdapterState()
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
                val loaded = loadTouchProfile(value.personality.current)
                val previous = _ui.value
                if (previous.touchGamepadActive && previous.touchProfileId != loaded.profileId) {
                    // The profile replacement is a live-input boundary. Release
                    // while the Classic transport can still carry neutral; do
                    // not stop or re-register that surviving link.
                    bridge.releaseTouchInput(TouchReleaseReason.PersonalityChanged)
                    session.neutralize()
                }
                _ui.update {
                    it.copy(
                        snapshot = value,
                        touchProfileId = loaded.profileId,
                        touchProfiles = loaded.library,
                        touchLayoutDocument = loaded.document,
                        touchLayoutWarning = loaded.warning,
                    )
                }
                refreshBridgeCompatibility()
                // The inventory arrives inside the snapshot, so the derived
                // Connected / Saved / Recent view is recomputed here rather than
                // only where a read is issued.
                publishControllerInventory()
                refreshAdapterAmiiboCatalog(value.amiibo)
                // Only from a live, settled session belonging to the adapter this
                // would be cached against. A snapshot carries no address, so
                // without the settled check the outgoing adapter's firmware
                // could be written onto the incoming adapter's record.
                if (_ui.value.connection.connected && activeAdapter.state.connected) {
                    cacheActiveAdapterState(
                        firmware = value.firmware.version,
                        personality = value.personality.current
                            .takeIf { it != Personality.Unknown }?.wireName,
                    )
                }
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

    // -----------------------------------------------------------------------
    // Adapter registry
    // -----------------------------------------------------------------------
    // Many known adapters, at most one active management session. The registry
    // is app-local truth about which adapters exist and what the user calls
    // them; it never asserts anything about live Bluetooth state.

    /** The active adapter, as the connection lifecycle consumes it. */
    private fun activeRelationship(): AdapterRelationship? = registry.active?.toRelationship()

    /**
     * The one place peer history changes.
     *
     * Persist-then-publish under a lock, for the same reason the registry does
     * it: an inventory read landing while the user removes a history row must
     * not interleave into a lost update. Nothing is written when the transform
     * is a no-op.
     */
    @Synchronized
    private fun updatePeerHistory(transform: (PeerHistoryBook) -> PeerHistoryBook) {
        val next = transform(peerHistory)
        if (next != peerHistory) {
            peerHistory = next
            peerHistoryStore.save(next)
        }
        publishControllerInventory()
    }

    /**
     * Recompute the Connected / Saved / Recent view.
     *
     * Called whenever EITHER of its two inputs moves: the adapter's inventory
     * (which arrives in the snapshot) and this app's history. The active adapter
     * is part of the key, so a switch republishes it against the new adapter
     * rather than leaving the previous adapter's controllers on screen.
     */
    private fun publishControllerInventory() {
        val history = peerHistory.forAdapter(registry.activeId)
        _ui.update { current ->
            current.copy(
                controllerInventory = ControllerInventory.build(current.snapshot.peers, history),
            )
        }
    }

    /**
     * The one place the registry changes.
     *
     * Persist-then-publish, under a lock, so a reconciliation arriving while the
     * user is renaming cannot interleave into a lost update. Nothing is written
     * when the transform is a no-op, which keeps a five-second association
     * refresh from rewriting the document forever.
     */
    @Synchronized
    private fun updateRegistry(transform: (AdapterRegistry) -> AdapterRegistry): AdapterRegistry {
        val next = transform(registry)
        if (next != registry) {
            registry = next
            registryStore.save(next)
        }
        publishAdapterState()
        return next
    }

    /**
     * Publish the registry and the active-adapter authority together.
     *
     * They are one picture. Publishing the registry without the switch state
     * would let the UI show an adapter as selected with no indication that it is
     * mid-transition or that reaching it failed, which is the ambiguity this
     * phase exists to remove.
     */
    private fun publishAdapterState() {
        val snapshot = registry
        val active = activeAdapter.state
        _ui.update {
            it.copy(
                adapters = snapshot.records,
                activeAdapterId = snapshot.activeId,
                adapterRelationship = snapshot.active?.toRelationship(),
                adapterSwitchInProgress = active.transitioning,
                adapterSwitchFailure = active.failure,
            )
        }
        // The controller view is keyed by adapter, so it has to follow the
        // active-adapter authority; otherwise a switch leaves the previous
        // adapter's controllers on screen under the new adapter's name.
        publishControllerInventory()
    }

    /**
     * Switch the active adapter.
     *
     * One generation-owned transition, executed by [AdapterSwitch]: the outgoing
     * adapter is retired completely before the incoming one is connected, and if
     * the incoming one cannot be reached the app settles at "selected, not
     * connected" for it rather than returning to the previous adapter.
     */
    fun selectAdapter(id: AdapterId) {
        if (registry.record(id) == null) return
        adapterSwitchJob = viewModelScope.launch {
            switchLock.withLock {
                when (val outcome = adapterSwitch.switchTo(id)) {
                    SwitchOutcome.AlreadyActive -> Unit
                    is SwitchOutcome.Superseded -> diagnostics.event(
                        "relationship", "adapter.switch_superseded",
                        "adapter=${id.shortLabel} generation=${outcome.generation}",
                    )
                    is SwitchOutcome.Activating -> diagnostics.event(
                        "relationship", "adapter.switch_activating",
                        "adapter=${id.shortLabel} " +
                            "previous=${outcome.plan.previous?.shortLabel ?: "none"} " +
                            "generation=${outcome.plan.generation}",
                    )
                }
            }
        }
    }

    /**
     * The Android side of one adapter switch.
     *
     * Every method here is a step [AdapterSwitch] calls in a fixed order; none
     * of them decides anything. The ordering guarantees live in that class and
     * are covered by `AdapterSwitchTest`, which drives this same interface with
     * a recording fake.
     */
    private inner class AdapterSwitchExecutor : AdapterSwitchPort {
        override fun selectionCommitted(target: AdapterId, previous: AdapterId?) {
            val record = registry.record(target)
            updateRegistry { it.selecting(target) }
            autoReconnectAttempted = false
            // A screen bound to the previous adapter must not survive the
            // switch: an Amiibo or keyboard page belongs to the adapter whose
            // contents it is showing.
            if (previous != null) {
                _ui.update {
                    it.copy(
                        section = AppSection.Adapter,
                        overlay = AppOverlay.None,
                        message = null,
                        connection = ConnectionState(
                            phase = ConnectionPhase.Disconnecting,
                            deviceName = record?.displayName,
                            message = "Switching to ${record?.displayName ?: "the selected adapter"}",
                        ),
                    )
                }
                savedState[KEY_SECTION] = AppSection.Adapter.name
            }
            publishAdapterState()
            diagnostics.event(
                "relationship", "adapter.selected",
                "adapter=${target.shortLabel} previous=${previous?.shortLabel ?: "none"} " +
                    "known=${registry.records.size}",
            )
        }

        override suspend fun stopControllerLink(previous: AdapterId) {
            exitTouchGamepad()
            stopControllerBridge()
            cancelAutomaticControllerResume()
        }

        override suspend fun retireManagement(previous: AdapterId) {
            // Captured before cancelling: the connect job clears its own field
            // in a finally block, so re-reading it to join would sometimes find
            // null and skip the wait -- turning the retirement back into a
            // request, which is the one thing it must not be.
            val pairing = relationshipPairingJob
            val connecting = relationshipConnectionJob
            pairing?.cancel()
            connecting?.cancel()
            pairingDevice = null
            relationshipCoordinator.cancelAndRetainRelationship()
            // Joining is what makes the retirement a real wait. A connect job
            // still unwinding would otherwise publish the previous adapter's
            // failure or success after the next adapter had become authoritative.
            runCatching { pairing?.join() }
            runCatching { connecting?.join() }
            relationshipPairingJob = null
            relationshipConnectionJob = null
            relationshipRetirementJob?.takeIf { it.isActive }?.let { runCatching { it.join() } }
            // AdapterRepository.disconnect() returns only after the transport has
            // retired its GATT generation and emitted its final state, so the
            // outgoing adapter has nothing left in flight once this returns.
            runCatching { adapter.disconnect() }
                .onFailure { diagnostics.error("management", "adapter switch disconnect", it) }
        }

        override fun clearAdapterScopedState() {
            adapter.clearDisconnectedSnapshot()
            kbmMappingsLoaded = false
        }

        override fun beginActivation(target: AdapterId) {
            val record = registry.record(target) ?: return
            relationshipCoordinator.restore(
                record.toRelationship(),
                associationStateOf(target),
                bondState(record.address),
            )
            publishAdapterState()
            publishRelationshipStatus()
            reconnectKnownAdapter(AdapterConnectReason.AdapterSwitch)
        }
    }

    /**
     * Record a verified adapter into the registry and make it the active one.
     *
     * A verified management connection is the only evidence strong enough to
     * create or rebind a registry entry: it means the identity was checked over
     * an encrypted session, not merely that something advertised nearby. It is
     * also what clears [AdapterRecord.repairRequired] — the adapter answering
     * with a working key is the proof that the repair worked.
     *
     * The user's alias is never overwritten here. The adapter's own name is
     * cache; what the user called it is not.
     */
    private fun adoptVerifiedAdapter(relationship: AdapterRelationship): AdapterRecord? {
        val id = AdapterId.fromAddress(relationship.address) ?: return null
        // A first pair selects the adapter it just verified. A switch already
        // selected its target, and adopt() will not disturb a live transition.
        activeAdapter.adopt(id, connected = true)
        // The registry's selection follows the active-adapter authority rather
        // than being decided here, so the two can never disagree about which
        // adapter is active. A verification that the authority did not accept --
        // one belonging to an adapter a switch has moved away from -- updates
        // that adapter's cached details and nothing else.
        val selectThis = activeAdapter.state.activeId == id
        val now = System.currentTimeMillis()
        val next = updateRegistry { current ->
            val existing = current.record(id)
            val record = (existing ?: AdapterRecord(id = id, address = id.value)).copy(
                associationId = relationship.associationId ?: existing?.associationId,
                lastKnownName = relationship.displayName
                    .takeIf { it.isNotBlank() && it != existing?.userAlias }
                    ?: existing?.lastKnownName
                    ?: AdapterRecord.DEFAULT_PRODUCT_NAME,
                lastSeenAtMillis = now,
                lastConnectedAtMillis = now,
                repairRequired = false,
            )
            current.with(record).let { if (selectThis) it.selecting(id) else it }
        }
        return next.record(id)
    }

    /**
     * Cache what the connected adapter reports, against that adapter.
     *
     * Only enough to let the adapter list say something honest about a unit that
     * is not currently connected. Live truth stays in `AdapterSnapshot`, which
     * belongs to the one active session and is cleared on disconnect.
     */
    private fun cacheActiveAdapterState(firmware: String?, personality: String?) {
        val id = registry.activeId ?: return
        if (firmware.isNullOrBlank() && personality.isNullOrBlank()) return
        updateRegistry { current ->
            current.update(id) { record ->
                record.copy(
                    lastFirmwareVersion = firmware?.takeIf(String::isNotBlank) ?: record.lastFirmwareVersion,
                    lastPersonality = personality?.takeIf(String::isNotBlank) ?: record.lastPersonality,
                )
            }
        }
    }

    /**
     * Rename one adapter, locally.
     *
     * The alias is app-local presentation state and is never written to the
     * adapter or into its advertised name: doing that would make a display
     * preference into Bluetooth identity, with the cache and cross-host
     * ambiguity that implies. A blank name clears the alias rather than storing
     * an empty one, which is how the user gets the adapter's own name back.
     */
    fun renameAdapter(id: AdapterId, alias: String?) {
        val cleaned = AdapterAlias.sanitize(alias)
        updateRegistry { it.update(id) { record -> record.copy(userAlias = cleaned) } }
        diagnostics.event(
            "relationship", "adapter.renamed",
            "adapter=${id.shortLabel} alias=${if (cleaned == null) "cleared" else "set"}",
        )
    }

    fun connect() = reconnectKnownAdapter(AdapterConnectReason.Manual)

    fun reconnectKnownAdapter(reason: AdapterConnectReason = AdapterConnectReason.Manual) {
        val relationship = activeRelationship() ?: return
        val decision = relationshipCoordinator.requestReconnect(relationship, reason, bondState(relationship.address))
        publishRelationshipStatus()
        executeLifecycleDecision(decision)
    }

    fun tryAutoReconnect() {
        if (autoReconnectAttempted || _ui.value.connection.connected || _ui.value.busy) return
        // A switch already owns the connection lifecycle and will activate its
        // own target. A foreground pass landing in the middle of one must not
        // start a second connection attempt beside it.
        if (activeAdapter.state.transitioning) return
        if (registry.active == null) return
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
        // adapters. Never use that unrelated Bluetooth truth to reconstruct the registry.
        if (activeAdapter.state.transitioning) {
            diagnostics.event("relationship", "association.reconcile", "deferred during an adapter switch")
            return
        }
        val result = AdapterRegistryReconciler.reconcile(registry, associations)
        updateRegistry { result.registry }
        val active = registry.active
        // Reconciliation may adopt the first adapter this app has ever known.
        activeAdapter.adopt(registry.activeId, connected = _ui.value.connection.connected)
        relationshipCoordinator.restore(
            active?.toRelationship(),
            result.stateOf(active?.id),
            active?.let { bondState(it.address) } ?: AndroidBondState.Unknown,
        )
        _ui.update {
            it.copy(
                associationStates = result.states,
                relationshipStatus = relationshipCoordinator.status,
            )
        }
        diagnostics.event(
            "relationship", "association.reconciled",
            "associations=${associations.size} known=${registry.records.size} " +
                "adopted=${result.adopted.size} active=${active?.id?.shortLabel ?: "none"} " +
                "state=${result.stateOf(active?.id)}",
        )
    }

    private fun associationStateOf(id: AdapterId?): CompanionAssociationState =
        id?.let { _ui.value.associationStates[it] } ?: CompanionAssociationState.Unknown

    /**
     * Remove one adapter from this app.
     *
     * Not a Bluetooth operation. The alias, cached state and app-owned
     * CompanionDeviceManager record for this adapter go; the Android bond and
     * the adapter's own stored bonds are untouched, and the user is told so.
     *
     * Only this adapter's association is removed. Earlier builds cleared every
     * app-owned association here, which was defensible when the app could only
     * ever own one; with a registry it would delete the user's other adapters.
     */
    fun removeAdapterFromApp(id: AdapterId) {
        val record = registry.record(id) ?: return
        val wasActive = registry.activeId == id
        // Advance the switch generation before anything else, so a transition
        // targeting the adapter being deleted cannot go on to activate it.
        if (wasActive) {
            adapterSwitchJob?.cancel()
            activeAdapter.cleared()
        }
        updateRegistry { it.without(id) }
        // History is keyed by adapter, so an adapter the app no longer knows
        // leaves behind rows nothing can ever attribute or display.
        updatePeerHistory { it.without(id) }
        diagnostics.event(
            "relationship", "adapter.removed",
            "adapter=${id.shortLabel} wasActive=$wasActive remaining=${registry.records.size}",
        )
        if (wasActive) {
            clearOwnedRelationship(record.toRelationship())
        } else {
            // A background adapter has no live session to retire, so this must
            // NOT claim relationshipRetirementJob: that slot tracks teardown of
            // the active session, and overwriting it would let a later connect
            // join a job that never disconnected anything.
            viewModelScope.launch { disassociate(record.toRelationship()) }
        }
    }

    /**
     * Repair one adapter's management pairing.
     *
     * Scoped to a single adapter: the other registry entries keep their alias,
     * cached state, association and Android bond. That isolation is the whole
     * point — reflashing one adapter must not cost the user the others.
     */
    fun prepareRepairPairing(id: AdapterId?, onReady: (needsAndroidSettings: Boolean) -> Unit) {
        val record = id?.let { registry.record(it) } ?: registry.active
        if (record == null) {
            onReady(false)
            return
        }
        val ambiguous = associationStateOf(record.id) == CompanionAssociationState.Ambiguous
        val needsSettings = bondState(record.address) == AndroidBondState.Bonded
        diagnostics.event(
            "relationship",
            "repair.started",
            "adapter=${record.id.shortLabel} association=${record.associationId ?: "none"} " +
                "ambiguous=$ambiguous platformBond=$needsSettings",
        )
        // Repair keeps the registry entry so the alias and history survive and
        // the repaired unit can be rebound to it. Only the association, which is
        // the part that is stale or duplicated, is dropped.
        updateRegistry {
            it.update(record.id) { existing -> existing.copy(associationId = null, repairRequired = true) }
        }
        clearOwnedRelationship(
            record.toRelationship(),
            retainRegistryEntry = true,
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

    /**
     * Re-read the adapter's saved pairings.
     *
     * What EXISTS always comes from the adapter, and the app never guesses at
     * it: a controller can be paired from the adapter's own pairing button with
     * no phone present. What the app keeps is a memory of what each peer WAS,
     * which is a different claim and is only ever shown as one -- see
     * [PeerHistoryRecord].
     */
    fun refreshPeers() = launch("Reading saved pairings") { readPeerInventory(explicit = true) }

    /**
     * Read the inventory and fold it into this adapter's history.
     *
     * Only a COMPLETE read is recorded. A partial one is indistinguishable from
     * an adapter that has forgotten a controller, and recording it would move a
     * live saved pairing into "Recent" -- telling the user a controller was
     * unpaired when nothing of the sort happened.
     *
     * The adapter identity is captured BEFORE the read and re-checked after it,
     * because a switch can complete while the read is in flight and writing one
     * adapter's controllers into another adapter's history would be a permanent
     * corruption rather than a transient display error.
     */
    private suspend fun readPeerInventory(explicit: Boolean) {
        val target = registry.activeId
        val inventory = adapter.refreshPeers()
        if (!inventory.complete) {
            diagnostics.event("peers", "refresh.incomplete", "explicit=$explicit")
            return
        }
        if (target != null && registry.activeId == target) {
            updatePeerHistory { book ->
                book.with(target, book.forAdapter(target).observing(inventory, System.currentTimeMillis()))
            }
        }
        diagnostics.event(
            "peers", "refreshed",
            "total=${inventory.total} controllers=${inventory.controllers.size} " +
                "companion=${inventory.peers.count { it.role != PeerRole.PhysicalController }} " +
                "remembered=${peerHistory.forAdapter(target).records.size} explicit=$explicit",
        )
    }

    /**
     * Ask the adapter to open its controller pairing window, then follow it.
     *
     * The polling loop is bounded by the ADAPTER's answer, not by a timer here:
     * it stops as soon as the status is no longer active, and in any case after
     * [PAIRING_POLL_LIMIT] reads. The adapter owns the deadline, so even if
     * this app is killed mid-operation the window still closes on its own --
     * which is why nothing in this function is load-bearing for safety.
     *
     * The operation generation guards the whole loop: a status belonging to an
     * older operation, or arriving after the user switched adapters, is
     * discarded rather than allowed to describe the current one.
     */
    fun startControllerPairing() = launch("Opening pairing window") {
        val adapterAtStart = registry.activeId
        val started = adapter.startPairing()
        _ui.update { it.copy(pairing = started) }
        diagnostics.event(
            "pairing", "start",
            "op=${started.operation} state=${started.state.wireName} reason=${started.reason.wireName}",
        )
        if (!started.active) {
            notice(pairingMessage(started))
            return@launch
        }
        var polls = 0
        var latest = started
        while (latest.active && polls++ < PAIRING_POLL_LIMIT) {
            delay(PAIRING_POLL_MILLIS)
            if (registry.activeId != adapterAtStart) {
                // The user switched adapters mid-operation. Stop describing an
                // operation that belongs to a different adapter entirely.
                _ui.update { it.copy(pairing = null) }
                return@launch
            }
            latest = runCatching { adapter.pairingStatus() }.getOrElse { error ->
                diagnostics.error("pairing", "status", error)
                _ui.update { it.copy(pairing = null) }
                return@launch
            }
            if (latest.operation != started.operation) {
                // A different operation is running now -- someone pressed the
                // adapter's own pairing button, or a second client started one.
                diagnostics.event("pairing", "superseded", "was=${started.operation} now=${latest.operation}")
                break
            }
            _ui.update { it.copy(pairing = latest) }
        }
        _ui.update { it.copy(pairing = latest.takeIf { s -> s.active }) }
        notice(pairingMessage(latest))
        // A controller that completed is a new peer; read the inventory so the
        // list shows it without the user refreshing.
        //
        // Read more than once. The adapter identifies a Classic controller in
        // two stages -- the HID descriptor binds a generic driver, then a PnP
        // SDP query returns the VID/PID that rebinds it to its real one -- and
        // pairing reports Paired at the first stage. A single read here lands
        // inside that gap, so the controller was recorded without its identity
        // and only a second pairing appeared to fix it. The adapter withholds a
        // provisional classification rather than publishing the fallback, so
        // "still absent" is exactly the signal to look again.
        if (latest.state == PairingState.Paired) {
            repeat(IDENTITY_SETTLE_READS) { attempt ->
                val ok = runCatching { readPeerInventory(explicit = false) }
                    .onFailure { diagnostics.error("peers", "refresh.after_pairing", it) }
                    .isSuccess
                // Stop as soon as every connected controller has an identity, or
                // if the read itself failed -- retrying a broken session buys
                // nothing. Bounded either way; this is cosmetic convergence, and
                // an explicit refresh always remains available.
                if (!ok || identityHasSettled()) return@repeat
                if (attempt < IDENTITY_SETTLE_READS - 1) delay(IDENTITY_SETTLE_MILLIS)
            }
        }
    }

    /**
     * Has every connected controller reported a classification?
     *
     * Absence is the adapter saying "not yet" rather than "unknown for good"
     * (see the firmware's mgmt_peers_classification_publishable), so this is a
     * convergence test, not a health check.
     */
    private fun identityHasSettled(): Boolean =
        _ui.value.snapshot.peers.controllers
            .filter { it.connected }
            .all { !it.classification.isNullOrBlank() }

    /**
     * Ask the adapter to close the window early.
     *
     * A courtesy, not a safety mechanism: the firmware's deadline is what
     * guarantees the window closes. Idempotent, so a cancel that races the
     * timeout reports idle rather than failing.
     */
    fun cancelControllerPairing() = launch("Stopping pairing") {
        val status = adapter.cancelPairing()
        _ui.update { it.copy(pairing = status.takeIf { s -> s.active }) }
        diagnostics.event("pairing", "cancel", "op=${status.operation} state=${status.state.wireName}")
    }

    /**
     * What to tell the user, per design §40: name the failure rather than
     * collapsing everything into "pairing failed".
     */
    private fun pairingMessage(status: PairingStatus): String = when (status.state) {
        PairingState.Paired -> "Controller paired."
        PairingState.TimedOut -> "No controller was paired. Put the controller in pairing mode and try again."
        PairingState.Cancelled -> "Pairing stopped."
        PairingState.Blocked -> when (status.reason) {
            PairingReason.Busy -> "The adapter is already pairing. Wait for it to finish."
            PairingReason.ManagementDisabled -> "Wireless management is off, so the adapter will not accept this."
            PairingReason.LockedOut -> "The adapter is clearing its pairings. Try again in a moment."
            // Points at the fix rather than just the fault: the list below is
            // exactly where the user forgets one to make room.
            PairingReason.StorageFull ->
                "The adapter has no room for another controller. Forget one below, then try again."
            else -> "The adapter would not start pairing."
        }
        PairingState.Idle -> "The adapter is not pairing."
        // Still running when the app stopped following it, or a state this
        // build does not recognise. Neither is a failure to report as one.
        PairingState.Discovering, PairingState.Connecting, PairingState.Unknown ->
            "Pairing is still running on the adapter."
    }

    /**
     * Forget one controller's pairing on the adapter.
     *
     * Deliberately distinct from [removePeerFromHistory]: this removes a
     * security credential and the controller must be paired again, while that
     * removes only what this app remembers. History is KEPT here on purpose --
     * a forgotten controller becomes a "Recent" row rather than vanishing, which
     * is what lets the user see what they did and what it used to be.
     *
     * The adapter's verified answer decides the message. An optimistic
     * "Forgotten" over a partial delete would be the one outcome the user cannot
     * detect for themselves.
     */
    fun forgetPeer(peerId: String, displayName: String) = launch("Forgetting $displayName") {
        val outcome = adapter.forgetPeer(peerId)
        diagnostics.event(
            "peers", "forget",
            "adapter=${registry.activeId?.shortLabel ?: "none"} result=${outcome.result.wireName} " +
                "stillBonded=${outcome.stillBonded}",
        )
        when (outcome.result) {
            PeerForgetResult.Removed ->
                notice("$displayName was forgotten. It will need to be paired again to reconnect.")
            // Not phrased as a failure: the end state the user asked for is the
            // end state the adapter is in.
            PeerForgetResult.AlreadyAbsent ->
                notice("$displayName was already not paired with this adapter.")
            PeerForgetResult.ManagementPeer ->
                notice("That is this phone's own connection to the adapter, not a controller. " +
                    "Use Repair pairing if you want to reset it.")
            PeerForgetResult.Incomplete ->
                notice("$displayName could not be fully forgotten; the adapter still has a " +
                    "pairing for it. The list has been refreshed with what it actually holds.")
            PeerForgetResult.Unknown ->
                notice("The adapter reported an outcome this app does not recognise. " +
                    "The list has been refreshed with what it actually holds.")
        }
    }

    /**
     * Forget one row of this app's own history.
     *
     * Not a Bluetooth operation and deliberately not offered under the same
     * word as forgetting a pairing: this removes what the app remembers about a
     * device the adapter has already stopped storing a key for. Selective
     * forget of a live pairing is Phase 5 and does not exist yet.
     */
    fun removePeerFromHistory(peerId: String) {
        val id = registry.activeId ?: return
        updatePeerHistory { book -> book.with(id, book.forAdapter(id).without(peerId)) }
        diagnostics.event("peers", "history.removed", "adapter=${id.shortLabel}")
    }
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

    /**
     * `null` destination restores this input's canonical binding.
     *
     * Legacy path. Used only when the adapter has no profile library: it writes
     * the realized mapping immediately, which is genuinely the only mapping
     * surface such firmware has. With a library open, edits go to the DRAFT.
     */
    fun bindKbm(profile: KbmProfile, source: KbmSource, destination: KbmDestination?) =
        kbmOperation("Updating binding") {
            adapter.bindKbm(profile, source, destination)
        }

    // ------------------------------------------------------------- profiles

    /** Open a profile for viewing and editing. Does NOT apply it. */
    fun openKbmProfile(profile: KbmProfileInfo) = kbmOperation("Opening ${profile.name}") {
        adapter.openKbmProfile(profile)
    }

    /**
     * Rebind one input in the open draft.
     *
     * Sends nothing. Thirty edits cost zero management commands and zero flash
     * erases, which is what makes Save and Discard mean anything.
     */
    fun editKbmBinding(source: KbmSource, destination: KbmDestination) =
        adapter.editKbmDraft { it.with(source, destination) }

    fun editKbmDraftName(name: String) = adapter.editKbmDraft { it.withName(name) }

    fun editKbmDraftMouse(mouse: KbmMouseConfig) =
        adapter.editKbmDraft { it.withMouse(mouse) }

    /** Throw the local edits away. Sends nothing. */
    fun discardKbmDraft() = adapter.discardKbmDraft()

    /**
     * SAVE. Stores the draft in the adapter's profile library.
     *
     * Deliberately does not apply it: the console keeps running whatever it was
     * running until the user presses Set Active.
     */
    fun saveKbmDraft() = kbmOperation("Saving profile") {
        val saved = adapter.saveKbmDraft()
        if (saved != null) notice("Saved '${saved.name}' — not applied yet")
    }

    /** APPLY. The only call here that changes what the console is doing. */
    fun applyKbmProfile(layout: KbmProfile, id: Int) = kbmOperation("Applying profile") {
        adapter.applyKbmProfile(layout, id)
        // Trust the readback, not the acknowledgement.
        val active = _ui.value.kbm.profiles.activeFor(layout)
        if (active?.sourceId != id) {
            notice("The adapter did not report this profile as active")
        }
    }

    fun renameKbmProfile(id: Int, name: String) = kbmOperation("Renaming profile") {
        adapter.renameKbmProfile(id, name)
    }

    fun duplicateKbmProfile(id: Int, name: String) = kbmOperation("Duplicating profile") {
        adapter.duplicateKbmProfile(id, name)
    }

    fun deleteKbmProfile(id: Int) = kbmOperation("Deleting profile") {
        adapter.deleteKbmProfile(id)
        notice("Profile deleted")
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
        val relationship = activeRelationship() ?: return null
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
        val hasRelationship = registry.active != null
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
                            hasRelationship = registry.active != null,
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
        val loaded = loadTouchProfile(_ui.value.snapshot.personality.current)
        if (loaded.profileId == null) {
            notice("Touch Gamepad needs a confirmed gameplay controller mode")
            return
        }
        bridge.enterTouchMode()
        applyTouchSettings(_ui.value.touchSettings)
        _ui.update {
            it.copy(
                touchGamepadActive = true,
                touchFaceLayout = inputBackend.requestedFaceLayout,
                touchProfileId = loaded.profileId,
                touchProfiles = loaded.library,
                touchLayoutDocument = loaded.document,
                touchLayoutWarning = loaded.warning,
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

    /**
     * Change the emulated controller from inside the Touch Gamepad.
     *
     * Deliberately the SAME [switchPersonality] the adapter screen calls, and
     * nothing else: the transition re-enumerates USB, may drop and re-establish
     * management, and reports its own failures. A second, Touch-only personality
     * state would be a second answer to "which controller is this" — and the one
     * the console believes is not the one a local flag would hold.
     *
     * Held input is released BEFORE the request goes out, while the link can
     * still carry a neutral report. Everything after that is the existing
     * lifecycle: the adapter snapshot arrives with the new personality, the
     * touch profile follows it, and the layout for that personality loads.
     */
    fun switchTouchPersonality(personality: Personality) {
        if (_ui.value.snapshot.personality.current == personality) return
        if (personality !in TouchProfileSelector.gameplayPersonalities) {
            notice("${personality.title} has no on-screen controller")
            return
        }
        bridge.releaseTouchInput(TouchReleaseReason.PersonalityChanged)
        session.neutralize()
        diagnostics.event("controller", "touch personality", "requested ${personality.wireName}")
        switchPersonality(personality)
    }

    /** Stop gameplay routing while the editor owns the surface. */
    fun beginTouchLayoutEdit() {
        bridge.releaseTouchInput(TouchReleaseReason.EditorEntered)
        session.neutralize()
    }

    /**
     * Commit an edited layout into a profile.
     *
     * [targetProfileId] is normally the active profile. Saving onto the
     * protected factory profile does not fail and does not overwrite it: the
     * library turns that into a new user profile named [newProfileName], which
     * is the only outcome that neither discards the user's work nor destroys the
     * one layout that is always supposed to be recoverable.
     */
    fun saveTouchLayout(
        value: TouchLayoutDocument,
        targetProfileId: String? = null,
        newProfileName: String = TouchProfileLibraryEditor.DEFAULT_NEW_PROFILE_NAME,
    ) {
        val active = _ui.value.touchProfileId ?: return
        val profile = TouchProfileCatalog.require(active)
        if (value.profileId != active || value.templateId != profile.defaultTemplate.id) {
            _ui.update { it.copy(touchLayoutWarning = "The edited layout no longer matches the active controller") }
            return
        }
        val library = _ui.value.touchProfiles ?: TouchProfileLibrary.empty(active)
        applyTouchProfileEdit(
            TouchProfileLibraryEditor.save(
                library = library,
                profileId = targetProfileId ?: library.selectedProfileId,
                document = value,
                nowEpochMs = System.currentTimeMillis(),
                newProfileName = newProfileName,
            ),
            "saved",
        )
    }

    /**
     * Put the shipped controller back on screen.
     *
     * Selects the factory profile rather than deleting anything: the user's own
     * profiles are still theirs, and "restore defaults" that quietly destroyed
     * them would be unrecoverable.
     */
    fun restoreTouchLayoutDefaults() {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(
            TouchProfileLibraryEditor.select(library, TouchProfileLibrary.FACTORY_PROFILE_ID),
            "restored the default profile",
        )
    }

    fun selectTouchProfile(profileId: String) {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(TouchProfileLibraryEditor.select(library, profileId), "selected")
    }

    fun createTouchProfile(name: String) {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(
            TouchProfileLibraryEditor.create(library, name, System.currentTimeMillis()),
            "created",
        )
    }

    fun duplicateTouchProfile(profileId: String, name: String? = null) {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(
            TouchProfileLibraryEditor.duplicate(library, profileId, name, System.currentTimeMillis()),
            "duplicated",
        )
    }

    fun renameTouchProfile(profileId: String, name: String) {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(TouchProfileLibraryEditor.rename(library, profileId, name), "renamed")
    }

    fun deleteTouchProfile(profileId: String) {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(TouchProfileLibraryEditor.delete(library, profileId), "deleted")
    }

    fun resetTouchProfile(profileId: String) {
        val library = _ui.value.touchProfiles ?: return
        applyTouchProfileEdit(
            TouchProfileLibraryEditor.resetToDefault(library, profileId, System.currentTimeMillis()),
            "reset",
        )
    }

    /**
     * A profile as a standalone document.
     *
     * The export/import foundation the editor architecture calls for: the
     * transport (a file, a share sheet, the management link) is a separate
     * decision, and this is the part of it that has to be stable.
     */
    fun exportTouchProfile(profileId: String): String? =
        _ui.value.touchProfiles?.profile(profileId)?.let(TouchProfileLibraryJsonCodec::encodeExport)

    fun importTouchProfile(raw: String) {
        val library = _ui.value.touchProfiles ?: return
        when (val decoded = TouchProfileLibraryJsonCodec.decodeExport(raw)) {
            is TouchProfileDecodeResult.Invalid -> notice(decoded.problem)
            is TouchProfileDecodeResult.Valid -> applyTouchProfileEdit(
                TouchProfileLibraryEditor.import(library, decoded.value, System.currentTimeMillis()),
                "imported",
            )
        }
    }

    /**
     * One place where a library edit becomes persisted, live state.
     *
     * Every one of these can change the geometry the console is being told
     * about, so each releases held input and neutralizes BEFORE the new layout
     * can be installed: a contact measured against the previous arrangement
     * means nothing once the arrangement changes, and leaving it held would send
     * the console a button the user is not pressing.
     */
    private fun applyTouchProfileEdit(edit: TouchProfileEdit, what: String) {
        when (edit) {
            is TouchProfileEdit.Rejected -> {
                // Both surfaces: the scaffold's message for the ordinary app, and
                // the layout warning, which is what the on-screen controller's own
                // menu shows. A refusal the user cannot see is a bug that reports
                // itself as "the button does nothing".
                notice(edit.reason)
                _ui.update { it.copy(touchLayoutWarning = edit.reason) }
                diagnostics.event("controller", "touch layout", "refused: ${edit.reason}")
                return
            }
            is TouchProfileEdit.Applied -> {
                bridge.releaseTouchInput(TouchReleaseReason.GeometryInvalidated)
                session.neutralize()
                touchProfileStore.save(edit.library)
                _ui.update {
                    it.copy(
                        touchProfiles = edit.library,
                        touchLayoutDocument = edit.library.activeDocument,
                        touchLayoutWarning = null,
                    )
                }
                // Counted, not sampled: the composition is what says whether the
                // layout the user is about to play is the one they built, and a
                // dropped control is otherwise invisible until they press it.
                val composition = TouchLayoutComposer.compose(
                    TouchProfileCatalog.require(edit.library.personality),
                    edit.library.activeDocument,
                )
                diagnostics.event(
                    "controller",
                    "touch layout",
                    buildString {
                        append(what)
                        append(" profile=").append(edit.library.personality.key)
                        append('/').append(edit.library.selectedProfileId)
                        append(" controls=").append(composition.layout.controls.size)
                        append(" groups=").append(edit.library.activeDocument.groups.size)
                        // Instances beyond one per binding: the number that says
                        // whether duplicate-safe aggregation is being exercised
                        // at all on this device.
                        append(" duplicates=").append(
                            composition.layout.controls.size -
                                composition.layout.controls.distinctBy { it.output }.size,
                        )
                        if (composition.degraded) append(" DEGRADED")
                        composition.warning?.let { append(" warning=").append(it) }
                    },
                )
            }
        }
    }

    /**
     * Push the settings screen's tunables into the engine.
     *
     * Copies the CURRENT config rather than rebuilding from
     * [TouchControlConfig.Default]: the gesture timings come from the platform's
     * own view configuration and are set by the touch surface, so rebuilding
     * here would quietly replace them with the stock constants every time a
     * slider moved.
     */
    private fun applyTouchSettings(settings: TouchGamepadSettings) {
        val current = bridge.touch.config
        bridge.touch.setConfig(
            current.copy(
                stickDeadzone = settings.stickDeadzone,
                latch = current.latch.copy(enabledByDefault = settings.doubleTapHold),
            ),
        )
    }

    private data class LoadedTouchProfile(
        val profileId: TouchProfileId?,
        val library: TouchProfileLibrary?,
        val document: TouchLayoutDocument?,
        val warning: String?,
    )

    private fun loadTouchProfile(personality: Personality): LoadedTouchProfile {
        val profileId = TouchProfileSelector.select(personality)
            ?: return LoadedTouchProfile(
                profileId = null,
                library = null,
                document = null,
                warning = when (personality) {
                    Personality.Config -> "Configuration mode has no gameplay touch profile"
                    else -> "The adapter's controller mode is not confirmed"
                },
            )
        val loaded = touchProfileStore.load(profileId)
        // A document that could not be read is the one failure here a user can
        // see and cannot explain: the controller comes back as the shipped
        // default and their own layout is simply gone from the picker.
        loaded.warning?.let {
            diagnostics.event("controller", "touch layout", "decode failed (${profileId.key}): $it")
        }
        return LoadedTouchProfile(
            profileId = profileId,
            library = loaded.library,
            document = loaded.library.activeDocument,
            warning = loaded.warning,
        )
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
            // The two ways a control can be down with nothing touching it, and
            // the one way a held control can still send a fresh press. All three
            // look identical in "held" and are completely different faults.
            // Armed-versus-engaged separates "the gesture is being offered and
            // nobody completes it" from "it completes when nobody meant it to",
            // and cancelled separates both from "it completes and the user keeps
            // taking it back", which is a gesture that is too easy to trip.
            append(" latched=").append(snapshot.latchedControls.size)
            append(" latch=").append(snapshot.latchesArmed)
            append("/").append(snapshot.latchesEngaged)
            append("/").append(snapshot.latchesCancelled)
            append("/").append(snapshot.latchesReleased)
            append("/").append(snapshot.latchesCleared)
            append(" retrigger=").append(snapshot.retriggerPulses)
            // An analog trigger held with nothing touching it looks identical to
            // one at rest in every counter above, so the LEVELS are named. The
            // two counters either side separate "the terminal click never fired"
            // from "a tap never produced a pull".
            append(" trigger=").append(snapshot.triggerDetents)
            append("/").append(snapshot.triggerPulses)
            snapshot.analogTriggers.filterValues { it > 0f }.toSortedMap().forEach { (id, level) ->
                append(' ').append(id).append('=').append("%.2f".format(level))
            }
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
            // Identities, never aliases: a diagnostic export is shared, and the
            // name a user gave a room in their home is not ours to include.
            "Known adapters" to ui.adapters.joinToString(", ") { record ->
                buildString {
                    append(record.id.shortLabel)
                    if (record.id == ui.activeAdapterId) append("*")
                    if (record.repairRequired) append(" repair")
                    append(" assoc=").append(ui.associationStates[record.id] ?: CompanionAssociationState.Unknown)
                }
            }.ifBlank { "none" },
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
                val id = AdapterId.fromAddress(decision.relationship.address)
                if (id != null) {
                    updateRegistry { current ->
                        current.update(id) { record ->
                            record.copy(associationId = decision.relationship.associationId ?: record.associationId)
                        }
                    }
                }
                diagnostics.event(
                    "relationship",
                    "association.metadata",
                    "adapter=${id?.shortLabel ?: "unknown"} association=${decision.relationship.associationId ?: "legacy"}",
                )
            }
            is AdapterLifecycleDecision.RepairRequired -> {
                // Repair is a property of one adapter. Marking it on the record
                // is what lets the adapter list show a per-unit badge instead of
                // putting the whole app into a repair state.
                registry.activeId?.let { id ->
                    updateRegistry { current -> current.update(id) { it.copy(repairRequired = true) } }
                }
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
                // The previous adapter's CompanionDeviceManager association used
                // to be deleted right here, whenever a connect landed on a
                // different association ID. That single call is what forced the
                // Forget/Pair cycle on anyone who owned two adapters: connecting
                // to the second unregistered the first. Adapters are a registry
                // now, and an adapter is unregistered only when the user asks.
                // Do not restore this.
                val record = adoptVerifiedAdapter(verified)
                // Settles the switch, if this connection was one. Guarded by
                // identity, so a result belonging to the adapter we switched
                // away from cannot settle the adapter we switched to.
                record?.id?.let(activeAdapter::activationSucceeded)
                publishAdapterState()
                _ui.update { it.copy(relationshipStatus = relationshipCoordinator.status) }
                diagnostics.event(
                    "relationship", "connect.verified",
                    "adapter=${record?.id?.shortLabel ?: "unknown"} attempt=${attempt.generation} " +
                        "reason=${attempt.reason.diagnosticName} association=${verified.associationId ?: "legacy"} " +
                        "known=${registry.records.size}",
                )
                // A fresh BLE management bond is not proof that the Classic HID-device path is
                // ready. Exercise it immediately while the user's Pico pairing window is still
                // open; the ownership read prevents stealing an already-active controller.
                requestAutomaticControllerResume()
                // One inventory read per verified session, quietly. History that
                // only advanced when the user pressed refresh would almost never
                // advance, and "Recent" would stay permanently empty; and reading
                // it here is what lets a rebooted adapter still show readable
                // controller names. Deliberately not routed through launch(): it
                // is not a user action and must not raise the modal progress
                // overlay over a connection that just succeeded.
                try {
                    // Learn what this firmware can do before the UI offers it.
                    // Ordered before the inventory read so a Forget button is
                    // never drawn against firmware that would answer `unknown
                    // command` to it.
                    adapter.probeManagementCapabilities()
                    readPeerInventory(explicit = false)
                } catch (cancelled: kotlinx.coroutines.CancellationException) {
                    // Never swallowed: this runs inside the connect job, whose
                    // own handler re-throws cancellation so the attempt unwinds.
                    throw cancelled
                } catch (error: Throwable) {
                    // A failed inventory read is not a failed connection. The
                    // session is already verified at this point and must not be
                    // torn down because an optional read did not answer.
                    diagnostics.error("peers", "refresh.session", error)
                }
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                diagnostics.event("relationship", "connect.cancelled", "attempt=${attempt.generation}")
                throw cancelled
            } catch (error: Throwable) {
                // The adapter erases its bonds on every firmware install, by
                // design. From here that appears as a connect-stage key failure
                // against a device Android still considers bonded, and no number
                // of retries can fix it -- the adapter has no key to
                // authenticate with. Name it and go straight to repair.
                val stillBonded = bondState(attempt.relationship.address) == AndroidBondState.Bonded
                val bondMismatch = AdapterResetSignature.isBondMismatch(error, stillBonded)
                val message = error.message ?: "The adapter connection could not be verified."
                val decision = relationshipCoordinator.connectionFailed(
                    attempt.generation,
                    message,
                    bondMismatch = bondMismatch,
                )
                // Settle the switch at "selected, not connected" for the adapter
                // the user chose. There is deliberately no fallback to the
                // previous adapter: reconnecting something other than what was
                // asked for, while the UI names the choice, is the lie this
                // whole transition design exists to prevent.
                AdapterId.fromAddress(attempt.relationship.address)?.let {
                    activeAdapter.activationFailed(it, message)
                }
                publishAdapterState()
                publishRelationshipStatus()
                if (bondMismatch) {
                    diagnostics.event(
                        "relationship", "repair.adapter_reset",
                        "attempt=${attempt.generation} bond=Bonded; adapter no longer holds the link key",
                    )
                    notice(AdapterResetSignature.REPAIR_MESSAGE)
                } else {
                    diagnostics.error("relationship", "connect attempt ${attempt.generation}", error)
                    notice(error.message ?: "The adapter connection could not be verified.")
                }
                if (decision is AdapterLifecycleDecision.RepairRequired) {
                    diagnostics.event("relationship", "repair.required", decision.message)
                }
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

    /**
     * Retire the live session belonging to one adapter and drop its association.
     *
     * [retainRegistryEntry] is what separates Repair from Remove: repair keeps
     * the row so alias, cached state and history survive and the repaired unit
     * rebinds to it, while remove has already deleted the row before getting
     * here. Either way the Android bond and the adapter's own bonds are left
     * alone; the app cannot delete the platform bond and must not pretend to.
     */
    private fun clearOwnedRelationship(
        relationship: AdapterRelationship?,
        retainRegistryEntry: Boolean = false,
        onComplete: (() -> Unit)? = null,
    ) {
        relationshipPairingJob?.cancel()
        relationshipConnectionJob?.cancel()
        pairingDevice = null
        autoReconnectAttempted = false
        relationshipCoordinator.forget()
        _ui.update {
            it.copy(
                relationshipStatus = relationshipCoordinator.status,
                connection = ConnectionState(),
            )
        }
        publishAdapterState()
        diagnostics.event(
            "relationship", "relationship.clear",
            "adapter=${AdapterId.fromAddress(relationship?.address)?.shortLabel ?: "none"} " +
                "registryEntry=${if (retainRegistryEntry) "retained" else "removed"}; " +
                "Android bond and adapter bonds retained",
        )
        val priorRetirement = relationshipRetirementJob
        relationshipRetirementJob = viewModelScope.launch {
            if (priorRetirement?.isActive == true) {
                priorRetirement.join()
            } else {
                runCatching { adapter.disconnect() }
                    .onFailure { diagnostics.error("management", "relationship clear disconnect", it) }
            }
            relationship?.let(::disassociate)
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
    // A "clear every app-owned association" helper used to live here, invoked by
    // Forget and by ambiguous Repair. It was correct while the app could own
    // exactly one adapter and every extra record was therefore stale. With a
    // registry it is not: every other adapter the user owns has an association
    // too, and clearing them all would silently unregister hardware the user
    // never touched. Both callers now disassociate one adapter by identity.
    // Do not reintroduce it.

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
        // Pairing progress polling. One second reads as live without flooding a
        // single-flight management carrier, and the limit is a backstop only:
        // the loop normally ends because the ADAPTER says the window closed.
        // Sized well past the firmware's 30 s window so the backstop never
        // fires first and reports "still running" for a finished operation.
        private const val PAIRING_POLL_MILLIS = 1_000L
        private const val PAIRING_POLL_LIMIT = 45

        // Re-reads allowed after pairing completes, while the adapter finishes
        // identifying a Classic controller (HID descriptor first, PnP SDP
        // after). Small and bounded on purpose: this only makes the list
        // correct without a manual refresh, and an unidentified controller is
        // still fully usable.
        private const val IDENTITY_SETTLE_READS = 4
        private const val IDENTITY_SETTLE_MILLIS = 1_000L
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
