package dev.picoswitch.companion.bridge

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothHidDevice
import android.bluetooth.BluetoothHidDeviceAppSdpSettings
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import dev.picoswitch.bridge.core.BridgeCounters
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.protocol.BridgeHidDescriptor
import dev.picoswitch.bridge.session.BridgeHost
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.bridge.session.BridgeTransport
import dev.picoswitch.companion.bluetooth.AdapterBluetoothIdentity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.concurrent.Executors

/** A paired adapter, as Android knows it. */
class AndroidBridgeHost(val device: BluetoothDevice) : BridgeHost {
    @SuppressLint("MissingPermission")
    override val address: String = runCatching { device.address }.getOrDefault("")

    @SuppressLint("MissingPermission")
    override val name: String? = runCatching { device.name }.getOrNull()
}

/**
 * Android's `BluetoothHidDevice` profile as a [BridgeTransport].
 *
 * TRANSPORT MECHANICS ONLY. Everything here is about persuading one specific host
 * stack to act as a Bluetooth HID device and stay that way; none of it is bridge
 * protocol. Report composition, cadence, motion gating, rumble and neutralization
 * all live in `BridgeSession` and are identical on every platform.
 *
 * Three Android-stack facts shape this file, and are the reason the boundary is
 * where it is:
 *
 * - **Android exposes ONE HID Device slot per system.** Holding a registration
 *   across a dropped link makes the next attempt collide with this app's own
 *   orphaned record and report that another app owns the profile. Released on
 *   disconnect; re-acquiring is the normal resume path.
 * - **The callbacks are authoritative, not the return values.** The AYN Thor's
 *   OEM stack returns `false` from `registerApp()` and then delivers
 *   `onAppStatusChanged(registered = true)`. Bounded timeouts exist only because
 *   a stack may never deliver the callback at all.
 * - **Output arrives with two different framings.** Most stacks deliver the
 *   adapter's feedback on the interrupt channel; others as a control-channel
 *   SET_REPORT. Both are forwarded; the shared decoder tolerates either.
 */
@SuppressLint("MissingPermission")
class AndroidHidTransport(
    context: Context,
    private val diagnostics: BridgeDiagnostics = BridgeDiagnostics.None,
    /**
     * Shared with the session so the reverse path reads as ONE ordered picture.
     * Counting the raw HID callback here is the only way to distinguish "the
     * adapter never sent anything" from "we received it and dropped it", and that
     * distinction is the whole difference between a firmware fault and an app one.
     */
    private val counters: BridgeCounters = BridgeCounters(),
) : BridgeTransport, BluetoothProfile.ServiceListener {

    private val appContext = context.applicationContext
    private val manager = appContext.getSystemService(BluetoothManager::class.java)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val executor = Executors.newSingleThreadExecutor()

    private lateinit var listener: BridgeTransport.Listener
    private var profile: BluetoothHidDevice? = null
    private var host: BluetoothDevice? = null
    private var requestedHost: BluetoothDevice? = null
    private var registrationTimeout: Job? = null
    private var connectionTimeout: Job? = null
    @Volatile private var registered = false
    @Volatile private var stopped = false

    /**
     * Whether an establishment is outstanding. See [HidEstablishmentPolicy]:
     * "not stopped" is not the same as "wants a link", and treating them as the
     * same let an Android profile rebind start an attempt nobody requested.
     */
    @Volatile private var intent = HidEstablishmentIntent.Idle
    @Volatile private var phase = BridgeLinkPhase.Idle
    @Volatile private var connectStartedAtMillis = 0L

    /** One deadline for the whole connect attempt; see [armConnectionTimeout]. */
    @Volatile private var connectDeadlineAtMillis = 0L

    override fun attach(listener: BridgeTransport.Listener) {
        this.listener = listener
    }

    /** Returns the very field the HID callbacks below read. Not a copy. */
    override fun attachedListener(): BridgeTransport.Listener? =
        if (this::listener.isInitialized) listener else null

    override fun knownHosts(): List<BridgeHost> = runCatching {
        manager?.adapter?.bondedDevices.orEmpty().filter {
            AdapterBluetoothIdentity.isKnownAdapterName(it.name)
        }.sortedBy {
            if (AdapterBluetoothIdentity.isCurrentName(it.name)) 0 else 1
        }.map(::AndroidBridgeHost)
    }.getOrDefault(emptyList())

    override fun start(preferredHost: BridgeHost?) {
        stopped = false
        // An explicit request is the ONLY thing that grants establishment
        // authority; see HidEstablishmentPolicy.
        intent = HidEstablishmentIntent.Wanted
        requestedHost = (preferredHost as? AndroidBridgeHost)?.device ?: requestedHost
        profile?.let { hid ->
            if (registered) {
                val target = requestedHost
                if (target != null) beginConnect(target) else publish(
                    BridgeLinkPhase.Ready,
                    message = "Controller mode is ready",
                    registered = true,
                )
            } else {
                register(hid)
            }
            return
        }
        publish(BridgeLinkPhase.Preparing, message = "Checking Android HID Device support")
        diagnostics.event("transport", "HID profile", "acquiring")
        try {
            val ok = manager?.adapter?.getProfileProxy(appContext, this, BluetoothProfile.HID_DEVICE) == true
            if (!ok) publish(
                BridgeLinkPhase.Unsupported,
                message = "This Android build does not expose the HID Device profile",
            )
        } catch (error: Throwable) {
            fail("Android could not acquire its HID Device profile", error)
        }
    }

    override fun connect(host: BridgeHost) {
        val device = (host as? AndroidBridgeHost)?.device ?: return
        stopped = false
        intent = HidEstablishmentIntent.Wanted
        requestedHost = device
        if (profile == null) {
            start(host)
            return
        }
        beginConnect(device)
    }

    override fun send(reportId: Int, payload: ByteArray): Boolean {
        val hid = profile ?: return false
        val device = host ?: return false
        return runCatching { hid.sendReport(device, reportId, payload) }.getOrDefault(false)
    }

    override fun stop() {
        stopped = true
        intent = HidEstablishmentIntent.Idle
        registrationTimeout?.cancel(); registrationTimeout = null
        connectionTimeout?.cancel(); connectionTimeout = null
        val hid = profile
        val device = host
        if (hid != null && device != null) runCatching { hid.disconnect(device) }
        host = null
        requestedHost = null
        BridgeForegroundService.stop(appContext, diagnostics)
        if (hid != null) runCatching { hid.unregisterApp() }
        runCatching { manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid) }
        profile = null
        registered = false
        phase = BridgeLinkPhase.Idle
        diagnostics.event("transport", "stopped")
    }

    override fun close() {
        stop()
        scope.cancel()
        executor.shutdownNow()
    }

    // ------------------------------------------------------------ profile proxy

    override fun onServiceConnected(profileId: Int, proxy: BluetoothProfile) {
        if (stopped) {
            manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, proxy)
            return
        }
        val hid = proxy as? BluetoothHidDevice ?: return
        profile = hid
        // Only an outstanding request may drive registration+connect from here.
        // Android rebinds this service on its own, and doing it unconditionally
        // resurrected failed attempts as a second, unowned generation.
        if (!HidEstablishmentPolicy.mayAutoRegister(intent, stopped)) {
            diagnostics.event(
                "transport", "HID profile",
                "bound with no establishment requested; staying passive",
            )
            return
        }
        register(hid)
    }

    override fun onServiceDisconnected(profileId: Int) {
        // Closing our own proxy after a failed/ended host connection produces this callback too.
        // The connection callback already published the authoritative product state in that case.
        if (profile == null && !registered) {
            diagnostics.event("transport", "HID profile", "released proxy confirmed")
            return
        }
        registrationTimeout?.cancel(); registrationTimeout = null
        connectionTimeout?.cancel(); connectionTimeout = null
        host = null
        profile = null
        registered = false
        // Losing the profile service ends this generation's authority. Without
        // this, Android's own rebind re-entered onServiceConnected() and started
        // a second, unrequested attempt.
        intent = HidEstablishmentIntent.Idle
        BridgeForegroundService.stop(appContext, diagnostics)
        listener.onLinkDown("Android HID Device service became unavailable")
        if (stopped) return
        publish(BridgeLinkPhase.Unsupported, message = "Android HID Device service became unavailable")
        diagnostics.event("transport", "HID profile", "service disconnected")
    }

    private fun register(hid: BluetoothHidDevice) {
        registrationTimeout?.cancel()
        registrationTimeout = null
        publish(BridgeLinkPhase.Registering, message = "Waiting for Android to confirm HID registration")
        try {
            val sdp = BluetoothHidDeviceAppSdpSettings(
                BridgeHidDescriptor.SDP_NAME,
                BridgeHidDescriptor.SDP_DESCRIPTION,
                BridgeHidDescriptor.SDP_PROVIDER,
                (BluetoothHidDevice.SUBCLASS1_COMBO.toInt() or BluetoothHidDevice.SUBCLASS2_GAMEPAD.toInt()).toByte(),
                BridgeHidDescriptor.bytes,
            )
            // Some OEM stacks, including the AYN Thor's Android 13 build, return false here
            // and then immediately deliver onAppStatusChanged(registered=true). The callback is
            // authoritative; treating the synchronous boolean as final leaves our own accepted
            // registration alive and makes the next attempt collide with it.
            val immediateAccepted = hid.registerApp(sdp, null, null, executor, callback)
            if (!immediateAccepted) diagnostics.event(
                "transport", "HID registration", "immediate result false; awaiting callback",
            )
            registrationTimeout = scope.launch {
                delay(REGISTRATION_CALLBACK_TIMEOUT_MS)
                if (stopped || profile !== hid || phase != BridgeLinkPhase.Registering) return@launch
                // Only an accepted synchronous request is ours to unregister. A false result may
                // mean a genuinely different provider owns the one system slot.
                if (immediateAccepted) runCatching { hid.unregisterApp() }
                closeFailedProfile(hid)
                publish(
                    BridgeLinkPhase.Failed,
                    message = if (immediateAccepted) {
                        "Android did not confirm HID Device registration; retry"
                    } else {
                        "Android did not confirm HID Device registration; another HID app may be active"
                    },
                )
                diagnostics.event("transport", "HID registration", "callback timeout")
            }
        } catch (error: Throwable) {
            closeFailedProfile(hid)
            fail("Android rejected HID Device registration", error)
        }
    }

    private fun beginConnect(device: BluetoothDevice) {
        val hid = profile ?: return publish(BridgeLinkPhase.Failed, message = "HID profile is not ready")
        connectionTimeout?.cancel()
        connectStartedAtMillis = System.currentTimeMillis()
        connectDeadlineAtMillis = connectStartedAtMillis + CONNECTION_CALLBACK_TIMEOUT_MS
        publish(BridgeLinkPhase.Connecting, device.name, "Connecting controller mode", registered = true)
        try {
            val immediateAccepted = hid.connect(device)
            // Android's HID Device profile refuses a host whose stored Class of
            // Device looks like a HID peripheral: btif_hd checks
            // (cod & 0x1F00) == 0x0500 at BTA_HD_OPEN_EVT and disconnects with
            // "remote device is not hid host". That check reads system remote
            // metadata we cannot see from the connect result, so record the
            // framework's own view of the host here -- it is the difference
            // between "the link failed" and "the phone refused this identity".
            val cls = runCatching { device.bluetoothClass }.getOrNull()
            diagnostics.event(
                "transport", "HID connection",
                "requested accepted=$immediateAccepted bond=${device.bondState} type=${device.type} " +
                    "major=0x%04X device=0x%04X hostOk=%s; awaiting callback".format(
                        cls?.majorDeviceClass ?: -1,
                        cls?.deviceClass ?: -1,
                        (cls != null && cls.majorDeviceClass != PERIPHERAL_MAJOR_CLASS).toString(),
                    ),
            )
            armConnectionTimeout(hid, device)
        } catch (error: Throwable) {
            fail("Android could not start controller mode", error)
        }
    }

    /**
     * The watchdog for a connect that never resolves.
     *
     * Bounded by ONE deadline for the whole attempt, set in [beginConnect] and
     * never extended. Two different silences have to be caught and only a
     * whole-attempt deadline catches both:
     *
     * - the stack accepts `connect()` and then says nothing at all, so no
     *   callback ever arrives to cancel this;
     * - the stack reports STATE_CONNECTING, which cancels the pending watchdog,
     *   and then says nothing more. Re-arming for a fresh interval each time
     *   would let a chatty stack push the deadline out forever, which is the same
     *   indefinite lie in a different costume.
     *
     * Declining to fire is logged with the reason. A watchdog that silently
     * decides not to bark is indistinguishable from one that never ran, and that
     * ambiguity costs a debugging session.
     */
    private fun armConnectionTimeout(hid: BluetoothHidDevice, device: BluetoothDevice) {
        connectionTimeout?.cancel()
        val remaining = connectDeadlineAtMillis - System.currentTimeMillis()
        connectionTimeout = scope.launch {
            if (remaining > 0) delay(remaining)
            val elapsed = System.currentTimeMillis() - connectStartedAtMillis
            if (stopped || profile !== hid || phase != BridgeLinkPhase.Connecting) {
                diagnostics.event(
                    "transport", "HID connection",
                    "watchdog stood down after ${elapsed}ms: stopped=$stopped " +
                        "sameProfile=${profile === hid} phase=$phase",
                )
                return@launch
            }
            publish(
                BridgeLinkPhase.Failed,
                device.name,
                "Android did not complete controller mode; reopen the adapter pairing window and retry",
                registered = true,
            )
            diagnostics.event(
                "transport", "HID connection",
                "callback timeout after ${elapsed}ms bond=${device.bondState} type=${device.type}",
            )
        }
    }

    /**
     * Give up this app's HID Device registration and proxy.
     *
     * Android exposes exactly one HID Device slot per system, so a registration we
     * no longer need is indistinguishable, from the next attempt's point of view,
     * from a third-party app owning it. Both calls are best-effort: the profile may
     * already be gone, and failing to release must not throw into a callback.
     */
    private fun releaseRegistration() {
        val hid = profile ?: return
        profile = null
        registered = false
        runCatching { hid.unregisterApp() }
        runCatching { manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid) }
    }

    private fun closeFailedProfile(hid: BluetoothHidDevice) {
        if (profile === hid) profile = null
        registered = false
        runCatching { manager?.adapter?.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid) }
    }

    private fun fail(message: String, error: Throwable) {
        publish(
            BridgeLinkPhase.Failed,
            message = "$message: ${error.message ?: error.javaClass.simpleName}",
        )
        diagnostics.error("transport", message, error)
    }

    private fun publish(
        next: BridgeLinkPhase,
        hostName: String? = null,
        message: String? = null,
        registered: Boolean = false,
    ) {
        phase = next
        this.registered = registered
        // One place decides when an establishment is over. Playing means it
        // succeeded; Failed/Unsupported/Idle mean it ended. In every case the
        // authority to start another one goes back to the caller, so an Android
        // profile rebind cannot resurrect it. See HidEstablishmentPolicy.
        when (next) {
            BridgeLinkPhase.Playing,
            BridgeLinkPhase.Failed,
            BridgeLinkPhase.Unsupported,
            BridgeLinkPhase.Idle -> intent = HidEstablishmentIntent.Idle
            else -> Unit
        }
        listener.onPhase(next, hostName, message, registered)
    }

    private fun stateName(state: Int): String = HidConnectionState.name(state)

    private val callback = object : BluetoothHidDevice.Callback() {
        override fun onAppStatusChanged(pluggedDevice: BluetoothDevice?, registered: Boolean) {
            if (stopped) return
            registrationTimeout?.cancel()
            registrationTimeout = null
            diagnostics.event(
                "transport", "HID registration",
                if (registered) "registered" else "unregistered",
            )
            if (registered) {
                val target = requestedHost
                if (target != null) {
                    this@AndroidHidTransport.registered = true
                    beginConnect(target)
                } else {
                    publish(
                        BridgeLinkPhase.Ready, pluggedDevice?.name,
                        "Controller mode is ready", registered = true,
                    )
                }
            } else if (profile == null) {
                diagnostics.event("transport", "HID registration", "released registration confirmed")
            } else if (phase != BridgeLinkPhase.Idle) {
                profile?.let(::closeFailedProfile)
                publish(
                    BridgeLinkPhase.Failed,
                    message = "Android removed HID Device registration; keep the app in front and prepare it again",
                )
            }
        }

        override fun onInterruptData(device: BluetoothDevice?, reportId: Byte, data: ByteArray?) {
            counters.transportOutputCallbacks.incrementAndGet()
            listener.onOutputReport(data, reportId.toInt() and 0xFF)
        }

        override fun onSetReport(device: BluetoothDevice?, type: Byte, id: Byte, data: ByteArray?) {
            val reportId = id.toInt() and 0xFF
            counters.transportOutputCallbacks.incrementAndGet()
            listener.onOutputReport(data, reportId)
            // Answering is a control-channel obligation, so it must reflect whether
            // the payload was usable. The shared decoder owns that judgement; asking
            // it here keeps one definition of "valid output report".
            val accepted = dev.picoswitch.bridge.protocol.BridgeOutputCodec.decode(data, reportId) != null
            runCatching {
                profile?.reportError(
                    device,
                    if (accepted) BluetoothHidDevice.ERROR_RSP_SUCCESS
                    else BluetoothHidDevice.ERROR_RSP_INVALID_PARAM,
                )
            }
        }

        override fun onGetReport(device: BluetoothDevice?, type: Byte, id: Byte, bufferSize: Int) {
            // A host may poll the current state over the control channel; answer
            // with a real snapshot rather than leaving the request to time out.
            runCatching {
                profile?.replyReport(device, type, id, listener.currentReport())
            }
        }

        override fun onConnectionStateChanged(device: BluetoothDevice, state: Int) {
            if (stopped) return
            diagnostics.event("transport", "HID connection state", stateName(state))
            // ONLY a terminal state disarms the watchdog.
            //
            // This callback used to cancel it unconditionally, before deciding
            // whether the state was one it handles. STATE_DISCONNECTING has no
            // branch below, so on a stack that reports it the watchdog was
            // silently thrown away and the phase stayed on "Connecting" forever
            // with nothing left that could ever resolve it. Measured on an
            // Android 16 tablet 2026-08-21: the BR/EDR ACL came up, the adapter
            // dropped it 750 ms later, and the app waited indefinitely.
            if (HidConnectionState.isTerminal(state)) {
                connectionTimeout?.cancel()
                connectionTimeout = null
            }
            when (state) {
                BluetoothProfile.STATE_CONNECTING -> {
                    publish(
                        BridgeLinkPhase.Connecting, device.name,
                        "Connecting controller mode", registered = true,
                    )
                    // Re-armed against the SAME whole-attempt deadline, so a
                    // chatty stack cannot push it out; see armConnectionTimeout.
                    profile?.let { armConnectionTimeout(it, device) }
                }
                BluetoothProfile.STATE_CONNECTED -> {
                    host = device
                    requestedHost = device
                    registered = true
                    phase = BridgeLinkPhase.Playing
                    // Foreground BEFORE any output can arrive: Android drops
                    // vibration from a non-foreground uid before consulting any
                    // setting, and returns success anyway. See
                    // BridgeForegroundService.
                    BridgeForegroundService.start(appContext, diagnostics)
                    listener.onLinkUp(device.name)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    val priorPhase = phase
                    val elapsed = if (connectStartedAtMillis == 0L) 0L
                        else System.currentTimeMillis() - connectStartedAtMillis
                    host = null
                    BridgeForegroundService.stop(appContext, diagnostics)
                    // Hand Android's single HID Device slot back before telling the
                    // session, so a resume attempt cannot race our own orphaned
                    // registration.
                    releaseRegistration()
                    if (priorPhase == BridgeLinkPhase.Connecting) {
                        publish(
                            BridgeLinkPhase.Failed,
                            device.name,
                            "Couldn’t finish Controller Link. Put PicoSwitch2 in pairing mode, then try again.",
                        )
                        diagnostics.event(
                            "transport", "HID connection rejected",
                            "elapsedMs=$elapsed bond=${device.bondState} type=${device.type}; released registration",
                        )
                    } else {
                        phase = BridgeLinkPhase.Idle
                        diagnostics.event("transport", "host disconnected", "elapsedMs=$elapsed; released HID registration")
                        listener.onLinkDown("Controller link disconnected")
                    }
                }
            }
        }
    }

    private companion object {
        const val REGISTRATION_CALLBACK_TIMEOUT_MS = 2_000L
        const val CONNECTION_CALLBACK_TIMEOUT_MS = 8_000L

        /**
         * `BluetoothClass.Device.Major.PERIPHERAL`.
         *
         * A host whose stored class carries this major class is rejected by
         * Android's own HID Device profile (`btif_hd`, `check_cod_hid()`:
         * `(cod & 0x1F00) == 0x0500`) immediately after the HID channels open,
         * with `remote device is not hid host, disconnecting`. The adapter is
         * the HID *host*, so this value never legitimately describes it.
         */
        const val PERIPHERAL_MAJOR_CLASS = 0x0500
    }
}
