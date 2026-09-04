package dev.picoswitch.management

/** BLE carrier constants and fragmentation, separate from logical command semantics. */
object BleManagementContract {
    const val DEFAULT_ATT_MTU = 23
    const val SERVICE_UUID = "7c5ad4ed-2731-417c-b316-058505c7c083"
    const val RX_UUID = "5252186a-817f-489f-ad75-94c3bd444769"
    const val TX_UUID = "81462706-8e64-407a-bc3d-d303529fbe1c"

    /**
     * The Controller Link data plane, on this SAME service.
     *
     * Declared here even though no Android code speaks it yet. The firmware added
     * these two characteristics to the management service for the Windows
     * companion, which means the service this app discovers is no longer the
     * service this file described -- and a contract that is silently a subset of
     * the peripheral's is exactly the kind of drift that turns into a field
     * report nobody can localise.
     *
     * Naming them also makes the conformance test able to check them: it compares
     * this object against `tools/fixtures/management/protocol-v1.json`, the
     * cross-platform record of what the adapter exposes, so a future firmware
     * change to either characteristic fails a JVM test on both companions rather
     * than only on the one that uses them.
     *
     * Companion to adapter: gameplay frames, write-without-response.
     * Firmware side is `config_ble_cl_in_uuid`.
     */
    const val CONTROLLER_LINK_IN_UUID = "2f9e54c1-0b7a-4d62-9c18-3e74a52166d0"

    /**
     * Adapter to companion: feedback notifications (rumble, player LED).
     * Firmware side is `config_ble_cl_out_uuid`.
     */
    const val CONTROLLER_LINK_OUT_UUID = "6b33e18a-c45d-470e-b291-7f0c88ae3914"
    const val ATT_PAYLOAD_WITH_DEFAULT_MTU = 20
    const val MAX_REPLY_PAYLOAD_BYTES = 511

    /** ATT write payload for a negotiated MTU: three bytes go to the header. */
    const val ATT_HEADER_BYTES = 3

    /**
     * How much of a command fits in one ATT write at [mtu].
     *
     * FRAGMENT SIZE IS NOT PART OF THE PROTOCOL. The adapter accumulates
     * received bytes into a line buffer and acts on the newline, so it cannot
     * observe where a command was split; only the total length matters, and that
     * is bounded well below its buffer. Sending 20 bytes at a time when the link
     * has agreed on far more simply multiplies the number of round trips, and
     * every round trip is another opportunity for a callback to arrive late.
     *
     * Never returns less than the default, so a failed or absent negotiation
     * behaves exactly as before.
     */
    fun attPayloadFor(mtu: Int): Int =
        (mtu - ATT_HEADER_BYTES).coerceAtLeast(ATT_PAYLOAD_WITH_DEFAULT_MTU)

    fun commandChunks(command: String, payloadBytes: Int): List<ByteArray> {
        require(payloadBytes > 0) { "Payload size must be positive" }
        return ManagementProtocol.frame(command)
            .toList()
            .chunked(payloadBytes)
            .map { it.toByteArray() }
    }
}

/** Incremental BLE notification assembly for one owned logical exchange. */
class BleReplyAssembler(
    private val maxPayloadBytes: Int = BleManagementContract.MAX_REPLY_PAYLOAD_BYTES,
) {
    private val payload = ArrayList<Byte>()

    init {
        require(maxPayloadBytes > 0) { "Reply limit must be positive" }
    }

    /** Returns the complete reply at LF, or null while more notification bytes are required. */
    fun accept(fragment: ByteArray): String? {
        for (byte in fragment) {
            if (byte.toInt() == '\n'.code) {
                if (payload.lastOrNull()?.toInt() == '\r'.code) payload.removeAt(payload.lastIndex)
                return payload.toByteArray().decodeToString()
            }
            payload += byte
            if (payload.size > maxPayloadBytes) {
                throw ManagementReplyTooLargeException(
                    "Adapter reply exceeded the $maxPayloadBytes-byte wireless payload limit",
                )
            }
        }
        return null
    }
}
