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

/** The adapter answered, but with something this build cannot use. */
open class ManagementProtocolException(message: String, cause: Throwable? = null) :
    ManagementException(message, cause)

/**
 * A multi-reply read did not reconstruct a complete, consistent result.
 *
 * Derives from [ManagementProtocolException] because that is what it is: the
 * adapter said something unusable. They were siblings, so a handler that meant
 * "the adapter's answer was bad" had to name both, and one that named only the
 * base type let a pagination failure escape as an unhandled transport error.
 */
class ManagementPaginationException(message: String) : ManagementProtocolException(message)

fun AdapterCommandException.isUnsupported(): Boolean =
    adapterMessage.equals("unknown command", ignoreCase = true) ||
        adapterMessage.equals("unavailable", ignoreCase = true) ||
        adapterMessage.startsWith("unavailable in ", ignoreCase = true) ||
        adapterMessage.equals("command unavailable over Bluetooth", ignoreCase = true)
