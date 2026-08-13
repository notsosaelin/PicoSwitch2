package dev.picoswitch.companion.controller

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import java.util.concurrent.Executors

enum class BridgePhase { Idle, AcquiringProfile, Registering, Ready, Connecting, Playing, Unsupported, Failed }
data class BridgeState(val phase: BridgePhase = BridgePhase.Idle, val hostName: String? = null, val message: String? = null)

@SuppressLint("MissingPermission")
class HidDeviceBridge(context: Context, private val input: AndroidInputRouter) : BluetoothProfile.ServiceListener {
    private val appContext = context.applicationContext
    private val manager = appContext.getSystemService(BluetoothManager::class.java)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val executor = Executors.newSingleThreadExecutor()
    private val _state = MutableStateFlow(BridgeState())
    val state: StateFlow<BridgeState> = _state.asStateFlow()
    private var profile: BluetoothHidDevice? = null
    private var host: BluetoothDevice? = null
    private var sender: Job? = null

    private val callback = object : BluetoothHidDevice.Callback() {
        override fun onAppStatusChanged(pluggedDevice: BluetoothDevice?, registered: Boolean) {
            if (registered) _state.value = BridgeState(BridgePhase.Ready, pluggedDevice?.name, "Select a paired PicoSwitch2 host")
            else if (_state.value.phase != BridgePhase.Idle) _state.value = BridgeState(BridgePhase.Failed, message = "Android rejected HID Device registration")
        }

        override fun onConnectionStateChanged(device: BluetoothDevice, state: Int) {
            when (state) {
                BluetoothProfile.STATE_CONNECTING -> _state.value = BridgeState(BridgePhase.Connecting, device.name)
                BluetoothProfile.STATE_CONNECTED -> {
                    host = device
                    _state.value = BridgeState(BridgePhase.Playing, device.name, "Input is streaming while this app stays in front")
                    startSender()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    sender?.cancel(); sender = null; host = null
                    input.neutralize()
                    _state.value = BridgeState(BridgePhase.Ready, message = "Controller link disconnected")
                }
            }
        }
    }

    fun acquire() {
        if (profile != null) return
        _state.value = BridgeState(BridgePhase.AcquiringProfile, message = "Checking Android HID Device support")
        val ok = manager?.adapter?.getProfileProxy(appContext, this, BluetoothProfile.HID_DEVICE) == true
        if (!ok) _state.value = BridgeState(BridgePhase.Unsupported, message = "This Android build does not expose the HID Device profile")
    }

    override fun onServiceConnected(profileId: Int, proxy: BluetoothProfile) {
        val hid = proxy as? BluetoothHidDevice ?: return
        profile = hid
        _state.value = BridgeState(BridgePhase.Registering)
        val sdp = BluetoothHidDeviceAppSdpSettings(
            "PicoSwitch Android Controller", "Built-in controls passthrough", "PicoSwitch2",
            (BluetoothHidDevice.SUBCLASS1_COMBO.toInt() or BluetoothHidDevice.SUBCLASS2_GAMEPAD.toInt()).toByte(),
            AndroidControllerDescriptor.bytes,
        )
        if (!hid.registerApp(sdp, null, null, executor, callback)) {
            _state.value = BridgeState(BridgePhase.Failed, message = "Another HID Device app may already be active")
        }
    }

    override fun onServiceDisconnected(profileId: Int) {
        profile = null
        sender?.cancel(); sender = null; input.neutralize()
        _state.value = BridgeState(BridgePhase.Unsupported, message = "Android HID Device service became unavailable")
    }

    fun pairedHosts(): List<BluetoothDevice> = runCatching {
        manager?.adapter?.bondedDevices.orEmpty().filter {
            val name = it.name.orEmpty()
            name.contains("PicoSwitch", true) || name.contains("Joypad Adapter", true)
        }
    }.getOrDefault(emptyList())

    fun connect(device: BluetoothDevice) {
        val hid = profile ?: return run { _state.value = BridgeState(BridgePhase.Failed, message = "HID profile is not ready") }
        _state.value = BridgeState(BridgePhase.Connecting, device.name)
        if (!hid.connect(device)) _state.value = BridgeState(BridgePhase.Failed, device.name, "Android could not start the HID connection")
    }

    fun stop() {
        val hid = profile
        val device = host
        if (hid != null && device != null) {
            hid.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(ControllerState.Neutral))
            hid.disconnect(device)
        }
        sender?.cancel(); sender = null; host = null; input.neutralize()
        if (hid != null) hid.unregisterApp()
        runCatching { manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid) }
        profile = null
        _state.value = BridgeState()
    }

    fun neutralize() {
        val device = host ?: return
        input.neutralize()
        profile?.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(ControllerState.Neutral))
    }

    private fun startSender() {
        sender?.cancel()
        sender = scope.launch {
            input.state.sample(8).distinctUntilChanged().collectLatest { state ->
                val device = host ?: return@collectLatest
                val ok = profile?.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(state)) == true
                if (!ok) _state.value = _state.value.copy(message = "Android rejected the latest input report")
            }
        }
    }
}
