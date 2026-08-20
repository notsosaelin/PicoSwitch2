package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.management.ManagementChannel
import kotlinx.coroutines.flow.StateFlow

interface ManagementTransport : ManagementChannel {
    val connection: StateFlow<ConnectionState>
    suspend fun scanAndConnect()
    suspend fun connectKnown(address: String) = scanAndConnect()
    suspend fun disconnect()
    override suspend fun transact(command: String, timeoutMillis: Long): String
    fun close() = Unit
}

typealias ManagementException = dev.picoswitch.management.ManagementException
typealias ManagementReplyTooLargeException = dev.picoswitch.management.ManagementReplyTooLargeException
typealias AdapterCommandException = dev.picoswitch.management.AdapterCommandException
