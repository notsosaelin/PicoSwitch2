package dev.picoswitch.companion.controller

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.os.Handler
import android.os.Looper
import dev.picoswitch.companion.diagnostics.DiagnosticLog
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import java.util.concurrent.Executors

enum class BridgePhase { Idle, AcquiringProfile, Registering, Ready, Connecting, Playing, Unsupported, Failed }
data class BridgeState(
    val phase: BridgePhase = BridgePhase.Idle,
    val hostName: String? = null,
    val message: String? = null,
    val registered: Boolean = false,
    val reportCount: Long = 0,
    val lastReportAtMillis: Long = 0,
)

@SuppressLint("MissingPermission")
class HidDeviceBridge(
    context: Context,
    private val input: AndroidInputRouter,
    private val diagnostics: DiagnosticLog? = null,
) : BluetoothProfile.ServiceListener {
    private val appContext = context.applicationContext
    private val manager = appContext.getSystemService(BluetoothManager::class.java)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val executor = Executors.newSingleThreadExecutor()
    private val _state = MutableStateFlow(BridgeState())
    val state: StateFlow<BridgeState> = _state.asStateFlow()
    private var profile: BluetoothHidDevice? = null
    private var host: BluetoothDevice? = null
    private var sender: Job? = null
    private var inputCollector: Job? = null
    // One latest-state mailbox prevents old motion reports surviving behind newer input.
    private val outgoing = LatestReportMailbox<ControllerState>()
    @Volatile private var stopped = false

    private val callback = object : BluetoothHidDevice.Callback() {
        override fun onAppStatusChanged(pluggedDevice: BluetoothDevice?, registered: Boolean) {
            if (stopped) return
            diagnostics?.event("controller", "HID registration", if (registered) "registered" else "unregistered")
            if (registered) _state.value = _state.value.copy(
                phase = BridgePhase.Ready, hostName = pluggedDevice?.name,
                message = "Select a paired PicoSwitch2 host", registered = true,
            ) else if (_state.value.phase != BridgePhase.Idle) _state.value = _state.value.copy(
                phase = BridgePhase.Failed, message = "Android removed HID Device registration; keep the app in front and prepare it again",
                registered = false,
            )
        }

        override fun onConnectionStateChanged(device: BluetoothDevice, state: Int) {
            if (stopped) return
            when (state) {
                BluetoothProfile.STATE_CONNECTING -> _state.value = BridgeState(BridgePhase.Connecting, device.name)
                BluetoothProfile.STATE_CONNECTED -> {
                    host = device
                    _state.value = _state.value.copy(
                        phase = BridgePhase.Playing, hostName = device.name,
                        message = "Input is streaming while this app stays in front", registered = true,
                    )
                    diagnostics?.event("controller", "host connected", "HID interrupt link ready")
                    startSender()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    sender?.cancel(); sender = null; inputCollector?.cancel(); inputCollector = null
                    drainOutgoing(); host = null
                    input.neutralize()
                    _state.value = _state.value.copy(
                        phase = BridgePhase.Ready, hostName = null, message = "Controller link disconnected", registered = true,
                    )
                    diagnostics?.event("controller", "host disconnected")
                }
            }
        }
    }

    fun acquire() {
        stopped = false
        profile?.let { register(it); return }
        _state.value = BridgeState(BridgePhase.AcquiringProfile, message = "Checking Android HID Device support")
        diagnostics?.event("controller", "HID profile", "acquiring")
        val ok = manager?.adapter?.getProfileProxy(appContext, this, BluetoothProfile.HID_DEVICE) == true
        if (!ok) _state.value = BridgeState(BridgePhase.Unsupported, message = "This Android build does not expose the HID Device profile")
    }

    override fun onServiceConnected(profileId: Int, proxy: BluetoothProfile) {
        if (stopped) {
            manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, proxy)
            return
        }
        val hid = proxy as? BluetoothHidDevice ?: return
        profile = hid
        register(hid)
    }

    private fun register(hid: BluetoothHidDevice) {
        _state.value = BridgeState(BridgePhase.Registering)
        val sdp = BluetoothHidDeviceAppSdpSettings(
            "PicoSwitch Android Controller", "Built-in controls passthrough", "PicoSwitch2",
            (BluetoothHidDevice.SUBCLASS1_COMBO.toInt() or BluetoothHidDevice.SUBCLASS2_GAMEPAD.toInt()).toByte(),
            AndroidControllerDescriptor.bytes,
        )
        if (!hid.registerApp(sdp, null, null, executor, callback)) {
            _state.value = BridgeState(BridgePhase.Failed, message = "Another HID Device app may already be active")
            diagnostics?.event("controller", "HID registration", "request rejected")
        }
    }

    override fun onServiceDisconnected(profileId: Int) {
        sender?.cancel(); sender = null; host = null; input.neutralize()
        inputCollector?.cancel(); inputCollector = null
        profile = null
        if (stopped) return
        _state.value = BridgeState(BridgePhase.Unsupported, message = "Android HID Device service became unavailable")
        diagnostics?.event("controller", "HID profile", "service disconnected")
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
        stopped = true
        val hid = profile
        val device = host
        if (hid != null && device != null) {
            hid.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(ControllerState.Neutral))
            hid.disconnect(device)
        }
        sender?.cancel(); sender = null; inputCollector?.cancel(); inputCollector = null
        drainOutgoing(); host = null; input.neutralize()
        if (hid != null) hid.unregisterApp()
        runCatching { manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid) }
        profile = null
        _state.value = BridgeState()
        diagnostics?.event("controller", "bridge stopped")
    }

    fun close() {
        stop()
        scope.cancel()
        executor.shutdownNow()
    }

    fun neutralize() {
        input.neutralize()
        drainOutgoing()
        val device = host ?: return
        recordReport(profile?.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(ControllerState.Neutral)) == true)
    }

    private fun startSender() {
        sender?.cancel()
        inputCollector?.cancel()
        inputCollector = scope.launch {
            input.state.collect { state -> outgoing.offer(state) }
        }
        sender = scope.launch {
            var previous: ControllerState? = null
            while (isActive) {
                val state = outgoing.receive()
                if (state == previous) continue
                previous = state
                val device = host ?: continue
                recordReport(profile?.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(state)) == true)
                // Coalesce axis motion to the documented 125 Hz ceiling. Button edges can wait
                // at most one interval and the conflated mailbox always retains the newest state.
                delay(REPORT_INTERVAL_MS)
            }
        }
    }

    private fun drainOutgoing() {
        outgoing.drain()
    }

    private fun recordReport(ok: Boolean) {
        if (!ok) {
            _state.value = _state.value.copy(message = "Android rejected the latest input report")
            diagnostics?.event("controller", "report rejected")
            return
        }
        val count = _state.value.reportCount + 1
        _state.value = _state.value.copy(reportCount = count, lastReportAtMillis = System.currentTimeMillis())
        if (count == 1L || count % 100L == 0L) diagnostics?.event("controller", "reports sent", count.toString())
    }

    companion object {
        private const val REPORT_INTERVAL_MS = 8L
    }
}
