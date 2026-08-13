package dev.picoswitch.companion.ui

import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothDevice
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.picoswitch.companion.controller.*
import dev.picoswitch.companion.data.*
import dev.picoswitch.companion.model.*
import dev.picoswitch.companion.transport.BleGattManagementTransport
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch

enum class AppSection(val label: String) { Home("Home"), Amiibo("Amiibo"), Controller("Controller"), Modes("Adapter"), More("More") }

data class CompanionUiState(
    val section: AppSection = AppSection.Home,
    val connection: ConnectionState = ConnectionState(),
    val snapshot: AdapterSnapshot = AdapterSnapshot(),
    val library: List<AmiiboLibraryItem> = emptyList(),
    val selectedAmiiboId: String? = null,
    val operation: OperationProgress? = null,
    val busy: Boolean = false,
    val message: String? = null,
    val bridge: BridgeState = BridgeState(),
    val controllerState: ControllerState = ControllerState.Neutral,
    val sourceDevices: List<SourceDeviceUi> = emptyList(),
    val selectedSourceDescriptor: String? = null,
)

data class SourceDeviceUi(val id: Int, val descriptor: String, val name: String, val vendorId: Int, val productId: Int)

class CompanionViewModel(application: Application) : AndroidViewModel(application) {
    val inputRouter = AndroidInputRouter()
    val hidBridge = HidDeviceBridge(application, inputRouter)
    private val adapter = AdapterRepository(BleGattManagementTransport(application))
    private val library = AmiiboLibrary(application)
    private val _ui = MutableStateFlow(CompanionUiState())
    val ui: StateFlow<CompanionUiState> = _ui.asStateFlow()

    init {
        viewModelScope.launch { adapter.connection.collect { value -> _ui.update { it.copy(connection = value) } } }
        viewModelScope.launch { adapter.snapshot.collect { value -> _ui.update { it.copy(snapshot = value) } } }
        viewModelScope.launch { library.items.collect { value -> _ui.update { old -> old.copy(library = value, selectedAmiiboId = old.selectedAmiiboId ?: value.firstOrNull()?.id) } } }
        viewModelScope.launch { hidBridge.state.collect { value -> _ui.update { it.copy(bridge = value) } } }
        viewModelScope.launch { inputRouter.state.collect { value -> _ui.update { it.copy(controllerState = value) } } }
        refreshSources()
    }

    fun navigate(section: AppSection) { _ui.update { it.copy(section = section, message = null) } }
    fun consumeMessage() { _ui.update { it.copy(message = null) } }
    fun selectAmiibo(id: String) { _ui.update { it.copy(selectedAmiiboId = id) } }

    fun connect() = launch("Connecting to PicoSwitch2") { adapter.connect() }
    fun disconnect() = launch("Disconnecting") { adapter.disconnect() }
    fun refresh() = launch("Refreshing adapter") { adapter.refreshAll() }
    fun wake() = launch("Requesting console wake") { adapter.wakeConsole(); notice("Wake request queued") }

    fun switchPersonality(personality: Personality) = launch("Switching adapter mode") {
        val reenumerating = adapter.setPersonality(personality)
        notice(if (reenumerating) "Mode changed. USB is re-enumerating; controller and audio may pause briefly." else "Already using ${personality.title}")
    }

    fun saveColor(target: ColorTarget, color: RgbColor) = launch("Saving color") {
        adapter.setColor(target, color)
        notice("Color saved. Reconnect or re-enumerate USB before expecting the console to refresh identity colors.")
    }

    fun setManagement(enabled: Boolean) = launch("Updating management access") {
        adapter.setManagementEnabled(enabled)
        notice(if (enabled) "Wireless management enabled for this boot" else "Wireless management disabled; this connection may close")
    }

    fun importAmiibo(uri: Uri, displayName: String) = launch("Importing Amiibo") {
        val resolver = getApplication<Application>().contentResolver
        val bytes = resolver.openInputStream(uri)?.use { it.readBytes() } ?: error("Could not read selected file")
        val name = uri.lastPathSegment?.substringAfterLast('/') ?: "amiibo.bin"
        val item = library.import(displayName, name, bytes)
        _ui.update { it.copy(selectedAmiiboId = item.id, section = AppSection.Amiibo) }
        notice("Imported ${item.displayName}")
    }

    fun loadSelectedAmiibo() = launch("Uploading Amiibo") {
        val id = requireNotNull(_ui.value.selectedAmiiboId) { "Select an Amiibo first" }
        adapter.uploadAmiibo(library.bytes(id)) { progress -> _ui.update { it.copy(operation = progress) } }
        notice("Amiibo loaded and saved on the adapter")
    }

    fun syncSelectedAmiibo() = launch("Syncing Amiibo") {
        val bytes = adapter.syncAmiibo { progress -> _ui.update { it.copy(operation = progress) } }
        val item = library.updateFromAdapter(_ui.value.selectedAmiiboId, bytes)
        _ui.update { it.copy(selectedAmiiboId = item.id) }
        notice("Synced console-written data into ${item.displayName}")
    }

    fun setPresented(value: Boolean) = launch(if (value) "Presenting Amiibo" else "Ejecting Amiibo") { adapter.setPresented(value) }
    fun selectCopy(used: Boolean) = launch("Selecting Amiibo copy") { adapter.selectAmiiboCopy(used) }
    fun clearAdapterAmiibo() = launch("Clearing adapter Amiibo") { adapter.clearAmiibo(); notice("Adapter Amiibo cleared") }
    fun deleteSelectedAmiibo() = launch("Deleting local Amiibo") {
        val id = _ui.value.selectedAmiiboId ?: return@launch
        library.delete(id)
        _ui.update { it.copy(selectedAmiiboId = it.library.firstOrNull { item -> item.id != id }?.id) }
    }

    fun refreshSources() {
        val devices = inputRouter.eligibleDevices()
        _ui.update { state ->
            state.copy(sourceDevices = devices.map { SourceDeviceUi(it.id, it.descriptor, it.name, it.vendorId, it.productId) })
        }
    }

    fun selectSource(descriptor: String) {
        val device = inputRouter.eligibleDevices().firstOrNull { it.descriptor == descriptor }
        inputRouter.select(device)
        _ui.update { it.copy(selectedSourceDescriptor = descriptor) }
    }

    fun acquireControllerBridge() = hidBridge.acquire()
    fun pairedControllerHosts(): List<BluetoothDevice> = hidBridge.pairedHosts()
    @SuppressLint("MissingPermission")
    fun controllerHostLabel(device: BluetoothDevice): String = runCatching { device.name }.getOrNull() ?: device.address
    fun connectControllerHost(device: BluetoothDevice) = hidBridge.connect(device)
    fun stopControllerBridge() = hidBridge.stop()
    fun neutralizeController() = hidBridge.neutralize()

    private fun launch(label: String, action: suspend () -> Unit) {
        if (_ui.value.busy) return
        viewModelScope.launch {
            _ui.update { it.copy(busy = true, operation = OperationProgress(label, 0, 0), message = null) }
            runCatching { action() }.onFailure { error -> notice(error.message ?: "Operation failed") }
            _ui.update { it.copy(busy = false, operation = null) }
        }
    }

    private fun notice(message: String) { _ui.update { it.copy(message = message) } }

    override fun onCleared() {
        hidBridge.stop()
        super.onCleared()
    }
}
