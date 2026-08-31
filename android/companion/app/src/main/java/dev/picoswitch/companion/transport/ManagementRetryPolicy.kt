package dev.picoswitch.companion.transport

/**
 * Which management commands may safely be sent again after no reply.
 *
 * THE FIRMWARE CAN DROP A COMMAND WITHOUT ANSWERING IT, by design. Its wireless
 * bridge holds one command and one response at a time, and a complete command
 * line arriving while either slot is occupied is discarded outright —
 * `config_wireless_bridge_receive()` resets its line buffer and returns BUSY.
 * Nothing is sent back, so the client learns of it only by timing out.
 *
 * Before this, that cost ten seconds and the whole session: the timeout path
 * invalidates the connection, so a single dropped chunk ended an upload and
 * disconnected the adapter. A command that can simply be repeated should be
 * repeated instead.
 *
 * REPEATABLE MEANS THE SECOND SEND CANNOT CHANGE THE OUTCOME OF THE FIRST, even
 * if the first was actually received and only its reply went missing. That is a
 * stronger requirement than "read-only", and it is why this is an allowlist
 * rather than a denylist: a command whose safety has not been reasoned about is
 * treated as unsafe.
 *
 * - `amiibo chunk <offset> <hex>` writes at an ABSOLUTE offset into the staging
 *   buffer, so sending it twice stores the same bytes in the same place.
 * - `amiibo read` and the status queries have no effect at all.
 *
 * Deliberately excluded, though they look harmless:
 *
 * - `amiibo begin` resets the staging buffer. Repeating one that was received
 *   would discard the chunks already sent and the upload would continue from the
 *   wrong place, producing a corrupt tag that still passed its own CRC.
 * - `amiibo commit` and `amiibo persist` publish and flash. Their effects are
 *   not defined to be repeatable and they are sent once per transfer, so there
 *   is nothing to gain by risking it.
 */
object ManagementRetryPolicy {

    /** Extra attempts after the first, for a repeatable command. */
    const val MAX_RETRIES = 2

    private val repeatablePrefixes = listOf(
        "amiibo chunk ",
        "amiibo read ",
        "amiibo status",
    )

    fun isRepeatable(command: String): Boolean {
        val normalized = command.trim().lowercase()
        return repeatablePrefixes.any { normalized.startsWith(it) }
    }
}
