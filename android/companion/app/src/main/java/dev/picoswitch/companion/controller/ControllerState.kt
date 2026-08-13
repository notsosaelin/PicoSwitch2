package dev.picoswitch.companion.controller

import kotlin.math.roundToInt

enum class ControllerButton {
    A, B, X, Y, L1, R1, L2, R2, Select, Start, LeftStick, RightStick, Home, Capture,
}

data class ControllerState(
    val leftX: Int = 128,
    val leftY: Int = 128,
    val rightX: Int = 128,
    val rightY: Int = 128,
    val leftTrigger: Int = 0,
    val rightTrigger: Int = 0,
    val buttons: Set<ControllerButton> = emptySet(),
    val dpadUp: Boolean = false,
    val dpadRight: Boolean = false,
    val dpadDown: Boolean = false,
    val dpadLeft: Boolean = false,
) {
    companion object { val Neutral = ControllerState() }
}

object ControllerReportEncoder {
    const val REPORT_ID = 1
    const val PAYLOAD_SIZE = 9

    fun encode(state: ControllerState): ByteArray {
        var bits = 0
        state.buttons.forEach { bits = bits or (1 shl it.ordinal) }
        return byteArrayOf(
            state.leftX.u8(), state.leftY.u8(), state.rightX.u8(), state.rightY.u8(),
            state.leftTrigger.u8(), state.rightTrigger.u8(),
            (bits and 0xFF).toByte(), ((bits ushr 8) and 0x3F).toByte(), hat(state).toByte(),
        )
    }

    fun hat(state: ControllerState): Int {
        val vertical = (if (state.dpadDown) 1 else 0) - (if (state.dpadUp) 1 else 0)
        val horizontal = (if (state.dpadRight) 1 else 0) - (if (state.dpadLeft) 1 else 0)
        return when (horizontal to vertical) {
            0 to -1 -> 0; 1 to -1 -> 1; 1 to 0 -> 2; 1 to 1 -> 3
            0 to 1 -> 4; -1 to 1 -> 5; -1 to 0 -> 6; -1 to -1 -> 7
            else -> 8
        }
    }

    private fun Int.u8() = coerceIn(0, 255).toByte()
}

data class AxisRange(val minimum: Float, val maximum: Float, val flat: Float = 0f) {
    fun stick(value: Float, invert: Boolean = false): Int {
        val center = (minimum + maximum) / 2f
        val radius = ((maximum - minimum) / 2f).takeIf { it > 0f } ?: 1f
        var normalized = ((value - center) / radius).coerceIn(-1f, 1f)
        val deadZone = maxOf(flat / radius, 0.04f)
        normalized = if (kotlin.math.abs(normalized) <= deadZone) 0f else {
            val magnitude = ((kotlin.math.abs(normalized) - deadZone) / (1f - deadZone)).coerceIn(0f, 1f)
            kotlin.math.sign(normalized) * magnitude
        }
        if (invert) normalized = -normalized
        return (128f + normalized * 127f).roundToInt().coerceIn(0, 255)
    }

    fun trigger(value: Float): Int {
        val span = (maximum - minimum).takeIf { it > 0f } ?: 1f
        return (((value - minimum) / span).coerceIn(0f, 1f) * 255f).roundToInt()
    }
}

object AndroidControllerDescriptor {
    // Byte-identical to tools/fixtures/android_controller_hid.h.
    val bytes = byteArrayOf(
        0x05,0x01, 0x09,0x05, 0xA1.toByte(),0x01, 0x85.toByte(),0x01,
        0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35, 0x09,0x33, 0x09,0x34,
        0x15,0x00, 0x26,0xFF.toByte(),0x00, 0x75,0x08, 0x95.toByte(),0x06, 0x81.toByte(),0x02,
        0x05,0x09, 0x19,0x01, 0x29,0x0E, 0x15,0x00, 0x25,0x01, 0x75,0x01,
        0x95.toByte(),0x0E, 0x81.toByte(),0x02, 0x75,0x01, 0x95.toByte(),0x02, 0x81.toByte(),0x03,
        0x05,0x01, 0x09,0x39, 0x15,0x00, 0x25,0x07, 0x35,0x00, 0x46,0x3B,0x01,
        0x65,0x14, 0x75,0x04, 0x95.toByte(),0x01, 0x81.toByte(),0x42, 0x75,0x04,
        0x95.toByte(),0x01, 0x81.toByte(),0x03, 0xC0.toByte(),
    )
}
