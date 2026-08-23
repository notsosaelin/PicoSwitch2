package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.DpadState
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * The one conversion between the touch engine's geometry domain and bridge wire
 * units.
 *
 * The engine works in `[-1,+1]` because that is the domain the standard gamepad
 * model and every piece of stick math are written in; the bridge works in
 * `0..255` because that is the wire. Converting in exactly one place is what
 * stops a renderer, a control and a settings screen from each rounding slightly
 * differently.
 *
 * The endpoint asymmetry is deliberate and is NOT a rounding bug: `128 + n*127`
 * puts full deflection at `1` and `255`, which is what `AxisRange.stick` — the
 * physical path — has always produced. A touch stick that reached `0` while the
 * physical stick reached `1` would be a second contract for the same axis, so
 * the shared formula wins over the tidier-looking one. `TouchAxisTest` pins the
 * two against each other.
 */
object TouchAxis {
    const val NEUTRAL = 128

    /**
     * `[-1,+1]` -> `0..255`, negative left/up.
     *
     * Non-finite input resolves to neutral rather than propagating: a NaN here
     * would become a garbage axis byte on the wire, and the only honest value
     * for "we do not know where the stick is" is centred.
     */
    fun toBridge(value: Float): Int {
        if (!value.isFinite()) return NEUTRAL
        return (NEUTRAL + value.coerceIn(-1f, 1f) * 127f).roundToInt().coerceIn(0, 255)
    }

    /** `[0,1]` -> `0..255`, rest at `0`. */
    fun triggerToBridge(value: Float): Int {
        if (!value.isFinite()) return 0
        return (value.coerceIn(0f, 1f) * 255f).roundToInt().coerceIn(0, 255)
    }
}

/** A stick position in the portable domain: `[-1,+1]`, negative left and up. */
data class TouchVector(val x: Float, val y: Float) {
    companion object { val Zero = TouchVector(0f, 0f) }
}

/**
 * Analog stick geometry, shared by every stick and every future host client.
 *
 * Two rules matter and both are easy to get subtly wrong:
 *
 * 1. CIRCULAR clamping. Clamping X and Y independently produces a square gate,
 *    so a diagonal reaches magnitude `sqrt(2)` before anything notices and the
 *    stick feels faster on the diagonals than on the cardinals. The vector's
 *    magnitude is what gets clamped; direction is preserved exactly.
 *
 * 2. RADIAL deadzone with RESCALING. Simply discarding an inner radius also
 *    discards that much of the usable range, so full deflection becomes
 *    unreachable at the top. The magnitude above the threshold is remapped onto
 *    the whole `0..1` range instead.
 */
object TouchStick {
    /**
     * @param dx displacement from the stick centre, in the layout's coordinate space
     * @param dy displacement from the stick centre, positive DOWN (screen convention)
     * @param radius travel radius in the same space; the distance meaning full deflection
     * @param deadzone inner fraction of [radius] that publishes centre, `0..1`
     */
    fun resolve(dx: Float, dy: Float, radius: Float, deadzone: Float): TouchVector {
        if (!dx.isFinite() || !dy.isFinite() || !radius.isFinite() || radius <= 0f) {
            return TouchVector.Zero
        }
        val nx = dx / radius
        val ny = dy / radius
        val magnitude = sqrt(nx * nx + ny * ny)
        if (magnitude <= 0f) return TouchVector.Zero

        val gate = deadzone.coerceIn(0f, 0.9f)
        if (magnitude <= gate) return TouchVector.Zero

        // Rescale so the first movement past the gate is small and the rim is
        // still exactly full scale.
        val scaled = ((magnitude - gate) / (1f - gate)).coerceIn(0f, 1f)
        val unitX = nx / magnitude
        val unitY = ny / magnitude
        return TouchVector(unitX * scaled, unitY * scaled)
    }
}

/**
 * D-pad geometry: one 2D control with eight sectors, not four unrelated buttons.
 *
 * A thumb slides around the ring rather than jumping between discrete keys, so
 * the useful model is an angle plus two thresholds:
 *
 * - a RADIAL pair, [TouchControlConfig.dpadEnterFraction] to engage and a lower
 *   [TouchControlConfig.dpadExitFraction] to disengage, so resting a thumb near
 *   the centre does not flicker on and off;
 * - an ANGULAR margin, [TouchControlConfig.dpadHysteresisDegrees], so a thumb
 *   parked on a sector boundary does not alternate Up / UpRight on sub-pixel
 *   noise.
 *
 * Opposite directions are structurally impossible from one contact: a single
 * angle selects a single sector, and only diagonals set two flags. Nothing here
 * needs a cancellation rule, and the wire hat code stays where it belongs — in
 * the protocol encoder.
 */
object TouchDpad {
    /** Sector index -> the two flags it raises. Index 0 is East, counter-clockwise. */
    private val SECTORS = arrayOf(
        DpadState(right = true),
        DpadState(up = true, right = true),
        DpadState(up = true),
        DpadState(up = true, left = true),
        DpadState(left = true),
        DpadState(down = true, left = true),
        DpadState(down = true),
        DpadState(down = true, right = true),
    )

    private const val SECTOR_DEGREES = 45f
    private const val HALF_SECTOR = SECTOR_DEGREES / 2f

    /**
     * @param dy positive DOWN, as the platform reports it; converted internally
     *   so that 90 degrees means Up.
     * @param previous the direction this control published last, for hysteresis.
     */
    fun resolve(
        dx: Float,
        dy: Float,
        radius: Float,
        config: TouchControlConfig,
        previous: DpadState,
    ): DpadState {
        if (!dx.isFinite() || !dy.isFinite() || !radius.isFinite() || radius <= 0f) {
            return DpadState.None
        }
        val magnitude = sqrt(dx * dx + dy * dy) / radius
        val engaged = previous != DpadState.None
        val threshold = if (engaged) config.dpadExitFraction else config.dpadEnterFraction
        if (magnitude < threshold) return DpadState.None

        // atan2 on -dy so the maths frame is the conventional one (90 = up) even
        // though the input frame has y growing downward.
        var degrees = Math.toDegrees(atan2(-dy.toDouble(), dx.toDouble())).toFloat()
        if (degrees < 0f) degrees += 360f

        val candidate = (((degrees + HALF_SECTOR) / SECTOR_DEGREES).toInt()) % SECTORS.size
        val held = SECTORS.indexOf(previous)
        if (held >= 0) {
            val offset = angularDistance(degrees, held * SECTOR_DEGREES)
            if (offset <= HALF_SECTOR + config.dpadHysteresisDegrees) return SECTORS[held]
        }
        return SECTORS[candidate]
    }

    private fun angularDistance(a: Float, b: Float): Float {
        val raw = abs(a - b) % 360f
        return if (raw > 180f) 360f - raw else raw
    }
}
