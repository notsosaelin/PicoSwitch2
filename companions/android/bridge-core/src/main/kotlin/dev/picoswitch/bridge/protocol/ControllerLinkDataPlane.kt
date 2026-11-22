package dev.picoswitch.bridge.protocol

/**
 * Controller Link (Path C) data-plane framing: the Kotlin half of the contract
 * `firmware/src/management/ns2_companion_link.h` defines, byte for byte the
 * same as the Windows `ControllerLinkDataPlane`.
 *
 * Gameplay rides a dedicated binary characteristic pair on the trusted
 * management link, never the newline-JSON command channel: one Bluetooth link
 * carries management, Controller Link and companion audio. Android moved here
 * from its separate Classic HID connection on 2026-11-22
 * (docs/experiments/android-two-link-coexistence-2026-11-22.md).
 */
object ControllerLinkDataPlane {
    /** Must equal NS2_COMPANION_LINK_VERSION; a mismatch is refused at start. */
    const val VERSION: Int = 1
    const val OPCODE_STATE: Int = 0x01   // companion -> adapter
    const val OPCODE_OUTPUT: Int = 0x81  // adapter -> companion

    /** `[version][opcode][sequence lo][sequence hi]` */
    const val HEADER_BYTES = 4

    /** The canonical v2 payload, report ID excluded: exactly what [ControllerReportEncoder.encode] produces. */
    const val PAYLOAD_BYTES = ControllerReportEncoder.PAYLOAD_SIZE_V2
    const val FRAME_BYTES = HEADER_BYTES + PAYLOAD_BYTES

    /** One frame must fit one ATT Write Command: value plus 3 bytes of overhead. */
    const val MINIMUM_ATT_MTU = FRAME_BYTES + 3

    /** `[version][opcode][report id]` then the feedback body. */
    const val OUTPUT_HEADER_BYTES = 3
    const val OUTPUT_MAX_PAYLOAD = BridgeOutputCodec.BODY_SIZE

    /**
     * Frame one encoded report. The sequence is defensive: a stale or
     * duplicated frame can never overwrite newer state on the adapter.
     */
    fun encodeInput(payload: ByteArray, sequence: Int): ByteArray {
        require(payload.size == PAYLOAD_BYTES) {
            "Controller Link payload must be $PAYLOAD_BYTES bytes, got ${payload.size}"
        }
        val frame = ByteArray(FRAME_BYTES)
        frame[0] = VERSION.toByte()
        frame[1] = OPCODE_STATE.toByte()
        frame[2] = (sequence and 0xFF).toByte()
        frame[3] = ((sequence ushr 8) and 0xFF).toByte()
        payload.copyInto(frame, HEADER_BYTES)
        return frame
    }

    /**
     * One adapter-to-companion feedback frame, or null when it cannot be one.
     * Null rather than an exception, so a stray notification is ignored instead
     * of tearing down a healthy link.
     */
    fun decodeOutput(frame: ByteArray?): OutputFrame? {
        if (frame == null || frame.size < OUTPUT_HEADER_BYTES) return null
        if ((frame[0].toInt() and 0xFF) != VERSION) return null
        if ((frame[1].toInt() and 0xFF) != OPCODE_OUTPUT) return null
        val body = frame.copyOfRange(OUTPUT_HEADER_BYTES, frame.size)
        if (body.size > OUTPUT_MAX_PAYLOAD) return null
        return OutputFrame(frame[2].toInt() and 0xFF, body)
    }

    /** Does the negotiated ATT MTU carry one whole gameplay frame? */
    fun mtuSufficient(attMtu: Int): Boolean = attMtu >= MINIMUM_ATT_MTU

    /** One decoded feedback frame: the report id and its raw body. */
    class OutputFrame(val reportId: Int, val payload: ByteArray)
}
