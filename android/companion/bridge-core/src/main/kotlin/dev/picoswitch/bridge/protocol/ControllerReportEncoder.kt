package dev.picoswitch.bridge.protocol

import dev.picoswitch.bridge.core.ControllerState

/**
 * Normalized controller state -> PicoSwitch Bridge input report.
 *
 * The only place in the bridge that knows the wire layout. Platform backends
 * never build report bytes; they produce a [ControllerState] and the session
 * encodes it, so a new platform cannot introduce a second, subtly different
 * encoding of the same contract.
 *
 * Full field documentation: `docs/bridge/PROTOCOL.md` and the C-side source of
 * truth `tools/fixtures/android_controller_hid.h`.
 */
object ControllerReportEncoder {
    const val REPORT_ID = 1
    const val OUTPUT_REPORT_ID = 2

    /** v1 payload length, retained so the compatibility test can pin it. */
    const val PAYLOAD_SIZE = 9

    /**
     * v2 payload: v1 fields plus motion, battery, flags and timestamp.
     *
     * One byte longer since bridge contract 4, which grew the button field from
     * two bytes to three so it could carry GL/GR.
     */
    const val PAYLOAD_SIZE_V2 = 26

    // Wire offsets WITHIN THE PAYLOAD. The report ID is not part of the payload a
    // transport sends, so these are the C contract offsets minus one.
    private const val OFF_GYRO = 10
    private const val OFF_ACCEL = 16
    private const val OFF_BATTERY = 22
    private const val OFF_FLAGS = 23
    private const val OFF_TIMESTAMP = 24

    const val FLAG_CHARGING = 0x01
    const val FLAG_MOTION_VALID = 0x02
    const val FLAG_BATTERY_VALID = 0x04

    /** Full v2 report. The first nine bytes are byte-identical to v1. */
    fun encode(state: ControllerState): ByteArray {
        val out = ByteArray(PAYLOAD_SIZE_V2)
        encodeCore(state, out)

        if (state.motion.valid) {
            putLe16(out, OFF_GYRO + 0, state.motion.gyroX)
            putLe16(out, OFF_GYRO + 2, state.motion.gyroY)
            putLe16(out, OFF_GYRO + 4, state.motion.gyroZ)
            putLe16(out, OFF_ACCEL + 0, state.motion.accelX)
            putLe16(out, OFF_ACCEL + 2, state.motion.accelY)
            putLe16(out, OFF_ACCEL + 4, state.motion.accelZ)
            putLe16(out, OFF_TIMESTAMP, state.motion.timestampTicks and 0xFFFF)
        }
        out[OFF_BATTERY] = state.battery.levelPercent.coerceIn(0, 100).toByte()

        var flags = 0
        if (state.motion.valid) flags = flags or FLAG_MOTION_VALID
        if (state.battery.valid) {
            flags = flags or FLAG_BATTERY_VALID
            if (state.battery.charging) flags = flags or FLAG_CHARGING
        }
        out[OFF_FLAGS] = flags.toByte()
        return out
    }

    /**
     * The original nine-byte report, kept for the v1 compatibility test.
     *
     * No longer a prefix of [encode]. Through contract 3 the first nine bytes of
     * both were identical, because every button still fitted in two bytes;
     * contract 4 needed a third for GL/GR, which moved the hat and everything
     * after it by one. A v1 peer is unaffected — it reads the v1 descriptor,
     * whose items still describe exactly these nine bytes — but the two layouts
     * are now genuinely different and are written separately rather than one
     * pretending to be a truncation of the other.
     */
    fun encodeV1(state: ControllerState): ByteArray {
        val out = ByteArray(PAYLOAD_SIZE)
        encodeAxes(state, out)
        val bits = buttonBits(state)
        out[6] = (bits and 0xFF).toByte()
        // 0x7F, not 0x3F: bit 14 is C / GameChat, the fifteenth button.
        out[7] = ((bits ushr 8) and 0x7F).toByte()
        out[8] = hat(state).toByte()
        return out
    }

    private fun encodeCore(state: ControllerState, out: ByteArray) {
        encodeAxes(state, out)
        val bits = buttonBits(state)
        out[6] = (bits and 0xFF).toByte()
        out[7] = ((bits ushr 8) and 0xFF).toByte()
        // Bit 16 is GR, the seventeenth and last button; the remaining seven bits
        // of this byte are the descriptor's padding and must stay clear.
        out[8] = ((bits ushr 16) and 0x01).toByte()
        out[9] = hat(state).toByte()
    }

    private fun encodeAxes(state: ControllerState, out: ByteArray) {
        out[0] = state.leftX.u8(); out[1] = state.leftY.u8()
        out[2] = state.rightX.u8(); out[3] = state.rightY.u8()
        out[4] = state.leftTrigger.u8(); out[5] = state.rightTrigger.u8()
    }

    /** `1 shl ordinal` per pressed button; see `ControllerButton`. */
    private fun buttonBits(state: ControllerState): Int {
        var bits = 0
        state.buttons.forEach { bits = bits or (1 shl it.ordinal) }
        return bits
    }

    /** The four retained D-pad directions collapsed to a HID hat code; 8 = neutral. */
    fun hat(state: ControllerState): Int {
        val vertical = (if (state.dpadDown) 1 else 0) - (if (state.dpadUp) 1 else 0)
        val horizontal = (if (state.dpadRight) 1 else 0) - (if (state.dpadLeft) 1 else 0)
        return when (horizontal to vertical) {
            0 to -1 -> 0; 1 to -1 -> 1; 1 to 0 -> 2; 1 to 1 -> 3
            0 to 1 -> 4; -1 to 1 -> 5; -1 to 0 -> 6; -1 to -1 -> 7
            else -> 8
        }
    }

    private fun putLe16(out: ByteArray, offset: Int, value: Int) {
        out[offset] = (value and 0xFF).toByte()
        out[offset + 1] = ((value shr 8) and 0xFF).toByte()
    }

    private fun Int.u8() = coerceIn(0, 255).toByte()
}
