package dev.picoswitch.management

/**
 * One already-connected logical management channel.
 *
 * Discovery, pairing, operating-system peer handles, GATT lifecycle, and UI
 * state intentionally do not belong here. Implementations must return exactly
 * one complete reply for the command or fail the current session.
 */
interface ManagementChannel {
    suspend fun transact(
        command: String,
        timeoutMillis: Long = DEFAULT_TIMEOUT_MILLIS,
    ): String

    companion object {
        const val DEFAULT_TIMEOUT_MILLIS = 10_000L
    }
}

open class ManagementException(message: String, cause: Throwable? = null) : Exception(message, cause)

class ManagementReplyTooLargeException(message: String) : ManagementException(message)

class AdapterCommandException(
    val command: String,
    val code: Int?,
    val adapterMessage: String,
) : ManagementException(adapterMessage)

class ManagementProtocolException(message: String, cause: Throwable? = null) :
    ManagementException(message, cause)

class ManagementPaginationException(message: String) : ManagementException(message)

fun AdapterCommandException.isUnsupported(): Boolean =
    adapterMessage.contains("unknown command", ignoreCase = true) ||
        adapterMessage.contains("unavailable", ignoreCase = true)
