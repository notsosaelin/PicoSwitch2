package dev.picoswitch.companion.protocol

import dev.picoswitch.companion.model.ConnectionState
import kotlinx.coroutines.flow.StateFlow

interface ManagementTransport {
    val connection: StateFlow<ConnectionState>
    suspend fun scanAndConnect()
    suspend fun disconnect()
    suspend fun transact(command: String, timeoutMillis: Long = 10_000): String
}

class ManagementException(message: String, cause: Throwable? = null) : Exception(message, cause)

class AdapterCommandException(
    val command: String,
    val code: Int?,
    message: String,
) : Exception(message)
