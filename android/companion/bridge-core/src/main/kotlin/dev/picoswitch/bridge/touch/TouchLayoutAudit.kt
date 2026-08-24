package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/** One thing wrong with a layout. [blocking] findings make a layout unplayable. */
data class TouchLayoutFinding(val message: String, val blocking: Boolean)

enum class TouchLayoutAuditMode {
    /** Repository-owned defaults: every declared output is mandatory. */
    ShippedTemplate,
    /** A user's draft may hide outputs, but unsafe geometry still blocks Save. */
    UserDraft,
    /** Runtime effective layout; hidden outputs are allowed and reported as warnings. */
    Runtime,
}

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
        val findings = auditGeometry(controls, region).toMutableList()

        // Legacy completeness check retained for callers that have not selected
        // a personality. Profile-backed layouts use the exhaustive overload
        // below instead of pretending every controller is a Pro2.
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

    fun audit(
        layout: TouchLayout,
        controls: List<ResolvedTouchControl>,
        region: TouchLayoutRegion,
        profile: TouchControllerProfile,
        mode: TouchLayoutAuditMode,
    ): List<TouchLayoutFinding> {
        val findings = auditGeometry(controls, region).toMutableList()
        val template = profile.defaultTemplate

        if (layout.profileId != profile.id) {
            findings += TouchLayoutFinding("Layout profile does not match ${profile.displayName}", true)
        }
        if (layout.templateId != template.id || layout.schemaVersion != template.schemaVersion ||
            layout.templateRevision != template.templateRevision
        ) {
            findings += TouchLayoutFinding("Layout template metadata is inconsistent", true)
        }

        layout.controls.forEach { control ->
            val finite = control.anchorX.isFinite() && control.anchorY.isFinite() &&
                control.widthUnits.isFinite() && control.heightUnits.isFinite() &&
                control.hitMarginUnits.isFinite() && control.visualRotationDegrees.isFinite() &&
                control.groupOffsetXUnits.isFinite() &&
                control.groupOffsetYUnits.isFinite()
            if (control.id.isBlank() || !finite || control.anchorX !in 0f..1f ||
                control.anchorY !in 0f..1f || control.widthUnits <= 0f ||
                control.heightUnits <= 0f || control.hitMarginUnits < 0f
            ) {
                findings += TouchLayoutFinding("Control '${control.id}' has invalid authored geometry", true)
            }
            if (control.output == TouchOutputControl.Unspecified || control.output !in profile.outputs) {
                findings += TouchLayoutFinding(
                    "Control '${control.id}' exposes ${control.output}, which is absent from ${profile.displayName}",
                    true,
                )
                return@forEach
            }
            val binding = profile.bindings[control.output]
            if (binding == null) {
                findings += TouchLayoutFinding("${control.output} has no fixed bridge binding", true)
            } else if (binding != control.action) {
                findings += TouchLayoutFinding("Control '${control.id}' changed the fixed ${control.output} binding", true)
            }
        }

        layout.controls.groupBy { it.output }.filterValues { it.size > 1 }.keys.forEach { output ->
            findings += TouchLayoutFinding("Profile output $output appears more than once", true)
        }
        val present = layout.controls.mapTo(mutableSetOf()) { it.output }
        (profile.outputs - present).forEach { missing ->
            findings += TouchLayoutFinding(
                "${profile.displayName} control $missing is hidden or missing",
                blocking = mode == TouchLayoutAuditMode.ShippedTemplate,
            )
        }
        return findings
    }

    private fun auditGeometry(
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
            // Both the artwork and the complete answerable target must remain in
            // the safe rectangle.  Checking only the visual half-extent would
            // let an invisible hit margin sit under a system gesture strip.
            val outside = control.centerX - control.hitHalfWidth < region.left - TOLERANCE ||
                control.centerX + control.hitHalfWidth > region.right + TOLERANCE ||
                control.centerY - control.hitHalfHeight < region.top - TOLERANCE ||
                control.centerY + control.hitHalfHeight > region.bottom + TOLERANCE
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

        return findings
    }

    /** Broad-phase boxes followed by the real hit shape where the boxes alone are ambiguous. */
    private fun overlaps(a: ResolvedTouchControl, b: ResolvedTouchControl): Boolean {
        val dx = abs(a.centerX - b.centerX)
        val dy = abs(a.centerY - b.centerY)
        val boxesOverlap = dx < (a.hitHalfWidth + b.hitHalfWidth) - TOLERANCE &&
            dy < (a.hitHalfHeight + b.hitHalfHeight) - TOLERANCE
        if (!boxesOverlap) return false

        // Circular controls can have diagonally intersecting bounding boxes while
        // their actual answerable regions remain disjoint. Use the same shape the
        // input router uses before declaring the layout ambiguous.
        if (a.spec.shape == TouchControlShape.Circle &&
            b.spec.shape == TouchControlShape.Circle &&
            abs(a.hitHalfWidth - a.hitHalfHeight) <= TOLERANCE &&
            abs(b.hitHalfWidth - b.hitHalfHeight) <= TOLERANCE
        ) {
            val minimumDistance = a.hitHalfWidth + b.hitHalfWidth - TOLERANCE
            return dx * dx + dy * dy < minimumDistance * minimumDistance
        }
        if (a.spec.shape != TouchControlShape.GameCubeContour &&
            b.spec.shape != TouchControlShape.GameCubeContour
        ) return true

        // A GameCube bean deliberately wraps around A: their boxes overlap in the
        // bean's empty concavity even though their answerable regions do not. Probe
        // only that small box intersection through the same hit tests used by input.
        val left = max(a.centerX - a.hitHalfWidth, b.centerX - b.hitHalfWidth)
        val right = min(a.centerX + a.hitHalfWidth, b.centerX + b.hitHalfWidth)
        val top = max(a.centerY - a.hitHalfHeight, b.centerY - b.hitHalfHeight)
        val bottom = min(a.centerY + a.hitHalfHeight, b.centerY + b.hitHalfHeight)
        val columns = max(1, ceil((right - left) / CONTOUR_PROBE_STEP).toInt())
        val rows = max(1, ceil((bottom - top) / CONTOUR_PROBE_STEP).toInt())
        for (row in 0 until rows) {
            val y = top + (row + 0.5f) * (bottom - top) / rows
            for (column in 0 until columns) {
                val x = left + (column + 0.5f) * (right - left) / columns
                if (a.hitTest(x, y) && b.hitTest(x, y)) return true
            }
        }
        return false
    }

    /** Half a pixel; guards against float placement noise, not against real overlap. */
    private const val TOLERANCE = 0.5f
    private const val CONTOUR_PROBE_STEP = 0.5f
}
