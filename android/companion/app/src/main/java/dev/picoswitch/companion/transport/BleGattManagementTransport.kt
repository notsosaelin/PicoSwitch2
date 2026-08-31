package dev.picoswitch.companion.transport

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import android.os.SystemClock
import dev.picoswitch.companion.diagnostics.DiagnosticLog
import dev.picoswitch.companion.diagnostics.ManagementDiagnosticContext
import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.protocol.ManagementConnectionContext
import dev.picoswitch.companion.protocol.ManagementException
import dev.picoswitch.companion.protocol.ManagementReplyTooLargeException
import dev.picoswitch.companion.protocol.ManagementTransport
import dev.picoswitch.management.BleManagementContract
import dev.picoswitch.management.BleReplyAssembler
import dev.picoswitch.management.SerializedManagementSession
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.UUID
import java.util.concurrent.atomic.AtomicLong
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Android GATT ownership for exactly one management session at a time. */
@SuppressLint("MissingPermission")
class BleGattManagementTransport(context: Context, private val diagnostics: DiagnosticLog? = null) : ManagementTransport {
    private val appContext = context.applicationContext
    private val manager = appContext.getSystemService(BluetoothManager::class.java)
    private val adapter get() = manager?.adapter
    private val session = SerializedManagementSession()
    private val lifecycle = Mutex()
    private val lifecycleScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val notifications = Channel<ByteArray>(capacity = 32)
    private val _connection = MutableStateFlow(ConnectionState())
    override val connection: StateFlow<ConnectionState> = _connection

    private class OwnedGatt(
        val generation: Long,
        val context: ManagementConnectionContext,
        val device: BluetoothDevice,
        val startedAtElapsedMillis: Long,
    ) {
        @Volatile var gatt: BluetoothGatt? = null
        @Volatile var rx: BluetoothGattCharacteristic? = null
        @Volatile var tx: BluetoothGattCharacteristic? = null
        @Volatile var writeReady: CompletableDeferred<Unit>? = null
        @Volatile var closeRequested = false
        @Volatile var terminalFailure = false
        @Volatile var closed = false
        @Volatile var serviceDiscoveryStarted = false
        @Volatile var negotiatedMtu = BleManagementContract.DEFAULT_ATT_MTU
        @Volatile var readyAtElapsedMillis = 0L
        @Volatile var lastReplyAtElapsedMillis = 0L
        @Volatile var commandTrace: CommandTrace? = null
        val ready = CompletableDeferred<Unit>()
        val disconnected = CompletableDeferred<Unit>()
    }

    private class CommandTrace(
        val sequence: Long,
        val type: String,
        val source: String,
        val startedAtElapsedMillis: Long,
    ) {
        @Volatile var writeCallbacks = 0
        @Volatile var notificationCount = 0
        @Volatile var notificationBytes = 0
        @Volatile var firstNotificationAtElapsedMillis = 0L
    }

    private data class PairingCandidate(val generation: Long, val device: BluetoothDevice)

    @Volatile private var current: OwnedGatt? = null
    @Volatile private var pairingCandidate: PairingCandidate? = null
    @Volatile private var nextContext = ManagementConnectionContext()
    private var nextGattGeneration = 0L
    private val nextCommandSequence = AtomicLong()

    override fun prepareConnection(context: ManagementConnectionContext) {
        nextContext = context
        if (!context.useDiscoveredPeer) pairingCandidate = null
        if (context.retry > 0) {
            diagnostics?.event(
                "management",
                "gatt.retry",
                "attempt=${context.logicalAttempt} reason=${context.reason} retry=${context.retry}/${GattRecoveryPolicy.MAX_CLEAN_RETRIES} priorGattRetired=${context.priorGattRetired}",
            )
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            val owner = ownerFor(gatt) ?: return staleCallback("state", gatt)
            log(
                owner,
                "gatt.state",
                "status=$status state=$newState elapsedMs=${SystemClock.elapsedRealtime() - owner.startedAtElapsedMillis}",
            )
            if (status != BluetoothGatt.GATT_SUCCESS) {
                val failure = GattTransportException(
                    "Bluetooth connection failed (${GattStatusFormatter.describe(GattFailureStage.Connect, status)})",
                    status, GattFailureStage.Connect,
                )
                fail(owner, failure)
                if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    owner.disconnected.complete(Unit)
                    closeExactlyOnce(owner)
                    clearIfCurrent(owner)
                }
                return
            }
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _connection.value = connectionState(owner, ConnectionPhase.Connecting, "Preparing management link")
                    val priorityAccepted = runCatching {
                        gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    }.getOrDefault(false)
                    log(owner, "gatt.priority", "source=setup priority=high apiAccepted=$priorityAccepted")

                    // The management protocol can return nearly 512 bytes. Staying at the default
                    // 23-byte ATT MTU turns one reply into dozens of notifications and exposed a
                    // reproducible Android 13 failure where the following command never reached a
                    // reply. Negotiate once before discovery; the overall connect deadline still
                    // bounds OEMs that accept the request but never issue the callback.
                    val mtuRequested = runCatching { gatt.requestMtu(REQUESTED_ATT_MTU) }.getOrDefault(false)
                    log(owner, "gatt.mtu_request", "mtu=$REQUESTED_ATT_MTU requested=$mtuRequested")
                    if (!mtuRequested) discoverServices(owner, gatt)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    owner.disconnected.complete(Unit)
                    owner.ready.completeExceptionally(ManagementException("Adapter disconnected"))
                    owner.writeReady?.completeExceptionally(ManagementException("Adapter disconnected during command"))
                    notifications.trySend(ByteArray(0))
                    if (!owner.terminalFailure) {
                        _connection.value = connectionState(
                            owner,
                            if (owner.closeRequested) ConnectionPhase.Idle else ConnectionPhase.Reconnecting,
                            if (owner.closeRequested) null else "Connection lost. Tap reconnect when the adapter is available.",
                        )
                    }
                    log(owner, if (owner.closeRequested) "gatt.closed" else "disconnect.remote")
                    closeExactlyOnce(owner)
                    clearIfCurrent(owner)
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            val owner = ownerFor(gatt) ?: return staleCallback("mtu", gatt)
            if (status == BluetoothGatt.GATT_SUCCESS) owner.negotiatedMtu = mtu
            log(owner, "gatt.mtu", "status=$status mtu=$mtu")
            discoverServices(owner, gatt)
        }

        /**
         * Android's framework dispatches this callback but its public SDK stub hides the method.
         * Declaring the same virtual signature lets diagnostic builds observe the negotiated
         * controller values without reflection or a Bluetooth behavior change. If an OEM omits
         * the callback, the adapter-side LE update event remains the independent observation.
         */
        @Suppress("unused")
        fun onConnectionUpdated(
            gatt: BluetoothGatt,
            interval: Int,
            latency: Int,
            timeout: Int,
            status: Int,
        ) {
            val owner = ownerFor(gatt) ?: return staleCallback("params", gatt)
            log(
                owner,
                "gatt.params",
                "status=$status intervalUnits=$interval intervalUs=${interval * 1_250L} " +
                    "latency=$latency timeoutUnits=$timeout timeoutMs=${timeout * 10L}",
            )
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val owner = ownerFor(gatt) ?: return staleCallback("services", gatt)
            log(owner, "gatt.services", "status=$status")
            if (status != BluetoothGatt.GATT_SUCCESS) {
                return fail(owner, GattTransportException(
                    "Service discovery failed (${GattStatusFormatter.describe(GattFailureStage.Services, status)})",
                    status, GattFailureStage.Services,
                ))
            }
            val service = gatt.getService(UUID.fromString(BleManagementContract.SERVICE_UUID))
                ?: return fail(owner, GattTransportException(
                    "This device does not expose PicoSwitch management", null, GattFailureStage.Services,
                ))
            owner.rx = service.getCharacteristic(UUID.fromString(BleManagementContract.RX_UUID))
            owner.tx = service.getCharacteristic(UUID.fromString(BleManagementContract.TX_UUID))
            val output = owner.tx ?: return fail(owner, GattTransportException(
                "Management notification characteristic is missing", null, GattFailureStage.Services,
            ))
            if (owner.rx == null) return fail(owner, GattTransportException(
                "Management command characteristic is missing", null, GattFailureStage.Services,
            ))
            val notificationAccepted = gatt.setCharacteristicNotification(output, true)
            log(
                owner,
                "gatt.notification_api",
                "characteristic=tx enabled=true apiAccepted=$notificationAccepted",
            )
            if (!notificationAccepted) return fail(owner, GattTransportException(
                "Could not enable management replies", null, GattFailureStage.Subscribe,
            ))
            val ccc = output.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG)
                ?: return fail(owner, GattTransportException(
                    "Management notification descriptor is missing", null, GattFailureStage.Subscribe,
                ))
            log(owner, "gatt.descriptor_begin", "descriptor=ccc operation=enable-notifications bytes=2")
            val descriptorAccepted = writeDescriptor(
                gatt,
                ccc,
                BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE,
            )
            log(
                owner,
                "gatt.descriptor_api",
                "descriptor=ccc operation=enable-notifications apiAccepted=$descriptorAccepted",
            )
            if (!descriptorAccepted) {
                fail(owner, GattTransportException(
                    "Could not subscribe to management replies", null, GattFailureStage.Subscribe,
                ))
            }
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            val owner = ownerFor(gatt) ?: return staleCallback("descriptor", gatt)
            log(owner, "gatt.subscribe", "status=$status")
            if (status == BluetoothGatt.GATT_SUCCESS) {
                _connection.value = connectionState(owner, ConnectionPhase.Connecting, "Verifying PicoSwitch2 identity")
                owner.readyAtElapsedMillis = SystemClock.elapsedRealtime()
                owner.ready.complete(Unit)
            } else {
                fail(owner, GattTransportException(
                    "Notification subscription failed (${GattStatusFormatter.describe(GattFailureStage.Subscribe, status)})",
                    status, GattFailureStage.Subscribe,
                ))
            }
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (ownerFor(gatt) != null && characteristic.uuid == TX_UUID) {
                recordNotification(ownerFor(gatt), characteristic.value)
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (characteristic.uuid == TX_UUID) recordNotification(ownerFor(gatt), value)
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            ownerFor(gatt)?.let { owner ->
                owner.commandTrace?.let { trace ->
                    trace.writeCallbacks += 1
                    log(
                        owner,
                        "command.write",
                        "seq=${trace.sequence} type=${trace.type} callback=${trace.writeCallbacks} status=$status " +
                            "source=${trace.source} elapsedMs=${SystemClock.elapsedRealtime() - trace.startedAtElapsedMillis}",
                    )
                }
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    owner.writeReady?.complete(Unit)
                } else {
                    owner.commandTrace?.let { trace ->
                        log(
                            owner,
                            "gatt.error",
                            "stage=command status=$status seq=${trace.sequence} source=${trace.source} action=observe-only",
                        )
                    }
                    owner.writeReady?.completeExceptionally(GattTransportException(
                        "Bluetooth write failed (${GattStatusFormatter.describe(GattFailureStage.Command, status)})",
                        status, GattFailureStage.Command,
                    ))
                }
            }
        }
    }

    private fun discoverServices(owner: OwnedGatt, gatt: BluetoothGatt) {
        synchronized(owner) {
            if (owner.serviceDiscoveryStarted || owner.closed || owner.terminalFailure) return
            owner.serviceDiscoveryStarted = true
        }
        _connection.value = connectionState(owner, ConnectionPhase.Connecting, "Discovering adapter services")
        log(owner, "gatt.services_begin", "operation=discoverServices")
        val accepted = gatt.discoverServices()
        log(owner, "gatt.services_api", "operation=discoverServices apiAccepted=$accepted")
        if (!accepted) {
            fail(owner, GattTransportException(
                "Service discovery could not start", null, GattFailureStage.Services,
            ))
        }
    }

    private fun recordNotification(owner: OwnedGatt?, value: ByteArray) {
        if (owner == null) return
        owner.commandTrace?.let { trace ->
            if (trace.firstNotificationAtElapsedMillis == 0L) {
                trace.firstNotificationAtElapsedMillis = SystemClock.elapsedRealtime()
                log(
                    owner,
                    "response.first",
                    "seq=${trace.sequence} type=${trace.type} source=${trace.source} bytes=${value.size} " +
                        "elapsedMs=${trace.firstNotificationAtElapsedMillis - trace.startedAtElapsedMillis}",
                )
            }
            trace.notificationCount += 1
            trace.notificationBytes += value.size
        }
        notifications.trySend(value.copyOf())
    }

    override suspend fun scanAndConnect() = scanAndConnectInternal(expectedAddress = null)

    override suspend fun scanAndConnect(expectedAddress: String) = scanAndConnectInternal(expectedAddress.uppercase())

    override suspend fun discover(): dev.picoswitch.companion.protocol.DiscoveredManagementPeer = lifecycle.withLock {
        retireCurrentLocked("before pairing discovery", settleIdle = true)
        val context = nextContext
        val device = scanDeviceLocked(
            expectedAddress = null,
            context = context,
            message = "Looking for PicoSwitch2 in pairing mode",
            diagnosticEvent = "pair.scan",
        )
        pairingCandidate = PairingCandidate(context.logicalAttempt, device)
        log(context, "pair.discovered", "bond=${device.bondState}")
        dev.picoswitch.companion.protocol.DiscoveredManagementPeer(
            device = device,
            displayName = safeName(device),
        )
    }

    private suspend fun scanAndConnectInternal(expectedAddress: String?) = lifecycle.withLock {
        retireCurrentLocked("before scan", settleIdle = true)
        val context = nextContext
        val device = scanDeviceLocked(
            expectedAddress = expectedAddress,
            context = context,
            message = if (expectedAddress == null) "Looking for PicoSwitch2" else "Looking for the saved PicoSwitch2",
            diagnosticEvent = "connect.scan_fallback",
        )
        connectDeviceLocked(device)
    }

    private suspend fun scanDeviceLocked(
        expectedAddress: String?,
        context: ManagementConnectionContext,
        message: String,
        diagnosticEvent: String,
    ): BluetoothDevice {
        val bluetooth = adapter ?: throw ManagementException("Bluetooth is not available on this device")
        if (!bluetooth.isEnabled) throw ManagementException("Turn on Bluetooth to find PicoSwitch2")
        val scanner = bluetooth.bluetoothLeScanner ?: throw ManagementException("Bluetooth LE scanning is unavailable")
        _connection.value = ConnectionState(
            phase = ConnectionPhase.Scanning,
            message = message,
            attempt = context.logicalAttempt.toInt(),
        )
        diagnostics?.event(
            "relationship", diagnosticEvent,
            "attempt=${context.logicalAttempt} reason=${context.reason} expected=${if (expectedAddress == null) "any" else "saved"}",
        )
        return try {
            withTimeout(SCAN_TIMEOUT_MILLIS) {
                suspendCancellableCoroutine<BluetoothDevice> { continuation ->
                    val filter = ScanFilter.Builder()
                        .setServiceUuid(ParcelUuid.fromString(BleManagementContract.SERVICE_UUID))
                        .build()
                    val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
                    val scanCallback = object : ScanCallback() {
                        override fun onScanResult(callbackType: Int, result: ScanResult) {
                            if (!continuation.isActive) return
                            if (expectedAddress != null && !result.device.address.equals(expectedAddress, true)) return
                            runCatching { scanner.stopScan(this) }
                            continuation.resume(result.device)
                        }

                        override fun onScanFailed(errorCode: Int) {
                            if (continuation.isActive) {
                                continuation.resumeWithException(ManagementException("Bluetooth scan failed ($errorCode)"))
                            }
                        }
                    }
                    continuation.invokeOnCancellation { runCatching { scanner.stopScan(scanCallback) } }
                    scanner.startScan(listOf(filter), settings, scanCallback)
                }
            }
        } catch (error: TimeoutCancellationException) {
            _connection.value = ConnectionState(
                ConnectionPhase.Failed,
                message = if (expectedAddress == null) {
                    "No management advertisement was found"
                } else "The saved adapter did not advertise management",
                attempt = context.logicalAttempt.toInt(),
            )
            diagnostics?.error("management", "discovery", error)
            throw ManagementException("No matching PicoSwitch2 management service was found within 15 seconds", error)
        }
    }

    override suspend fun connectKnown(address: String) = lifecycle.withLock {
        retireCurrentLocked("before direct connect", settleIdle = true)
        val bluetooth = adapter ?: throw ManagementException("Bluetooth is not available on this device")
        if (!bluetooth.isEnabled) throw ManagementException("Turn on Bluetooth to connect to PicoSwitch2")
        val device = pairingCandidate?.takeIf {
            nextContext.useDiscoveredPeer &&
                it.generation == nextContext.logicalAttempt &&
                it.device.address.equals(address, true)
        }?.device
            ?: runCatching { bluetooth.getRemoteDevice(address) }
                .getOrElse { throw ManagementException("The saved PicoSwitch2 address is invalid", it) }
        connectDeviceLocked(device)
    }

    private suspend fun connectDeviceLocked(device: BluetoothDevice) {
        val owner = OwnedGatt(++nextGattGeneration, nextContext, device, SystemClock.elapsedRealtime())
        current = owner
        _connection.value = connectionState(owner, ConnectionPhase.Connecting, "Connecting")
        log(
            owner,
            "connect.generation",
            "source=${connectionSource(owner.context)} reason=${owner.context.reason} " +
                "association=${owner.context.associationId ?: "none"} " +
                "bond=${owner.context.bondState} priorGattRetired=${owner.context.priorGattRetired} " +
                "expectsBonding=${owner.context.expectsBonding} " +
                "retry=${owner.context.retry}/${GattRecoveryPolicy.MAX_CLEAN_RETRIES}",
        )
        log(owner, "connect.request", "source=${connectionSource(owner.context)} api=connectGatt")
        val pendingGatt = try {
            device.connectGatt(
                appContext,
                false,
                callback,
                BluetoothDevice.TRANSPORT_LE,
                BluetoothDevice.PHY_LE_1M_MASK,
            )
        } catch (error: Throwable) {
            val failure = GattTransportException("Android could not create a GATT client", null, GattFailureStage.Connect, error)
            fail(owner, failure)
            clearIfCurrent(owner)
            throw failure
        }
        owner.gatt = pendingGatt
        log(
            owner,
            "gatt.created",
            "source=${connectionSource(owner.context)} api=connectGatt autoConnect=false transport=LE phy=1M",
        )
        // An ordinary connect runs over an already-bonded link, so 15 s is generous. A connect that
        // is deliberately provoking LE bonding has Android's own pairing dialog inside it, and that
        // is human-paced; failing it on the normal deadline would look like a connect fault.
        val deadline = if (owner.context.expectsBonding) BONDING_CONNECT_TIMEOUT_MILLIS
                       else CONNECT_TIMEOUT_MILLIS
        try {
            withTimeout(deadline) { owner.ready.await() }
        } catch (timeout: TimeoutCancellationException) {
            val failure = GattTransportException("PicoSwitch2 connection timed out", null, GattFailureStage.Connect, timeout)
            fail(owner, failure)
            retireLocked(owner, "connect timeout", settleIdle = false)
            throw failure
        } catch (error: Throwable) {
            retireLocked(owner, "connect failure", settleIdle = false)
            throw error
        }
    }

    override fun markValidated() {
        val owner = current ?: return
        if (!owner.ready.isCompleted || owner.terminalFailure || owner.closed) return
        val balancedAccepted = runCatching {
            owner.gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_BALANCED) == true
        }.getOrDefault(false)
        log(owner, "gatt.priority", "source=setup priority=balanced apiAccepted=$balancedAccepted after=identity")
        _connection.value = connectionState(owner, ConnectionPhase.Connected, null)
        pairingCandidate = null
        log(owner, "gatt.ready", "management identity verified")
    }

    override suspend fun disconnect() {
        session.mutate {
            lifecycle.withLock { retireCurrentLocked("requested disconnect", settleIdle = true) }
            while (notifications.tryReceive().isSuccess) Unit
        }
    }

    override fun close() {
        val owner = current
        owner?.let { log(it, "gatt.close_requested", "reason=transport close source=app") }
        current = null
        pairingCandidate = null
        owner?.closeRequested = true
        owner?.let { log(it, "gatt.disconnect_requested", "reason=transport close source=app") }
        runCatching { owner?.gatt?.disconnect() }
        owner?.let(::closeExactlyOnce)
        owner?.ready?.cancel()
        owner?.writeReady?.cancel()
        while (notifications.tryReceive().isSuccess) Unit
        _connection.value = ConnectionState()
        notifications.trySend(ByteArray(0))
        lifecycleScope.cancel()
    }

    override suspend fun transact(command: String, timeoutMillis: Long): String {
        val sequence = nextCommandSequence.incrementAndGet()
        val queuedAt = SystemClock.elapsedRealtime()
        val workflow = ManagementDiagnosticContext.workflow()
        val queuedOwner = current
        val source = requestSource(workflow, queuedOwner?.context)
        diagnostics?.event(
            "management",
            "request.queued",
            "seq=$sequence type=${DiagnosticLog.commandType(command)} source=$source workflow=$workflow " +
                "gatt=${queuedOwner?.generation ?: "none"} gattObj=${gattIdentity(queuedOwner?.gatt)} " +
                "state=${_connection.value.phase} timeoutMs=$timeoutMillis",
        )
        // OFF THE MAIN THREAD, DELIBERATELY, AND THIS IS A CORRECTNESS FIX RATHER
        // THAN AN OPTIMISATION.
        //
        // Callers reach this from viewModelScope, whose default dispatcher is
        // Main. Everything below then resumes there: each fragment's write
        // callback, and every notification of the reply. A command is fragmented
        // to 20-byte ATT payloads, so one Amiibo chunk is several write
        // round-trips plus the notifications carrying its answer — dozens of
        // main-thread dispatches, all inside ONE timeout budget.
        //
        // That made the timeout a measure of how busy the UI was. Browsing a
        // large library while a transfer ran could delay those continuations
        // past the budget, and the timeout path calls invalidate(), which tears
        // the management session down: an upload stalling at an arbitrary chunk
        // and taking the adapter connection with it. Observed at offsets 64, 96
        // and 288 of the same file — arbitrary, which is what a scheduling
        // cause looks like and a protocol cause does not.
        //
        // The GATT API is callable from any thread and its callbacks already
        // arrive on binder threads, so nothing here wanted Main in the first
        // place; it was inherited from the caller.
        return withContext(Dispatchers.IO) { session.exchange {
            val startedAt = SystemClock.elapsedRealtime()
            val owner = current ?: throw ManagementException("Connect to the adapter first")
            val activeGatt = owner.gatt ?: throw ManagementException("Connect to the adapter first")
            val characteristic = owner.rx ?: throw ManagementException("Management service is not ready")
            if (!owner.ready.isCompleted || owner.terminalFailure || owner.closed) {
                throw ManagementException("Adapter management service is not ready")
            }
            // The adapter's wireless carrier is a one-slot cross-core bridge. Android can deliver the
            // final notification to this process before the firmware's next task turn has made that
            // slot available to core0 again. A new write in the observed 1-3 ms window was accepted by
            // GATT but intermittently produced no reply. Keep the logical single-flight guarantee and
            // include one bounded carrier turnaround between exchanges; do not inflate command timeouts.
            val turnaround = ManagementTurnaroundPolicy.delayMillis(
                SystemClock.elapsedRealtime(), owner.lastReplyAtElapsedMillis,
            )
            if (turnaround > 0L) delay(turnaround)
            while (notifications.tryReceive().isSuccess) Unit
            val now = SystemClock.elapsedRealtime()
            val trace = CommandTrace(
                sequence = sequence,
                type = DiagnosticLog.commandType(command),
                source = source,
                startedAtElapsedMillis = now,
            )
            owner.commandTrace = trace
            log(
                owner,
                "request.begin",
                "seq=${trace.sequence} type=${trace.type} source=${trace.source} workflow=$workflow " +
                    "queuedMs=${now - queuedAt} preflightMs=${now - startedAt}",
            )
            diagnostics?.commandStarted(
                command,
                "seq=${trace.sequence} gatt=${owner.generation} mtu=${owner.negotiatedMtu} " +
                    "source=${trace.source} caller=$workflow " +
                    "bridge=${ManagementDiagnosticContext.bridgePhase()} " +
                    "personality=${ManagementDiagnosticContext.personalityPhase()} " +
                    "sinceReadyMs=${elapsedOrNone(now, owner.readyAtElapsedMillis)} " +
                    "sinceReplyMs=${elapsedOrNone(now, owner.lastReplyAtElapsedMillis)}",
            )
            try {
                withTimeout(timeoutMillis) {
                    val chunks = BleManagementContract.commandChunks(
                        command,
                        BleManagementContract.ATT_PAYLOAD_WITH_DEFAULT_MTU,
                    )
                    for ((index, part) in chunks.withIndex()) {
                        owner.writeReady = CompletableDeferred()
                        log(
                            owner,
                            "request.write_begin",
                            "seq=${trace.sequence} type=${trace.type} source=${trace.source} " +
                                "characteristic=rx bytes=${part.size} chunk=${index + 1}/${chunks.size}",
                        )
                        val apiResult = writeCharacteristic(activeGatt, characteristic, part)
                        log(
                            owner,
                            "request.write_api",
                            "seq=${trace.sequence} type=${trace.type} source=${trace.source} " +
                                "characteristic=rx bytes=${part.size} chunk=${index + 1}/${chunks.size} " +
                                "apiAccepted=${apiResult.accepted} apiStatus=${apiResult.status ?: "boolean"}",
                        )
                        if (!apiResult.accepted) {
                            throw GattTransportException("Could not send '$command'", null, GattFailureStage.Command)
                        }
                        owner.writeReady?.await()
                    }
                    val assembler = BleReplyAssembler()
                    while (true) {
                        val part = notifications.receive()
                        if (part.isEmpty() && !_connection.value.connected) {
                            throw ManagementException("Adapter disconnected during '$command'")
                        }
                        assembler.accept(part)?.let { response ->
                            val completedAt = SystemClock.elapsedRealtime()
                            owner.lastReplyAtElapsedMillis = completedAt
                            log(
                                owner,
                                "request.complete",
                                "seq=${trace.sequence} type=${trace.type} source=${trace.source} " +
                                    "elapsedMs=${completedAt - trace.startedAtElapsedMillis} " +
                                    "responseBytes=${response.encodeToByteArray().size}",
                            )
                            diagnostics?.commandFinished(
                                command,
                                response.encodeToByteArray().size,
                                "seq=${trace.sequence} gatt=${owner.generation} source=${trace.source} " +
                                    "elapsedMs=${completedAt - trace.startedAtElapsedMillis} " +
                                    "firstNotifyMs=${elapsedOrNone(trace.firstNotificationAtElapsedMillis, trace.startedAtElapsedMillis)} " +
                                    "notifications=${trace.notificationCount}/${trace.notificationBytes}B " +
                                    "writes=${trace.writeCallbacks}",
                            )
                            return@withTimeout response
                        }
                    }
                    @Suppress("UNREACHABLE_CODE") ""
                }
            } catch (error: TimeoutCancellationException) {
                log(
                    owner,
                    "request.timeout",
                    "seq=${trace.sequence} type=${trace.type} source=${trace.source} timeoutMs=$timeoutMillis",
                )
                logTerminal(owner, trace, error)
                invalidate(owner, "Adapter did not reply. Reconnect to start a clean management session.")
                diagnostics?.error("management", DiagnosticLog.commandType(command), error)
                throw ManagementException(
                    "${DiagnosticLog.commandType(command)} timed out after ${timeoutMillis / 1000} seconds",
                    error,
                )
            } catch (error: ManagementReplyTooLargeException) {
                logTerminal(owner, trace, error)
                invalidate(owner, "Reply was too large; reconnect to start a clean management session")
                diagnostics?.error("management", DiagnosticLog.commandType(command), error)
                throw error
            } catch (error: ManagementException) {
                logTerminal(owner, trace, error)
                diagnostics?.error("management", DiagnosticLog.commandType(command), error)
                throw error
            } finally {
                log(
                    owner,
                    "request.release",
                    "seq=${trace.sequence} type=${trace.type} source=${trace.source} " +
                        "elapsedMs=${SystemClock.elapsedRealtime() - trace.startedAtElapsedMillis}",
                )
                if (owner.commandTrace === trace) owner.commandTrace = null
            }
        } }
    }

    private fun logTerminal(owner: OwnedGatt, trace: CommandTrace, error: Throwable) {
        log(
            owner,
            "command.terminal",
            "seq=${trace.sequence} type=${trace.type} source=${trace.source} " +
                "elapsedMs=${SystemClock.elapsedRealtime() - trace.startedAtElapsedMillis} " +
                "notifications=${trace.notificationCount}/${trace.notificationBytes}B writes=${trace.writeCallbacks} " +
                "error=${error.javaClass.simpleName}",
        )
    }

    private fun invalidate(owner: OwnedGatt, message: String) {
        owner.terminalFailure = true
        _connection.value = connectionState(owner, ConnectionPhase.Failed, message)
        lifecycleScope.launch {
            lifecycle.withLock {
                if (current === owner) retireLocked(owner, "invalidated session", settleIdle = false)
            }
        }
    }

    private suspend fun retireCurrentLocked(reason: String, settleIdle: Boolean) {
        current?.let { retireLocked(it, reason, settleIdle) }
        if (settleIdle && current == null) _connection.value = ConnectionState()
    }

    private suspend fun retireLocked(owner: OwnedGatt, reason: String, settleIdle: Boolean) {
        if (owner.closed) {
            clearIfCurrent(owner)
            if (settleIdle) _connection.value = ConnectionState()
            return
        }
        owner.closeRequested = true
        log(owner, "gatt.close_requested", "reason=$reason")
        if (settleIdle) _connection.value = connectionState(owner, ConnectionPhase.Disconnecting, null)
        log(owner, "gatt.disconnect_requested", "reason=$reason source=app")
        runCatching { owner.gatt?.disconnect() }
        val callbackObserved = withTimeoutOrNull(DISCONNECT_TIMEOUT_MILLIS) {
            owner.disconnected.await()
            true
        } ?: false
        closeExactlyOnce(owner)
        clearIfCurrent(owner)
        owner.ready.completeExceptionally(ManagementException("Management GATT retired: $reason"))
        owner.writeReady?.completeExceptionally(ManagementException("Management GATT retired: $reason"))
        log(
            owner,
            "gatt.closed",
            "reason=$reason callback=$callbackObserved elapsedMs=${SystemClock.elapsedRealtime() - owner.startedAtElapsedMillis}",
        )
        if (settleIdle) _connection.value = ConnectionState()
    }

    private fun fail(owner: OwnedGatt, error: GattTransportException) {
        owner.terminalFailure = true
        _connection.value = connectionState(owner, ConnectionPhase.Failed, error.message)
        log(owner, "gatt.error", "stage=${error.stage.diagnosticName} status=${error.status ?: "none"}")
        owner.ready.completeExceptionally(error)
        owner.writeReady?.completeExceptionally(error)
    }

    private fun closeExactlyOnce(owner: OwnedGatt) {
        synchronized(owner) {
            if (owner.closed) return
            owner.closed = true
            log(
                owner,
                "gatt.close_api",
                "source=${if (owner.closeRequested) "app" else "stack"} operation=close",
            )
            runCatching { owner.gatt?.close() }
            owner.rx = null
            owner.tx = null
        }
    }

    private fun clearIfCurrent(owner: OwnedGatt) {
        if (current === owner) current = null
    }

    private fun ownerFor(gatt: BluetoothGatt): OwnedGatt? {
        val owner = current ?: return null
        return owner.takeIf {
            it.gatt === gatt && GattCallbackAuthority.isAuthoritative(
                currentGeneration = current?.generation,
                callbackGeneration = it.generation,
                callbackOwnerClosed = it.closed,
            )
        }
    }

    private fun staleCallback(kind: String, gatt: BluetoothGatt) {
        diagnostics?.event(
            "management",
            "gatt.stale_callback",
            "callback=$kind callbackGatt=${gattIdentity(gatt)} currentGatt=${gattIdentity(current?.gatt)} " +
                "currentGeneration=${current?.generation ?: "none"} state=${_connection.value.phase} ignored=true",
        )
        // A callback from an already-retired Android client has no authority. Its owning
        // generation already closed it exactly once (or is completing that retirement now).
    }

    private fun connectionState(owner: OwnedGatt, phase: ConnectionPhase, message: String?) = ConnectionState(
        phase = phase,
        deviceName = safeName(owner.device),
        address = owner.device.address,
        message = message,
        attempt = (owner.context.logicalAttempt.takeIf { it > 0 } ?: owner.generation).toInt(),
    )

    private fun log(owner: OwnedGatt, event: String, detail: String = "") {
        val prefix = "attempt=${owner.context.logicalAttempt.takeIf { it > 0 } ?: owner.generation} " +
            "gatt=${owner.generation} gattObj=${gattIdentity(owner.gatt)} state=${_connection.value.phase} " +
            "reason=${owner.context.reason} retry=${owner.context.retry}/${GattRecoveryPolicy.MAX_CLEAN_RETRIES}"
        diagnostics?.event("management", event, if (detail.isBlank()) prefix else "$prefix $detail")
    }

    private fun log(context: ManagementConnectionContext, event: String, detail: String = "") {
        val prefix = "attempt=${context.logicalAttempt} reason=${context.reason} retry=${context.retry}/${GattRecoveryPolicy.MAX_CLEAN_RETRIES}"
        diagnostics?.event("management", event, if (detail.isBlank()) prefix else "$prefix $detail")
    }

    private fun safeName(device: BluetoothDevice): String? = runCatching { device.name }.getOrNull()

    private fun elapsedOrNone(later: Long, earlier: Long): String =
        if (earlier == 0L || later < earlier) "none" else (later - earlier).toString()

    private data class GattApiResult(val accepted: Boolean, val status: Int?)

    private fun writeCharacteristic(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        bytes: ByteArray,
    ): GattApiResult {
        return if (Build.VERSION.SDK_INT >= 33) {
            val status = gatt.writeCharacteristic(
                characteristic,
                bytes,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
            )
            GattApiResult(status == BluetoothStatusCodes.SUCCESS, status)
        } else {
            @Suppress("DEPRECATION")
            run {
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                characteristic.value = bytes
                GattApiResult(gatt.writeCharacteristic(characteristic), null)
            }
        }
    }

    private fun requestSource(workflow: String, context: ManagementConnectionContext?): String = when {
        workflow == "background-input-poll" -> "background"
        workflow == "connect-identity" && context?.reason == "first-pair" -> "setup"
        workflow == "connect-identity" -> "reconnect"
        else -> "foreground"
    }

    private fun connectionSource(context: ManagementConnectionContext): String =
        if (context.reason == "first-pair") "setup" else "reconnect"

    private fun gattIdentity(gatt: BluetoothGatt?): String =
        gatt?.let { "0x${System.identityHashCode(it).toUInt().toString(16)}" } ?: "none"

    private fun writeDescriptor(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, bytes: ByteArray): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            gatt.writeDescriptor(descriptor, bytes) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                descriptor.value = bytes
                gatt.writeDescriptor(descriptor)
            }
        }
    }

    companion object {
        private const val CONNECT_TIMEOUT_MILLIS = 15_000L
        private const val BONDING_CONNECT_TIMEOUT_MILLIS = 60_000L
        private const val SCAN_TIMEOUT_MILLIS = 15_000L
        private const val DISCONNECT_TIMEOUT_MILLIS = 1_250L
        private const val REQUESTED_ATT_MTU = 517
        private val CLIENT_CHARACTERISTIC_CONFIG = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private val TX_UUID = UUID.fromString(BleManagementContract.TX_UUID)
    }
}
