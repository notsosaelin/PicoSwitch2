package dev.picoswitch.companion.transport

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.protocol.ManagementException
import dev.picoswitch.companion.protocol.ManagementProtocol
import dev.picoswitch.companion.protocol.ManagementTransport
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.ByteArrayOutputStream
import java.util.UUID
import kotlin.coroutines.resume

@SuppressLint("MissingPermission")
class BleGattManagementTransport(context: Context) : ManagementTransport {
    private val appContext = context.applicationContext
    private val manager = appContext.getSystemService(BluetoothManager::class.java)
    private val adapter get() = manager?.adapter
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val commandMutex = Mutex()
    private val notifications = Channel<ByteArray>(Channel.UNLIMITED)
    private val _connection = MutableStateFlow(ConnectionState())
    override val connection: StateFlow<ConnectionState> = _connection

    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var rx: BluetoothGattCharacteristic? = null
    @Volatile private var tx: BluetoothGattCharacteristic? = null
    @Volatile private var connectReady: CompletableDeferred<Unit>? = null
    @Volatile private var descriptorReady: CompletableDeferred<Unit>? = null
    @Volatile private var writeReady: CompletableDeferred<Unit>? = null
    @Volatile private var requestedDisconnect = false

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                gatt = g
                _connection.value = _connection.value.copy(phase = ConnectionPhase.Connecting, message = "Discovering adapter services")
                if (!g.discoverServices()) failConnection("Service discovery could not start")
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                rx = null
                tx = null
                val expected = requestedDisconnect
                requestedDisconnect = false
                connectReady?.completeExceptionally(ManagementException(if (expected) "Disconnected" else "Adapter disconnected"))
                writeReady?.completeExceptionally(ManagementException("Adapter disconnected during command"))
                _connection.value = ConnectionState(
                    phase = if (expected) ConnectionPhase.Idle else ConnectionPhase.Reconnecting,
                    deviceName = safeName(g.device), address = g.device.address,
                    message = if (expected) null else "Connection lost. Tap reconnect when the adapter is available.",
                )
                g.close()
                if (gatt === g) gatt = null
            } else if (status != BluetoothGatt.GATT_SUCCESS) {
                failConnection("Bluetooth error $status")
                g.close()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) return failConnection("Service discovery failed ($status)")
            val service = g.getService(UUID.fromString(ManagementProtocol.SERVICE_UUID))
                ?: return failConnection("This device does not expose PicoSwitch management")
            rx = service.getCharacteristic(UUID.fromString(ManagementProtocol.RX_UUID))
            tx = service.getCharacteristic(UUID.fromString(ManagementProtocol.TX_UUID))
            val output = tx ?: return failConnection("Management notification characteristic is missing")
            if (rx == null) return failConnection("Management command characteristic is missing")
            if (!g.setCharacteristicNotification(output, true)) return failConnection("Could not enable management replies")
            val ccc = output.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG)
                ?: return failConnection("Management notification descriptor is missing")
            descriptorReady = CompletableDeferred()
            if (!writeDescriptor(g, ccc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)) {
                failConnection("Could not subscribe to management replies")
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                descriptorReady?.complete(Unit)
                val device = g.device
                _connection.value = ConnectionState(ConnectionPhase.Connected, safeName(device) ?: "PicoSwitch2", device.address)
                connectReady?.complete(Unit)
            } else {
                descriptorReady?.completeExceptionally(ManagementException("Notification subscription failed ($status)"))
                failConnection("Notification subscription failed ($status)")
            }
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == UUID.fromString(ManagementProtocol.TX_UUID)) notifications.trySend(characteristic.value.copyOf())
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            if (characteristic.uuid == UUID.fromString(ManagementProtocol.TX_UUID)) notifications.trySend(value.copyOf())
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            completeWrite(status)
        }

    }

    override suspend fun scanAndConnect() {
        disconnect()
        val bt = adapter ?: throw ManagementException("Bluetooth is not available on this device")
        if (!bt.isEnabled) throw ManagementException("Turn on Bluetooth to find PicoSwitch2")
        val scanner = bt.bluetoothLeScanner ?: throw ManagementException("Bluetooth LE scanning is unavailable")
        _connection.value = ConnectionState(ConnectionPhase.Scanning, message = "Looking for PicoSwitch2")

        val device = withTimeout(15_000) {
            suspendCancellableCoroutine { continuation ->
                val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid.fromString(ManagementProtocol.SERVICE_UUID)).build()
                val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
                val scanCallback = object : ScanCallback() {
                    override fun onScanResult(callbackType: Int, result: ScanResult) {
                        if (continuation.isActive) {
                            scanner.stopScan(this)
                            continuation.resume(result.device)
                        }
                    }
                    override fun onScanFailed(errorCode: Int) {
                        if (continuation.isActive) continuation.cancel(ManagementException("Bluetooth scan failed ($errorCode)"))
                    }
                }
                continuation.invokeOnCancellation { scanner.stopScan(scanCallback) }
                scanner.startScan(listOf(filter), settings, scanCallback)
            }
        }

        _connection.value = ConnectionState(ConnectionPhase.Connecting, safeName(device), device.address, "Connecting")
        requestedDisconnect = false
        connectReady = CompletableDeferred()
        gatt = device.connectGatt(appContext, false, callback, BluetoothDevice.TRANSPORT_LE, BluetoothDevice.PHY_LE_1M_MASK)
        withTimeout(15_000) { connectReady?.await() }
    }

    override suspend fun disconnect() {
        commandMutex.withLock {
            requestedDisconnect = true
            val active = gatt
            if (active != null) {
                _connection.value = _connection.value.copy(phase = ConnectionPhase.Disconnecting, message = null)
                active.disconnect()
                delay(150)
                active.close()
            }
            gatt = null
            rx = null
            tx = null
            while (notifications.tryReceive().isSuccess) Unit
            _connection.value = ConnectionState()
        }
    }

    override suspend fun transact(command: String, timeoutMillis: Long): String = commandMutex.withLock {
        val activeGatt = gatt ?: throw ManagementException("Connect to the adapter first")
        val characteristic = rx ?: throw ManagementException("Management service is not ready")
        if (!_connection.value.connected) throw ManagementException("Adapter is not connected")
        while (notifications.tryReceive().isSuccess) Unit
        val mtuPayload = (characteristic.writeType.let { 20 }).coerceAtLeast(ManagementProtocol.MIN_GATT_PAYLOAD)
        try {
            withTimeout(timeoutMillis) {
                for (part in ManagementProtocol.chunks(command, mtuPayload)) {
                    writeReady = CompletableDeferred()
                    if (!writeCharacteristic(activeGatt, characteristic, part)) throw ManagementException("Could not send '$command'")
                    writeReady?.await()
                }
                val buffer = ByteArrayOutputStream()
                while (true) {
                    val part = notifications.receive()
                    for (byte in part) {
                        if (byte.toInt() == '\n'.code) return@withTimeout buffer.toString(Charsets.UTF_8.name()).trimEnd('\r')
                        buffer.write(byte.toInt())
                        if (buffer.size() > ManagementProtocol.MAX_REPLY_BYTES) throw ManagementException("Adapter reply exceeded ${ManagementProtocol.MAX_REPLY_BYTES} bytes")
                    }
                }
                @Suppress("UNREACHABLE_CODE") ""
            }
        } catch (error: TimeoutCancellationException) {
            // Replies have no request ID. A late reply must never become the next command's reply.
            requestedDisconnect = true
            activeGatt.disconnect()
            activeGatt.close()
            if (gatt === activeGatt) gatt = null
            rx = null
            tx = null
            _connection.value = ConnectionState(
                ConnectionPhase.Failed, _connection.value.deviceName, _connection.value.address,
                "Adapter did not reply. Reconnect to start a clean management session.",
            )
            throw ManagementException("'$command' timed out after ${timeoutMillis / 1000} seconds", error)
        }
    }

    private fun completeWrite(status: Int) {
        if (status == BluetoothGatt.GATT_SUCCESS) writeReady?.complete(Unit)
        else writeReady?.completeExceptionally(ManagementException("Bluetooth write failed ($status)"))
    }

    private fun failConnection(message: String) {
        _connection.value = _connection.value.copy(phase = ConnectionPhase.Failed, message = message)
        connectReady?.completeExceptionally(ManagementException(message))
    }

    private fun safeName(device: BluetoothDevice): String? = runCatching { device.name }.getOrNull()

    private fun writeCharacteristic(g: BluetoothGatt, c: BluetoothGattCharacteristic, bytes: ByteArray): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run { c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT; c.value = bytes; g.writeCharacteristic(c) }
        }
    }

    private fun writeDescriptor(g: BluetoothGatt, d: BluetoothGattDescriptor, bytes: ByteArray): Boolean {
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, bytes) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run { d.value = bytes; g.writeDescriptor(d) }
        }
    }

    companion object {
        private val CLIENT_CHARACTERISTIC_CONFIG = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
