package dev.picoswitch.management

/** BLE carrier constants and fragmentation, separate from logical command semantics. */
object BleManagementContract {
    const val DEFAULT_ATT_MTU = 23
    const val SERVICE_UUID = "7c5ad4ed-2731-417c-b316-058505c7c083"
    const val RX_UUID = "5252186a-817f-489f-ad75-94c3bd444769"
    const val TX_UUID = "81462706-8e64-407a-bc3d-d303529fbe1c"
    const val ATT_PAYLOAD_WITH_DEFAULT_MTU = 20
    const val MAX_REPLY_PAYLOAD_BYTES = 511

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
