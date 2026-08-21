package dev.picoswitch.companion.protocol

import android.bluetooth.BluetoothDevice
import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.management.ManagementChannel
import kotlinx.coroutines.flow.StateFlow

interface ManagementTransport : ManagementChannel {
    val connection: StateFlow<ConnectionState>
    fun prepareConnection(context: ManagementConnectionContext) = Unit
    /** Discover the advertised management endpoint without opening GATT. */
    suspend fun discover(): DiscoveredManagementPeer =
        throw ManagementException("BLE management discovery is unavailable")
    suspend fun scanAndConnect()
    suspend fun scanAndConnect(expectedAddress: String) = scanAndConnect()
    suspend fun connectKnown(address: String) = scanAndConnect()
    suspend fun disconnect()
    /** Promote a subscribed GATT link only after the management identity reply is verified. */
    fun markValidated() = Unit
    override suspend fun transact(command: String, timeoutMillis: Long): String
    fun close() = Unit
}

data class DiscoveredManagementPeer(
    /** The exact BluetoothDevice delivered by the filtered BLE ScanResult. */
    val device: BluetoothDevice,
    val displayName: String? = null,
) {
    val address: String get() = device.address
}

data class ManagementConnectionContext(
    val logicalAttempt: Long = 0,
    val reason: String = "unspecified",
    val associationId: Int? = null,
    val bondState: String = "unknown",
    val retry: Int = 0,
    val priorGattRetired: Boolean = false,
    val useDiscoveredPeer: Boolean = false,
    /**
     * This connection is expected to provoke Android's LE bonding procedure rather than run over
     * an already-bonded link, so the user's own pairing dialog sits inside the connect deadline.
     * Only the compatibility path in [dev.picoswitch.companion.data.AdapterBondStarter] sets this.
     */
    val expectsBonding: Boolean = false,
)

typealias ManagementException = dev.picoswitch.management.ManagementException
typealias ManagementReplyTooLargeException = dev.picoswitch.management.ManagementReplyTooLargeException
typealias AdapterCommandException = dev.picoswitch.management.AdapterCommandException
