package dev.picoswitch.companion.controller

import kotlin.math.roundToInt

enum class ControllerButton {
    A, B, X, Y, L1, R1, L2, R2, Select, Start, LeftStick, RightStick, Home, Capture,
}

/**
 * Motion sample in the adapter's wire units, already converted from Android's SI
 * sensor values. See [MotionScale] and tools/fixtures/android_controller_hid.h.
 *
 * [valid] is what tells the adapter whether motion is live; when the sensors are
 * idled (the console is not consuming motion) the flag clears and the firmware
 * stops publishing motion rather than latching the last sample.
 */
data class ControllerMotion(
    val gyroX: Int = 0,
    val gyroY: Int = 0,
    val gyroZ: Int = 0,
    val accelX: Int = 0,
    val accelY: Int = 0,
    val accelZ: Int = 0,
    /** Free-running millisecond stamp; wraps at 16 bits on the wire. */
    val timestampMs: Int = 0,
    val valid: Boolean = false,
) {
    companion object { val None = ControllerMotion() }
}

/** Handheld battery, forwarded so the console shows a real controller battery. */
data class ControllerBattery(
    val levelPercent: Int = 0,
    val charging: Boolean = false,
    val valid: Boolean = false,
) {
    companion object { val Unknown = ControllerBattery() }
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
    val motion: ControllerMotion = ControllerMotion.None,
    val battery: ControllerBattery = ControllerBattery.Unknown,
) {
    companion object { val Neutral = ControllerState() }
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

/**
 * Android SI sensor values -> adapter wire counts.
 *
 * The adapter expects the same units it already receives from a DualSense, so the
 * handheld reuses the hardware-validated motion translation instead of adding a
 * second scaling convention: 8192 counts/g for acceleration and 16.384 counts/dps
 * for angular rate. These constants are the exact mirror of the C contract.
 */
object MotionScale {
    const val ACCEL_COUNTS_PER_G = 8192.0
    const val GYRO_COUNTS_PER_DPS = 16.384
    const val GRAVITY_MS2 = 9.80665
    const val DEGREES_PER_RADIAN = 57.2957795

    /** TYPE_ACCELEROMETER is m/s^2 and gravity-inclusive, which is what we want. */
    fun accelCounts(metersPerSecondSquared: Float): Int =
        clamp16(metersPerSecondSquared * (ACCEL_COUNTS_PER_G / GRAVITY_MS2))

    /** TYPE_GYROSCOPE is rad/s. */
    fun gyroCounts(radiansPerSecond: Float): Int =
        clamp16(radiansPerSecond * DEGREES_PER_RADIAN * GYRO_COUNTS_PER_DPS)

    fun clamp16(value: Double): Int = value.roundToInt().coerceIn(-32768, 32767)
}

object ControllerReportEncoder {
    const val REPORT_ID = 1
    const val OUTPUT_REPORT_ID = 2

    /** v1 payload length, retained so the compatibility test can pin it. */
    const val PAYLOAD_SIZE = 9

    /** v2 payload: v1 fields plus motion, battery, flags and timestamp. */
    const val PAYLOAD_SIZE_V2 = 25

    // Wire offsets WITHIN THE PAYLOAD (the report ID is not part of sendReport's
    // data argument, so these are the C contract offsets minus one).
    private const val OFF_GYRO = 9
    private const val OFF_ACCEL = 15
    private const val OFF_BATTERY = 21
    private const val OFF_FLAGS = 22
    private const val OFF_TIMESTAMP = 23

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
            putLe16(out, OFF_TIMESTAMP, state.motion.timestampMs and 0xFFFF)
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

    /** The original nine-byte report, kept for the v1 compatibility test. */
    fun encodeV1(state: ControllerState): ByteArray =
        ByteArray(PAYLOAD_SIZE).also { encodeCore(state, it) }

    private fun encodeCore(state: ControllerState, out: ByteArray) {
        var bits = 0
        state.buttons.forEach { bits = bits or (1 shl it.ordinal) }
        out[0] = state.leftX.u8(); out[1] = state.leftY.u8()
        out[2] = state.rightX.u8(); out[3] = state.rightY.u8()
        out[4] = state.leftTrigger.u8(); out[5] = state.rightTrigger.u8()
        out[6] = (bits and 0xFF).toByte()
        out[7] = ((bits ushr 8) and 0x3F).toByte()
        out[8] = hat(state).toByte()
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

    private fun putLe16(out: ByteArray, offset: Int, value: Int) {
        out[offset] = (value and 0xFF).toByte()
        out[offset + 1] = ((value shr 8) and 0xFF).toByte()
    }

    private fun Int.u8() = coerceIn(0, 255).toByte()
}

/**
 * Feedback the adapter sends back: rumble, the console's player number, and
 * whether motion is actually being consumed. Pure so it is unit-testable.
 */
data class ControllerFeedback(
    val rumbleLeft: Int = 0,
    val rumbleRight: Int = 0,
    val playerLed: Int = 0,
    val motionWanted: Boolean = false,
) {
    /** Phones have a single actuator; drive it from the stronger motor. */
    val rumbleAmplitude: Int get() = maxOf(rumbleLeft, rumbleRight)

    companion object {
        val None = ControllerFeedback()
        const val FLAG_MOTION_WANTED = 0x01

        /**
         * Decode an output report payload. Accepts the payload with or without a
         * leading report-ID byte, because Android delivers control-channel
         * SET_REPORT and interrupt-channel data with different framing depending
         * on the OEM stack. Returns null when the payload cannot be a feedback
         * report, so a stray report can never be applied as rumble.
         */
        fun decode(data: ByteArray?, reportId: Int? = null): ControllerFeedback? {
            if (data == null) return null
            var body = data
            if (reportId != null && reportId != ControllerReportEncoder.OUTPUT_REPORT_ID) return null
            // Tolerate an embedded report ID: some stacks include it in `data`.
            if (body.size == 5 && body[0].toInt() and 0xFF == ControllerReportEncoder.OUTPUT_REPORT_ID) {
                body = body.copyOfRange(1, body.size)
            }
            if (body.size < 4) return null
            return ControllerFeedback(
                rumbleLeft = body[0].toInt() and 0xFF,
                rumbleRight = body[1].toInt() and 0xFF,
                playerLed = body[2].toInt() and 0xFF,
                motionWanted = (body[3].toInt() and FLAG_MOTION_WANTED) != 0,
            )
        }
    }
}

object AndroidControllerDescriptor {
    // Byte-identical to ANDROID_CONTROLLER_V2_HID_DESCRIPTOR in
    // tools/fixtures/android_controller_hid.h. The firmware identifies this
    // bridge by an EXACT match on these bytes, so any edit here must be made in
    // both places -- AndroidControllerDescriptorTest pins the length and content
    // hash to make an accidental one-sided change fail loudly.
    val bytes = byteArrayOf(
        0x05, 0x01, 0x09, 0x05, 0xA1.toByte(), 0x01, 0x85.toByte(), 0x01,
        // v1 axes
        0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,
        0x15, 0x00, 0x26, 0xFF.toByte(), 0x00, 0x75, 0x08, 0x95.toByte(), 0x06, 0x81.toByte(), 0x02,
        // v1 buttons
        0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
        0x95.toByte(), 0x0E, 0x81.toByte(), 0x02, 0x75, 0x01, 0x95.toByte(), 0x02, 0x81.toByte(), 0x03,
        // v1 hat
        0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01,
        0x65, 0x14, 0x75, 0x04, 0x95.toByte(), 0x01, 0x81.toByte(), 0x42, 0x75, 0x04,
        0x95.toByte(), 0x01, 0x81.toByte(), 0x03,
        // v2 vendor extension: motion, battery, flags, timestamp
        0x06, 0x00, 0xFF.toByte(), 0x65, 0x00,
        0x09, 0x20, 0x09, 0x21, 0x09, 0x22, 0x09, 0x23, 0x09, 0x24, 0x09, 0x25,
        0x16, 0x00, 0x80.toByte(), 0x26, 0xFF.toByte(), 0x7F, 0x75, 0x10, 0x95.toByte(), 0x06,
        0x81.toByte(), 0x02,
        0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF.toByte(), 0x00, 0x75, 0x08,
        0x95.toByte(), 0x02, 0x81.toByte(), 0x02,
        0x09, 0x32, 0x15, 0x00, 0x27, 0xFF.toByte(), 0xFF.toByte(), 0x00, 0x00,
        0x75, 0x10, 0x95.toByte(), 0x01, 0x81.toByte(), 0x02,
        // v2 output report: rumble + player LED + flags
        0x85.toByte(), 0x02, 0x09, 0x40, 0x09, 0x41, 0x09, 0x42, 0x09, 0x43,
        0x15, 0x00, 0x26, 0xFF.toByte(), 0x00, 0x75, 0x08, 0x95.toByte(), 0x04, 0x91.toByte(), 0x02,
        0xC0.toByte(),
    )
}
