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
import dev.picoswitch.companion.controller.*
import dev.picoswitch.companion.data.*
import dev.picoswitch.companion.diagnostics.DiagnosticEntry
import dev.picoswitch.companion.diagnostics.DiagnosticLog
import dev.picoswitch.companion.diagnostics.DiagnosticSummary
import dev.picoswitch.companion.model.*
import dev.picoswitch.companion.transport.BleGattManagementTransport
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest

enum class AppSection(val label: String) { Home("Home"), Amiibo("Amiibo"), Controller("Input"), Modes("Adapter"), More("More") }

data class PlatformDiagnostics(
    val bluetoothAvailable: Boolean = false,
    val bluetoothEnabled: Boolean = false,
    val scanPermission: Boolean = false,
    val connectPermission: Boolean = false,
    val companionDeviceManager: Boolean = false,
)

data class CompanionUiState(
    val section: AppSection = AppSection.Home,
    val connection: ConnectionState = ConnectionState(),
    val snapshot: AdapterSnapshot = AdapterSnapshot(),
    val library: List<AmiiboLibraryItem> = emptyList(),
    val libraryWarnings: List<String> = emptyList(),
    val selectedAmiiboId: String? = null,
    val operation: OperationProgress? = null,
    val busy: Boolean = false,
    val message: String? = null,
    val bridge: BridgeState = BridgeState(),
    val controllerState: ControllerState = ControllerState.Neutral,
    val sourceDevices: List<SourceDeviceUi> = emptyList(),
    val selectedSourceDescriptor: String? = null,
    val platform: PlatformDiagnostics = PlatformDiagnostics(),
    val diagnosticSummary: DiagnosticSummary = DiagnosticSummary(),
    val diagnosticEntries: List<DiagnosticEntry> = emptyList(),
    val identityRefreshPending: Boolean = false,
)

data class SourceDeviceUi(val id: Int, val descriptor: String, val name: String, val vendorId: Int, val productId: Int)

class CompanionViewModel(application: Application, private val savedState: SavedStateHandle) : AndroidViewModel(application) {
    val diagnostics = DiagnosticLog()
    val inputRouter = AndroidInputRouter()
    val hidBridge = HidDeviceBridge(application, inputRouter, diagnostics)
    private val adapter = AdapterRepository(BleGattManagementTransport(application, diagnostics))
    private val library = AmiiboLibrary(application)
    private val initialSection = savedState.get<String>(KEY_SECTION)?.let { runCatching { AppSection.valueOf(it) }.getOrNull() } ?: AppSection.Home
    private val _ui = MutableStateFlow(
        CompanionUiState(
            section = initialSection,
            selectedAmiiboId = savedState[KEY_AMIIBO],
            selectedSourceDescriptor = savedState[KEY_SOURCE],
            identityRefreshPending = savedState[KEY_IDENTITY_PENDING] ?: false,
        ),
    )
    val ui: StateFlow<CompanionUiState> = _ui.asStateFlow()

    init {
        diagnostics.event("app", "created", "version ${BuildConfig.VERSION_NAME}")
        viewModelScope.launch { adapter.connection.collect { value -> _ui.update { it.copy(connection = value) } } }
        viewModelScope.launch { adapter.snapshot.collect { value -> _ui.update { it.copy(snapshot = value) } } }
        viewModelScope.launch {
            library.items.collect { value ->
                _ui.update { old ->
                    val selected = old.selectedAmiiboId?.takeIf { id -> value.any { it.id == id } } ?: value.firstOrNull()?.id
                    savedState[KEY_AMIIBO] = selected
                    old.copy(library = value, selectedAmiiboId = selected)
                }
            }
        }
        viewModelScope.launch { library.warnings.collect { value -> _ui.update { it.copy(libraryWarnings = value) } } }
        viewModelScope.launch { hidBridge.state.collect { value -> _ui.update { it.copy(bridge = value) } } }
        viewModelScope.launch { inputRouter.state.collect { value -> _ui.update { it.copy(controllerState = value) } } }
        viewModelScope.launch { diagnostics.summary.collect { value -> _ui.update { it.copy(diagnosticSummary = value) } } }
        viewModelScope.launch { diagnostics.entries.collect { value -> _ui.update { it.copy(diagnosticEntries = value) } } }
        viewModelScope.launch {
            adapter.connection.map { it.connected }.distinctUntilChanged().collectLatest { connected ->
                while (connected) {
                    delay(ADAPTER_POLL_MILLIS)
                    val state = _ui.value
                    if (!state.busy) {
                        runCatching { adapter.refreshController() }
                            .onFailure { diagnostics.error("management", "background controller refresh", it) }
                        if (state.snapshot.capabilities.amiibo == CapabilityState.Available) {
                            runCatching { adapter.refreshAmiibo() }
                                .onFailure { diagnostics.error("management", "background Amiibo refresh", it) }
                        }
                    }
                }
            }
        }
        refreshPlatformDiagnostics()
        refreshSources()
    }

    fun navigate(section: AppSection) {
        savedState[KEY_SECTION] = section.name
        _ui.update { it.copy(section = section, message = null) }
    }
    fun consumeMessage() { _ui.update { it.copy(message = null) } }
    fun selectAmiibo(id: String) { savedState[KEY_AMIIBO] = id; _ui.update { it.copy(selectedAmiiboId = id) } }

    fun connect() = launch("Connecting to PicoSwitch2") { adapter.connect() }
    fun disconnect() = launch("Disconnecting") { adapter.disconnect() }
    fun refresh() = launch("Refreshing adapter") { adapter.refreshAll() }
    fun wake() = launch("Requesting console wake") { adapter.wakeConsole(); notice("Wake request queued") }

    fun switchPersonality(personality: Personality) = launch("Switching adapter mode") {
        val reenumerating = adapter.setPersonality(personality)
        if (reenumerating) markIdentityRefreshPending("personality switch")
        notice(if (reenumerating) "Mode changed. USB is re-enumerating; controller and audio may pause briefly." else "Already using ${personality.title}")
    }

    fun saveColor(target: ColorTarget, color: RgbColor) = launch("Saving color") {
        adapter.setColor(target, color)
        markIdentityRefreshPending("identity color saved")
        notice("Color saved. Reconnect or re-enumerate USB before expecting the console to refresh identity colors.")
    }

    fun clearIdentityRefreshPending() {
        savedState[KEY_IDENTITY_PENDING] = false
        _ui.update { it.copy(identityRefreshPending = false) }
        diagnostics.event("adapter", "identity refresh acknowledged")
    }

    fun setManagement(enabled: Boolean) = launch("Updating management access") {
        adapter.setManagementEnabled(enabled)
        notice(if (enabled) "Wireless management enabled for this boot" else "Wireless management disabled; this connection may close")
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
        notice(if (result.duplicate) "That exact backup is already in the library" else "Imported ${result.item.displayName}")
    }

    fun loadSelectedAmiibo() = launch("Uploading Amiibo") {
        val id = requireNotNull(_ui.value.selectedAmiiboId) { "Select an Amiibo first" }
        adapter.uploadAmiibo(library.bytes(id)) { progress -> _ui.update { it.copy(operation = progress) } }
        notice("Amiibo loaded and saved on the adapter")
    }

    fun syncSelectedAmiibo() = launch("Syncing Amiibo") {
        val download = adapter.downloadAmiibo { progress -> _ui.update { it.copy(operation = progress) } }
        // Dirty protection is acknowledged only after the private file and index are durable.
        val item = library.updateFromAdapter(_ui.value.selectedAmiiboId, download.bytes)
        adapter.acknowledgeDownloadedAmiibo(download)
        _ui.update { it.copy(selectedAmiiboId = item.id) }
        savedState[KEY_AMIIBO] = item.id
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

    fun removeBond(index: Int) = launch("Removing management bond") {
        adapter.removeBond(index)
        notice("Management bond removed")
    }

    fun refreshSources() {
        val devices = inputRouter.eligibleDevices()
        val desired = savedState.get<String>(KEY_SOURCE)
        if (inputRouter.selectedDescriptor != null && devices.none { it.descriptor == inputRouter.selectedDescriptor }) {
            inputRouter.select(null)
        }
        if (inputRouter.selectedDescriptor == null && desired != null) inputRouter.select(devices.firstOrNull { it.descriptor == desired })
        _ui.update { state ->
            state.copy(
                sourceDevices = devices.map { SourceDeviceUi(it.id, it.descriptor, it.name.take(120), it.vendorId, it.productId) },
                selectedSourceDescriptor = inputRouter.selectedDescriptor,
            )
        }
    }

    fun selectSource(descriptor: String) {
        val device = inputRouter.eligibleDevices().firstOrNull { it.descriptor == descriptor }
        inputRouter.select(device)
        savedState[KEY_SOURCE] = device?.descriptor
        _ui.update { it.copy(selectedSourceDescriptor = device?.descriptor) }
    }

    fun acquireControllerBridge() = hidBridge.acquire()
    fun pairedControllerHosts(): List<BluetoothDevice> = hidBridge.pairedHosts()
    @SuppressLint("MissingPermission")
    fun controllerHostLabel(device: BluetoothDevice): String = runCatching { device.name }.getOrNull()?.take(80) ?: "saved adapter"
    fun connectControllerHost(device: BluetoothDevice) = hidBridge.connect(device)
    fun stopControllerBridge() = hidBridge.stop()
    fun neutralizeController() = hidBridge.neutralize()

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
        val ui = _ui.value
        val directory = File(getApplication<Application>().cacheDir, "diagnostics").apply { mkdirs() }
        val file = File(directory, "picoswitch-companion-diagnostics.txt")
        val descriptorHash = MessageDigest.getInstance("SHA-256").digest(AndroidControllerDescriptor.bytes)
            .joinToString("") { "%02x".format(it) }
        val report = diagnostics.export(linkedMapOf(
            "App" to BuildConfig.VERSION_NAME,
            "Android" to "${Build.VERSION.RELEASE} API ${Build.VERSION.SDK_INT}",
            "Device" to "${Build.MANUFACTURER} ${Build.MODEL}",
            "Bluetooth available/enabled" to "${ui.platform.bluetoothAvailable}/${ui.platform.bluetoothEnabled}",
            "Permissions scan/connect" to "${ui.platform.scanPermission}/${ui.platform.connectPermission}",
            "CompanionDeviceManager" to ui.platform.companionDeviceManager.toString(),
            "Management state" to ui.connection.phase.name,
            "Firmware" to ui.snapshot.firmware.version.ifBlank { "unknown" },
            "Capabilities" to ui.snapshot.capabilities.toString(),
            "HID state/registered" to "${ui.bridge.phase}/${ui.bridge.registered}",
            "HID descriptor" to "${AndroidControllerDescriptor.bytes.size} bytes sha256=$descriptorHash",
            "Saved HID hosts" to pairedControllerHosts().size.toString(),
            "Reports" to "${ui.bridge.reportCount}; last=${ui.bridge.lastReportAtMillis}",
            "Controller state" to ControllerReportEncoder.encode(ui.controllerState).joinToString(" ") { "%02X".format(it) },
            "Identity refresh pending" to ui.identityRefreshPending.toString(),
        ))
        file.writeText(report)
        diagnostics.event("app", "diagnostics exported")
        return file
    }

    private fun launch(label: String, action: suspend () -> Unit) {
        if (_ui.value.busy) return
        viewModelScope.launch {
            _ui.update { it.copy(busy = true, operation = OperationProgress(label, 0, 0), message = null) }
            runCatching { action() }.onFailure { error -> diagnostics.error("app", label, error); notice(error.message ?: "Operation failed") }
            _ui.update { it.copy(busy = false, operation = null) }
        }
    }

    private fun markIdentityRefreshPending(reason: String) {
        savedState[KEY_IDENTITY_PENDING] = true
        _ui.update { it.copy(identityRefreshPending = true) }
        diagnostics.event("adapter", "re-enumeration required", reason)
    }

    private fun notice(message: String) { _ui.update { it.copy(message = message) } }

    override fun onCleared() {
        hidBridge.close()
        adapter.close()
        super.onCleared()
    }

    companion object {
        private const val MAX_IMPORT_BYTES = 2048
        private const val ADAPTER_POLL_MILLIS = 5_000L
        private const val KEY_SECTION = "section"
        private const val KEY_AMIIBO = "selectedAmiibo"
        private const val KEY_SOURCE = "selectedSource"
        private const val KEY_IDENTITY_PENDING = "identityRefreshPending"
    }
}
