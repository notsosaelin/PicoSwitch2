package dev.picoswitch.bridge.core

import kotlin.math.roundToInt

/**
 * The PicoSwitch Bridge's logical controller buttons.
 *
 * These are BRIDGE semantics, not any host platform's. A platform backend
 * translates its own identifiers (Android `KEYCODE_BUTTON_A`, a Windows XInput
 * bit, a Linux `BTN_SOUTH`) into these before anything shared sees them.
 *
 * Ordinal IS the wire bit: [dev.picoswitch.bridge.protocol.ControllerReportEncoder]
 * writes `1 shl ordinal` into the button field, and the firmware's generic
 * sequential profile reads usage `ordinal + 1`. Append only, and never reorder.
 *
 * [C] is the Switch 2 GameChat button (usage 15 -> JP_BUTTON_A3 -> NS2_DST_C ->
 * SWITCH_EXTRA_C). Most host devices have no physical key for it, which is
 * exactly why backends are expected to offer it as a virtual button alongside
 * Home and Capture.
 *
 * [GL] and [GR] are the Pro Controller 2 GRIP buttons (usages 16/17 ->
 * JP_BUTTON_A4/A5 -> NS2_DST_GL/GR -> SWITCH_EXTRA_GL/GR). Those destinations
 * long predate this enum; what was missing was any way for the bridge to reach
 * them, which is what bridge contract 4 added. Like [C], almost no host device
 * has a physical key for them, so they arrive from the on-screen controller or
 * from a virtual press.
 */
enum class ControllerButton {
    A, B, X, Y, L1, R1, L2, R2, Select, Start, LeftStick, RightStick, Home, Capture, C,
    GL, GR,
}

/**
 * One motion sample in the bridge's canonical convention.
 *
 * Units, axes and orientation are defined once, in [MotionConvention]. A platform
 * backend converts its own sensor frame and units into that convention before
 * constructing this; nothing downstream re-interprets it.
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
    /** Free-running 100 us stamp; wraps at 16 bits on the wire. See [MotionConvention]. */
    val timestampTicks: Int = 0,
    val valid: Boolean = false,
) {
    companion object { val None = ControllerMotion() }
}

/** Host battery, forwarded so the console shows a real controller battery. */
data class ControllerBattery(
    val levelPercent: Int = 0,
    val charging: Boolean = false,
    val valid: Boolean = false,
) {
    companion object { val Unknown = ControllerBattery() }
}

/**
 * The complete normalized controller state — the single value that crosses the
 * platform boundary in the input direction.
 *
 * Sticks are `0..255` with `128` neutral; triggers are `0..255` with `0` at rest.
 * Those are the bridge's units, chosen to match the wire contract exactly so the
 * encoder is a memcpy rather than a second place where scaling can be wrong.
 * Backends normalize into them; see [AxisRange].
 */
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

/**
 * A host axis's reported range, and the conversion into bridge units.
 *
 * Every platform reports analog axes with its own minimum/maximum/rest values,
 * and most report a manufacturer dead zone alongside. Handing those three numbers
 * to shared code keeps the normalization identical across platforms instead of
 * each backend inventing its own curve.
 */
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
 * Digital D-pad state, however the host produced it.
 *
 * Four retained directions rather than a hat code: opposite directions cancel,
 * and releasing one side restores the still-held side without inventing an edge.
 * The hat encoding is a wire detail and lives in the protocol layer.
 */
data class DpadState(
    val up: Boolean = false,
    val right: Boolean = false,
    val down: Boolean = false,
    val left: Boolean = false,
) {
    companion object {
        val None = DpadState()

        /** Threshold for an analog hat axis pair; `-1..1`, positive right/down. */
        fun fromAxes(x: Float, y: Float): DpadState = DpadState(
            up = y < -0.5f, right = x > 0.5f, down = y > 0.5f, left = x < -0.5f,
        )
    }
}

/**
 * The analog half of one host input event, applied as a unit.
 *
 * Grouped on purpose: a platform delivers sticks, triggers and hat together, and
 * publishing them as one state change keeps a single physical event from becoming
 * several observable snapshots.
 *
 * [dpad] is null when the source has no hat axes at all, which means "leave the
 * hat contribution as it is" rather than "the hat is centered".
 */
data class AnalogFrame(
    val leftX: Int,
    val leftY: Int,
    val rightX: Int,
    val rightY: Int,
    val leftTrigger: Int,
    val rightTrigger: Int,
    val dpad: DpadState? = null,
)
