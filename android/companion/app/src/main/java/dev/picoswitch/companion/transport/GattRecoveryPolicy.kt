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

/**
 * Android reuses the same integer callback field for disjoint HCI and GATT
 * namespaces. Keep the stage beside the value so diagnostics never turn an
 * HCI disconnect reason into an ATT error (or vice versa).
 */
object GattStatusFormatter {
    fun describe(stage: GattFailureStage, status: Int): String {
        val namespace = if (stage == GattFailureStage.Connect) "HCI" else "GATT"
        val symbolic = when (stage) {
            GattFailureStage.Connect -> when (status) {
                0x00 -> "SUCCESS"
                0x08 -> "CONNECTION_TIMEOUT"
                0x13 -> "PEER_USER_TERMINATION"
                0x16 -> "LOCAL_HOST_TERMINATION"
                0x3e -> "FAILED_TO_ESTABLISH"
                0x85 -> "ANDROID_GATT_ERROR"
                0x8f -> "ANDROID_CONNECTION_CONGESTED"
                0x93 -> "ANDROID_CONNECTION_TIMEOUT"
                else -> "UNKNOWN"
            }
            else -> when (status) {
                0x00 -> "SUCCESS"
                0x05 -> "INSUFFICIENT_AUTHENTICATION"
                0x08 -> "INSUFFICIENT_AUTHORIZATION"
                0x0d -> "INVALID_ATTRIBUTE_LENGTH"
                0x13 -> "VALUE_NOT_ALLOWED"
                0x85 -> "ANDROID_GATT_ERROR"
                0x8f -> "ANDROID_GATT_CONGESTED"
                else -> "UNKNOWN"
            }
        }
        return "$namespace status=0x${status.toString(16).uppercase().padStart(2, '0')} $symbolic"
    }
}

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
