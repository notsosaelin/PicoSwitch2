package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.management.ManagementChannel
import kotlinx.coroutines.flow.StateFlow

interface ManagementTransport : ManagementChannel {
    val connection: StateFlow<ConnectionState>
    fun prepareConnection(context: ManagementConnectionContext) = Unit
    suspend fun scanAndConnect()
    suspend fun scanAndConnect(expectedAddress: String) = scanAndConnect()
    suspend fun connectKnown(address: String) = scanAndConnect()
    suspend fun disconnect()
    /** Promote a subscribed GATT link only after the management identity reply is verified. */
    fun markValidated() = Unit
    override suspend fun transact(command: String, timeoutMillis: Long): String
    fun close() = Unit
}

data class ManagementConnectionContext(
    val logicalAttempt: Long = 0,
    val reason: String = "unspecified",
    val associationId: Int? = null,
    val bondState: String = "unknown",
    val retry: Int = 0,
    val priorGattRetired: Boolean = false,
)

typealias ManagementException = dev.picoswitch.management.ManagementException
typealias ManagementReplyTooLargeException = dev.picoswitch.management.ManagementReplyTooLargeException
typealias AdapterCommandException = dev.picoswitch.management.AdapterCommandException
