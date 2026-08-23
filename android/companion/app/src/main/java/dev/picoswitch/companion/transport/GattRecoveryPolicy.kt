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
                // The adapter erases its Bluetooth bonds on every firmware
                // install by design (see config.c: the one-shot install marker),
                // so these two are a routine, expected lifecycle event here --
                // not an exotic error. Naming them is what turned an
                // unactionable "HCI status=0x05 UNKNOWN" into a diagnosis.
                0x05 -> "AUTHENTICATION_FAILURE"
                0x06 -> "PIN_OR_KEY_MISSING"
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

/**
 * Did the adapter forget us?
 *
 * The adapter wipes its Bluetooth bonds on every firmware install, deliberately,
 * so a stale controller cannot reconnect after a flash and make a test result
 * meaningless. That is a lifecycle event, not a fault -- but it leaves a
 * three-party relationship with only one party cleared: Android keeps its LTK
 * and the app keeps its saved relationship.
 *
 * What Android then reports is unambiguous. Confirmed on hardware 2026-08-23:
 * the tablet's stack logged `btm_sec_encrypt_change: encrypt failure status 0x6`
 * (PIN_OR_KEY_MISSING) and surfaced connect-stage status 0x05 to the app, on a
 * device it still reported as BOND_BONDED. Six attempts over fourteen minutes
 * all failed identically before Android gave up and dropped its own bond.
 *
 * Both halves are load-bearing:
 *
 *   * the STATUS says the peer rejected or lacked our key. Generic stack
 *     failures (133), timeouts and congestion say nothing of the sort and must
 *     keep their ordinary retry behaviour;
 *   * the BOND says Android still believes in a pairing. Without a bond this is
 *     simply "not paired", which is a different message and a different flow.
 *
 * Only the CONNECT stage qualifies. GATT status 0x05 at the ATT layer is
 * "insufficient authentication" for one attribute -- an entirely different
 * situation from a link that cannot be encrypted at all.
 */
object AdapterResetSignature {
    /** HCI 0x05 Authentication Failure. */
    const val AUTHENTICATION_FAILURE = 0x05
    /** HCI 0x06 PIN or Key Missing -- the peer has no key for us. */
    const val PIN_OR_KEY_MISSING = 0x06

    fun isBondMismatch(stage: GattFailureStage, status: Int?, androidStillBonded: Boolean): Boolean =
        androidStillBonded &&
            stage == GattFailureStage.Connect &&
            (status == AUTHENTICATION_FAILURE || status == PIN_OR_KEY_MISSING)

    /** As above, for a thrown transport failure. */
    fun isBondMismatch(error: Throwable, androidStillBonded: Boolean): Boolean {
        val failure = generateSequence(error) { it.cause }
            .filterIsInstance<GattTransportException>()
            .firstOrNull() ?: return false
        return isBondMismatch(failure.stage, failure.status, androidStillBonded)
    }

    /**
     * What the user has to do, and why. The app cannot clear the Android bond
     * itself: BluetoothDevice.removeBond() is a @SystemApi gated on
     * BLUETOOTH_PRIVILEGED (signature|privileged), and reflection to it is
     * blocked at this API level. Saying so plainly beats retrying forever.
     */
    const val REPAIR_MESSAGE: String =
        "The adapter was reset and no longer recognises this pairing. " +
            "Forget PicoSwitch2 in Android Bluetooth settings, then pair again."
}

/** Pure authority check used before any Android callback may mutate session state. */
object GattCallbackAuthority {
    fun isAuthoritative(currentGeneration: Long?, callbackGeneration: Long, callbackOwnerClosed: Boolean): Boolean =
        currentGeneration == callbackGeneration && !callbackOwnerClosed
}
