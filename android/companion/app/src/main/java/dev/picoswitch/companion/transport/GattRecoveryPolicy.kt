package dev.picoswitch.companion.transport

enum class GattFailureStage(val diagnosticName: String) {
    Connect("connect"),
    Services("services"),
    Subscribe("subscribe"),
    Command("command"),
}

class GattTransportException(
    message: String,
    val status: Int?,
    val stage: GattFailureStage,
    cause: Throwable? = null,
) : dev.picoswitch.management.ManagementException(message, cause)

/** Small, bounded policy. Status 133 is Android's long-standing generic stack failure. */
object GattRecoveryPolicy {
    const val GENERIC_STACK_ERROR = 133
    const val CONNECTION_CONGESTED = 0x8f
    const val CONNECTION_TIMEOUT = 0x93
    const val MAX_CLEAN_RETRIES = 1
    const val RETRY_BACKOFF_MILLIS = 350L

    fun isRetryable(error: Throwable): Boolean {
        val failure = generateSequence(error) { it.cause }.filterIsInstance<GattTransportException>().firstOrNull()
            ?: return false
        return failure.stage == GattFailureStage.Connect && failure.status in setOf(
            GENERIC_STACK_ERROR,
            CONNECTION_CONGESTED,
            CONNECTION_TIMEOUT,
        )
    }

    fun shouldRetry(error: Throwable, retriesUsed: Int): Boolean =
        retriesUsed < MAX_CLEAN_RETRIES && isRetryable(error)
}

/** Pure authority check used before any Android callback may mutate session state. */
object GattCallbackAuthority {
    fun isAuthoritative(currentGeneration: Long?, callbackGeneration: Long, callbackOwnerClosed: Boolean): Boolean =
        currentGeneration == callbackGeneration && !callbackOwnerClosed
}
