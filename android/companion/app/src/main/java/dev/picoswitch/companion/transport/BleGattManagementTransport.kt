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
import dev.picoswitch.companion.diagnostics.DiagnosticLog
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
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.UUID
import kotlin.coroutines.resume

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
        val startedAtMillis: Long,
    ) {
        @Volatile var gatt: BluetoothGatt? = null
        @Volatile var rx: BluetoothGattCharacteristic? = null
        @Volatile var tx: BluetoothGattCharacteristic? = null
        @Volatile var writeReady: CompletableDeferred<Unit>? = null
        @Volatile var closeRequested = false
        @Volatile var terminalFailure = false
        @Volatile var closed = false
        val ready = CompletableDeferred<Unit>()
        val disconnected = CompletableDeferred<Unit>()
    }

    @Volatile private var current: OwnedGatt? = null
    @Volatile private var nextContext = ManagementConnectionContext()
    private var nextGattGeneration = 0L

    override fun prepareConnection(context: ManagementConnectionContext) {
        nextContext = context
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
                "status=$status state=$newState elapsedMs=${System.currentTimeMillis() - owner.startedAtMillis}",
            )
            if (status != BluetoothGatt.GATT_SUCCESS) {
                val failure = GattTransportException(
                    "Bluetooth connection failed ($status)", status, GattFailureStage.Connect,
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
                    _connection.value = connectionState(owner, ConnectionPhase.Connecting, "Discovering adapter services")
                    if (!gatt.discoverServices()) {
                        fail(owner, GattTransportException(
                            "Service discovery could not start", null, GattFailureStage.Services,
                        ))
                    }
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

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val owner = ownerFor(gatt) ?: return staleCallback("services", gatt)
            log(owner, "gatt.services", "status=$status")
            if (status != BluetoothGatt.GATT_SUCCESS) {
                return fail(owner, GattTransportException(
                    "Service discovery failed ($status)", status, GattFailureStage.Services,
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
            if (!gatt.setCharacteristicNotification(output, true)) return fail(owner, GattTransportException(
                "Could not enable management replies", null, GattFailureStage.Subscribe,
            ))
            val ccc = output.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG)
                ?: return fail(owner, GattTransportException(
                    "Management notification descriptor is missing", null, GattFailureStage.Subscribe,
                ))
            if (!writeDescriptor(gatt, ccc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)) {
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
                owner.ready.complete(Unit)
            } else {
                fail(owner, GattTransportException(
                    "Notification subscription failed ($status)", status, GattFailureStage.Subscribe,
                ))
            }
        }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (ownerFor(gatt) != null && characteristic.uuid == TX_UUID) {
                notifications.trySend(characteristic.value.copyOf())
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (ownerFor(gatt) != null && characteristic.uuid == TX_UUID) notifications.trySend(value.copyOf())
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            ownerFor(gatt)?.let { owner ->
                if (status == BluetoothGatt.GATT_SUCCESS) owner.writeReady?.complete(Unit)
                else owner.writeReady?.completeExceptionally(GattTransportException(
                    "Bluetooth write failed ($status)", status, GattFailureStage.Command,
                ))
            }
        }
    }

    override suspend fun scanAndConnect() = scanAndConnectInternal(expectedAddress = null)

    override suspend fun scanAndConnect(expectedAddress: String) = scanAndConnectInternal(expectedAddress.uppercase())

    private suspend fun scanAndConnectInternal(expectedAddress: String?) = lifecycle.withLock {
        retireCurrentLocked("before scan", settleIdle = true)
        val bluetooth = adapter ?: throw ManagementException("Bluetooth is not available on this device")
        if (!bluetooth.isEnabled) throw ManagementException("Turn on Bluetooth to find PicoSwitch2")
        val scanner = bluetooth.bluetoothLeScanner ?: throw ManagementException("Bluetooth LE scanning is unavailable")
        val context = nextContext
        _connection.value = ConnectionState(
            phase = ConnectionPhase.Scanning,
            message = if (expectedAddress == null) "Looking for PicoSwitch2" else "Looking for the saved PicoSwitch2",
            attempt = context.logicalAttempt.toInt(),
        )
        diagnostics?.event(
            "relationship", "connect.scan_fallback",
            "attempt=${context.logicalAttempt} reason=${context.reason} expected=${if (expectedAddress == null) "any" else "saved"}",
        )
        val device = try {
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
                            scanner.stopScan(this)
                            continuation.resume(result.device)
                        }

                        override fun onScanFailed(errorCode: Int) {
                            if (continuation.isActive) {
                                continuation.cancel(ManagementException("Bluetooth scan failed ($errorCode)"))
                            }
                        }
                    }
                    continuation.invokeOnCancellation { scanner.stopScan(scanCallback) }
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
        connectDeviceLocked(device)
    }

    override suspend fun connectKnown(address: String) = lifecycle.withLock {
        retireCurrentLocked("before direct connect", settleIdle = true)
        val bluetooth = adapter ?: throw ManagementException("Bluetooth is not available on this device")
        if (!bluetooth.isEnabled) throw ManagementException("Turn on Bluetooth to connect to PicoSwitch2")
        val device = runCatching { bluetooth.getRemoteDevice(address) }
            .getOrElse { throw ManagementException("The saved PicoSwitch2 address is invalid", it) }
        connectDeviceLocked(device)
    }

    private suspend fun connectDeviceLocked(device: BluetoothDevice) {
        val owner = OwnedGatt(++nextGattGeneration, nextContext, device, System.currentTimeMillis())
        current = owner
        _connection.value = connectionState(owner, ConnectionPhase.Connecting, "Connecting")
        log(
            owner,
            "connect.generation",
            "reason=${owner.context.reason} association=${owner.context.associationId ?: "none"} " +
                "bond=${owner.context.bondState} priorGattRetired=${owner.context.priorGattRetired} " +
                "retry=${owner.context.retry}/${GattRecoveryPolicy.MAX_CLEAN_RETRIES}",
        )
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
        try {
            withTimeout(CONNECT_TIMEOUT_MILLIS) { owner.ready.await() }
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
        _connection.value = connectionState(owner, ConnectionPhase.Connected, null)
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
        current = null
        owner?.closeRequested = true
        runCatching { owner?.gatt?.disconnect() }
        owner?.let(::closeExactlyOnce)
        owner?.ready?.cancel()
        owner?.writeReady?.cancel()
        while (notifications.tryReceive().isSuccess) Unit
        _connection.value = ConnectionState()
        notifications.trySend(ByteArray(0))
        lifecycleScope.cancel()
    }

    override suspend fun transact(command: String, timeoutMillis: Long): String = session.exchange {
        val owner = current ?: throw ManagementException("Connect to the adapter first")
        val activeGatt = owner.gatt ?: throw ManagementException("Connect to the adapter first")
        val characteristic = owner.rx ?: throw ManagementException("Management service is not ready")
        if (!owner.ready.isCompleted || owner.terminalFailure || owner.closed) {
            throw ManagementException("Adapter management service is not ready")
        }
        while (notifications.tryReceive().isSuccess) Unit
        diagnostics?.commandStarted(command)
        try {
            withTimeout(timeoutMillis) {
                for (part in BleManagementContract.commandChunks(command, BleManagementContract.ATT_PAYLOAD_WITH_DEFAULT_MTU)) {
                    owner.writeReady = CompletableDeferred()
                    if (!writeCharacteristic(activeGatt, characteristic, part)) {
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
                        diagnostics?.commandFinished(command, response.encodeToByteArray().size)
                        return@withTimeout response
                    }
                }
                @Suppress("UNREACHABLE_CODE") ""
            }
        } catch (error: TimeoutCancellationException) {
            invalidate(owner, "Adapter did not reply. Reconnect to start a clean management session.")
            diagnostics?.error("management", DiagnosticLog.commandType(command), error)
            throw ManagementException("${DiagnosticLog.commandType(command)} timed out after ${timeoutMillis / 1000} seconds", error)
        } catch (error: ManagementReplyTooLargeException) {
            invalidate(owner, "Reply was too large; reconnect to start a clean management session")
            diagnostics?.error("management", DiagnosticLog.commandType(command), error)
            throw error
        } catch (error: ManagementException) {
            diagnostics?.error("management", DiagnosticLog.commandType(command), error)
            throw error
        }
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
            "reason=$reason callback=$callbackObserved elapsedMs=${System.currentTimeMillis() - owner.startedAtMillis}",
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
        diagnostics?.event("management", "gatt.stale_callback", "callback=$kind ignored")
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
            "gatt=${owner.generation} reason=${owner.context.reason} retry=${owner.context.retry}/${GattRecoveryPolicy.MAX_CLEAN_RETRIES}"
        diagnostics?.event("management", event, if (detail.isBlank()) prefix else "$prefix $detail")
    }

    private fun safeName(device: BluetoothDevice): String? = runCatching { device.name }.getOrNull()

    private fun writeCharacteristic(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, bytes: ByteArray): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            gatt.writeCharacteristic(
                characteristic,
                bytes,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
            ) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                characteristic.value = bytes
                gatt.writeCharacteristic(characteristic)
            }
        }
    }

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
        private const val SCAN_TIMEOUT_MILLIS = 15_000L
        private const val DISCONNECT_TIMEOUT_MILLIS = 1_250L
        private val CLIENT_CHARACTERISTIC_CONFIG = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private val TX_UUID = UUID.fromString(BleManagementContract.TX_UUID)
    }
}
