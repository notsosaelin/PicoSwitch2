package dev.picoswitch.companion.controller

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.os.Handler
import android.os.Looper
import dev.picoswitch.companion.bluetooth.AdapterBluetoothIdentity
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
    private var registrationTimeout: Job? = null
    private var connectionTimeout: Job? = null
    private var requestedHost: BluetoothDevice? = null
    private var sender: Job? = null
    private var inputCollector: Job? = null
    // One latest-state mailbox prevents old motion reports surviving behind newer input.
    private val outgoing = LatestReportMailbox<ControllerState>()
    @Volatile private var stopped = false

    private val callback = object : BluetoothHidDevice.Callback() {
        override fun onAppStatusChanged(pluggedDevice: BluetoothDevice?, registered: Boolean) {
            if (stopped) return
            registrationTimeout?.cancel()
            registrationTimeout = null
            diagnostics?.event("controller", "HID registration", if (registered) "registered" else "unregistered")
            if (registered) {
                val target = requestedHost
                if (target != null) beginConnect(target) else _state.value = _state.value.copy(
                    phase = BridgePhase.Ready, hostName = pluggedDevice?.name,
                    message = "Controller mode is ready", registered = true,
                )
            } else if (_state.value.phase != BridgePhase.Idle) {
                val hid = profile
                if (hid != null) closeFailedProfile(hid)
                _state.value = _state.value.copy(
                    phase = BridgePhase.Failed, message = "Android removed HID Device registration; keep the app in front and prepare it again",
                    registered = false,
                )
            }
        }

        override fun onConnectionStateChanged(device: BluetoothDevice, state: Int) {
            if (stopped) return
            connectionTimeout?.cancel()
            connectionTimeout = null
            when (state) {
                BluetoothProfile.STATE_CONNECTING -> _state.value = _state.value.copy(
                    phase = BridgePhase.Connecting,
                    hostName = device.name,
                    message = "Connecting controller mode",
                    registered = true,
                )
                BluetoothProfile.STATE_CONNECTED -> {
                    host = device
                    requestedHost = device
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

    fun acquire(preferredHost: BluetoothDevice? = null) {
        stopped = false
        requestedHost = preferredHost ?: requestedHost
        profile?.let {
            if (_state.value.registered) {
                val target = requestedHost
                if (target != null) beginConnect(target)
                else _state.value = _state.value.copy(
                    phase = BridgePhase.Ready,
                    message = "Controller mode is ready",
                )
            } else {
                register(it)
            }
            return
        }
        _state.value = BridgeState(BridgePhase.AcquiringProfile, message = "Checking Android HID Device support")
        diagnostics?.event("controller", "HID profile", "acquiring")
        try {
            val ok = manager?.adapter?.getProfileProxy(appContext, this, BluetoothProfile.HID_DEVICE) == true
            if (!ok) _state.value = BridgeState(BridgePhase.Unsupported, message = "This Android build does not expose the HID Device profile")
        } catch (error: Throwable) {
            fail("Android could not acquire its HID Device profile", error)
        }
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
        registrationTimeout?.cancel()
        registrationTimeout = null
        _state.value = BridgeState(BridgePhase.Registering, message = "Waiting for Android to confirm HID registration")
        try {
            val sdp = BluetoothHidDeviceAppSdpSettings(
                "PicoSwitch Android Controller", "Built-in controls passthrough", "PicoSwitch2",
                (BluetoothHidDevice.SUBCLASS1_COMBO.toInt() or BluetoothHidDevice.SUBCLASS2_GAMEPAD.toInt()).toByte(),
                AndroidControllerDescriptor.bytes,
            )
            // Some OEM stacks, including the AYN Thor's Android 13 build, return false here
            // and then immediately deliver onAppStatusChanged(registered=true). The callback is
            // authoritative; treating the synchronous boolean as final leaves our own accepted
            // registration alive and makes the next attempt collide with it.
            val immediateAccepted = hid.registerApp(sdp, null, null, executor, callback)
            if (!immediateAccepted) diagnostics?.event(
                "controller", "HID registration", "immediate result false; awaiting callback",
            )
            registrationTimeout = scope.launch {
                delay(REGISTRATION_CALLBACK_TIMEOUT_MS)
                if (stopped || profile !== hid || _state.value.phase != BridgePhase.Registering) return@launch
                // Only an accepted synchronous request is ours to unregister. A false result may
                // mean a genuinely different provider owns the one system slot.
                if (immediateAccepted) runCatching { hid.unregisterApp() }
                closeFailedProfile(hid)
                _state.value = BridgeState(
                    BridgePhase.Failed,
                    message = if (immediateAccepted) {
                        "Android did not confirm HID Device registration; retry"
                    } else {
                        "Android did not confirm HID Device registration; another HID app may be active"
                    },
                )
                diagnostics?.event("controller", "HID registration", "callback timeout")
            }
        } catch (error: Throwable) {
            closeFailedProfile(hid)
            fail("Android rejected HID Device registration", error)
        }
    }

    private fun closeFailedProfile(hid: BluetoothHidDevice) {
        if (profile === hid) profile = null
        runCatching { manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid) }
    }

    private fun fail(message: String, error: Throwable) {
        _state.value = BridgeState(BridgePhase.Failed, message = "$message: ${error.message ?: error.javaClass.simpleName}")
        diagnostics?.error("controller", message, error)
    }

    override fun onServiceDisconnected(profileId: Int) {
        registrationTimeout?.cancel(); registrationTimeout = null
        connectionTimeout?.cancel(); connectionTimeout = null
        sender?.cancel(); sender = null; host = null; input.neutralize()
        inputCollector?.cancel(); inputCollector = null
        profile = null
        if (stopped) return
        _state.value = BridgeState(BridgePhase.Unsupported, message = "Android HID Device service became unavailable")
        diagnostics?.event("controller", "HID profile", "service disconnected")
    }

    fun pairedHosts(): List<BluetoothDevice> = runCatching {
        manager?.adapter?.bondedDevices.orEmpty().filter {
            AdapterBluetoothIdentity.isKnownAdapterName(it.name)
        }.sortedBy { if (AdapterBluetoothIdentity.isCurrentName(it.name)) 0 else 1 }
    }.getOrDefault(emptyList())

    fun connect(device: BluetoothDevice) {
        requestedHost = device
        beginConnect(device)
    }

    private fun beginConnect(device: BluetoothDevice) {
        val hid = profile ?: return run { _state.value = BridgeState(BridgePhase.Failed, message = "HID profile is not ready") }
        connectionTimeout?.cancel()
        _state.value = BridgeState(BridgePhase.Connecting, device.name, "Connecting controller mode", registered = true)
        try {
            val immediateAccepted = hid.connect(device)
            if (!immediateAccepted) diagnostics?.event(
                "controller", "HID connection", "immediate result false; awaiting callback",
            )
            connectionTimeout = scope.launch {
                delay(CONNECTION_CALLBACK_TIMEOUT_MS)
                if (stopped || profile !== hid || _state.value.phase != BridgePhase.Connecting) return@launch
                _state.value = BridgeState(
                    BridgePhase.Failed,
                    device.name,
                    "Android did not complete controller mode; reopen the adapter pairing window and retry",
                    registered = true,
                )
                diagnostics?.event("controller", "HID connection", "callback timeout")
            }
        } catch (error: Throwable) {
            fail("Android could not start controller mode", error)
        }
    }

    fun stop() {
        stopped = true
        registrationTimeout?.cancel(); registrationTimeout = null
        connectionTimeout?.cancel(); connectionTimeout = null
        val hid = profile
        val device = host
        if (hid != null && device != null) {
            hid.sendReport(device, ControllerReportEncoder.REPORT_ID, ControllerReportEncoder.encode(ControllerState.Neutral))
            hid.disconnect(device)
        }
        sender?.cancel(); sender = null; inputCollector?.cancel(); inputCollector = null
        drainOutgoing(); host = null; requestedHost = null; input.neutralize()
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
        private const val REGISTRATION_CALLBACK_TIMEOUT_MS = 2_000L
        private const val CONNECTION_CALLBACK_TIMEOUT_MS = 8_000L
    }
}
