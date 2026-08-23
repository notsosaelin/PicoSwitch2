package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition
import kotlin.math.abs
import kotlin.math.min

/** One thing wrong with a layout. [blocking] findings make a layout unplayable. */
data class TouchLayoutFinding(val message: String, val blocking: Boolean)

/**
 * Mechanical layout validation.
 *
 * A declarative layout can be checked instead of merely looked at, and it should
 * be: the failures that matter here — a hidden hit region overlapping its
 * neighbour, a target below the size a thumb can reliably find, a control that
 * drifted outside the safe rectangle — are precisely the ones a screenshot does
 * not show. The audit runs on real resolved geometry, so it covers every window
 * shape the layout is ever asked to fit rather than the one that was rendered.
 */
object TouchLayoutAudit {

    /**
     * Smallest interactive target, in logical units.
     *
     * The platform accessibility guidance for an interactive target is 48; a
     * gameplay control may show smaller artwork but must not answer to a smaller
     * region than this.
     */
    const val MIN_TARGET_UNITS = 44f

    /**
     * Every already-logical action a usable controller has to expose.
     *
     * L2/R2 are deliberately absent: they are reached through
     * [TouchControlAction.Trigger], which publishes the digital bit AND the
     * analog value together, and are checked as triggers below.
     */
    private val REQUIRED_LOGICAL = setOf(
        ControllerButton.L1, ControllerButton.R1,
        ControllerButton.Select, ControllerButton.Start,
        ControllerButton.LeftStick, ControllerButton.RightStick,
        ControllerButton.Home, ControllerButton.Capture, ControllerButton.C,
    )

    fun audit(
        controls: List<ResolvedTouchControl>,
        region: TouchLayoutRegion,
    ): List<TouchLayoutFinding> {
        val findings = mutableListOf<TouchLayoutFinding>()
        val unit = region.unitScale.takeIf { it > 0f } ?: 1f

        controls.groupBy { it.id }.filterValues { it.size > 1 }.keys.forEach { duplicate ->
            findings += TouchLayoutFinding("Duplicate control id '$duplicate'", blocking = true)
        }

        controls.forEach { control ->
            val shortestUnits = min(control.hitHalfWidth, control.hitHalfHeight) * 2f / unit
            if (shortestUnits < MIN_TARGET_UNITS) {
                findings += TouchLayoutFinding(
                    "Control '${control.id}' answers to only ${shortestUnits.toInt()} units",
                    blocking = true,
                )
            }
            val outside = control.centerX - control.halfWidth < region.left - TOLERANCE ||
                control.centerX + control.halfWidth > region.right + TOLERANCE ||
                control.centerY - control.halfHeight < region.top - TOLERANCE ||
                control.centerY + control.halfHeight > region.bottom + TOLERANCE
            if (outside) {
                findings += TouchLayoutFinding(
                    "Control '${control.id}' is outside the interaction area",
                    blocking = true,
                )
            }
        }

        for (i in controls.indices) {
            for (j in i + 1 until controls.size) {
                val a = controls[i]
                val b = controls[j]
                if (!overlaps(a, b)) continue
                findings += TouchLayoutFinding(
                    "Controls '${a.id}' and '${b.id}' have overlapping hit regions",
                    // Overlap is always blocking: the router would have to let
                    // priority decide, and a control the user cannot reliably
                    // press is worse than a refusal to draw the layout.
                    blocking = true,
                )
            }
        }

        val actions = controls.map { it.spec.action }
        val logical = actions.filterIsInstance<TouchControlAction.Logical>().map { it.button }.toSet()
        (REQUIRED_LOGICAL - logical).forEach {
            findings += TouchLayoutFinding("Layout has no control for $it", blocking = false)
        }
        FaceButtonPosition.entries.forEach { position ->
            if (actions.none { it is TouchControlAction.Face && it.position == position }) {
                findings += TouchLayoutFinding("Layout has no $position face control", blocking = false)
            }
        }
        ControlSide.entries.forEach { side ->
            if (actions.none { it is TouchControlAction.Stick && it.side == side }) {
                findings += TouchLayoutFinding("Layout has no $side stick", blocking = false)
            }
            if (actions.none { it is TouchControlAction.Trigger && it.side == side }) {
                findings += TouchLayoutFinding("Layout has no $side trigger", blocking = false)
            }
        }
        if (actions.none { it is TouchControlAction.Directions }) {
            findings += TouchLayoutFinding("Layout has no D-pad", blocking = false)
        }
        return findings
    }

    /**
     * Bounding-box overlap, with a small tolerance.
     *
     * Deliberately coarser than the circular hit test: two circles whose boxes
     * touch at a corner do not actually overlap, but a layout that relies on
     * that is one font-scale change away from ambiguity, and the extra margin
     * costs nothing on a layout with real gutters.
     */
    private fun overlaps(a: ResolvedTouchControl, b: ResolvedTouchControl): Boolean {
        val dx = abs(a.centerX - b.centerX)
        val dy = abs(a.centerY - b.centerY)
        return dx < (a.hitHalfWidth + b.hitHalfWidth) - TOLERANCE &&
            dy < (a.hitHalfHeight + b.hitHalfHeight) - TOLERANCE
    }

    /** Half a pixel; guards against float placement noise, not against real overlap. */
    private const val TOLERANCE = 0.5f
}
