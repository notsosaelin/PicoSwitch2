package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/**
 * One thing wrong with a layout. [blocking] findings make a layout unplayable.
 *
 * [controlIds] names the instances the finding is ABOUT, when it is about
 * particular ones. It exists so an editor can point at the offending control
 * instead of printing a sentence: the same audit run that decides whether the
 * layout may be played decides which controls are drawn as broken, so the two
 * cannot disagree.
 */
data class TouchLayoutFinding(
    val message: String,
    val blocking: Boolean,
    val controlIds: Set<String> = emptySet(),
)

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

        // Duplicate outputs are LEGAL from Editor 2.0 onward. Two A buttons are
        // two instances contributing to one binding, and the engine aggregates
        // them; refusing the layout here would refuse the feature. What must
        // still hold is that each instance has its own identity and its own
        // unambiguous hit region, and `auditGeometry` above checks both.
        val present = layout.controls.mapTo(mutableSetOf()) { it.output }
        // Outputs the shipped default deliberately does not place are not
        // missing when they are absent — that IS the authored starting point.
        // Supporting a control and placing one are separate claims; see
        // [TouchTemplateControl.inDefaultLayout]. Everything else stays exactly
        // as strict, so a genuinely dropped control is still blocking.
        val optional = profile.defaultTemplate.controls
            .filterNot { it.inDefaultLayout }
            .mapTo(mutableSetOf()) { it.output }
        (profile.outputs - present - optional).forEach { missing ->
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
            findings += TouchLayoutFinding(
                "Duplicate control id '$duplicate'",
                blocking = true,
                controlIds = setOf(duplicate),
            )
        }

        controls.forEach { control ->
            val name = control.spec.displayName
            val shortestUnits = min(control.hitHalfWidth, control.hitHalfHeight) * 2f / unit
            if (shortestUnits < MIN_TARGET_UNITS) {
                findings += TouchLayoutFinding(
                    "$name answers to only ${shortestUnits.toInt()} units",
                    blocking = true,
                    controlIds = setOf(control.id),
                )
            }
            // Both the artwork and the complete answerable target must remain in
            // the safe rectangle.  Checking only the visual half-extent would
            // let an invisible hit margin sit under a system gesture strip, and
            // checking the UNROTATED extent would let a turned control's corner
            // do the same.
            val outside = control.centerX - control.hitExtentX < region.left - TOLERANCE ||
                control.centerX + control.hitExtentX > region.right + TOLERANCE ||
                control.centerY - control.hitExtentY < region.top - TOLERANCE ||
                control.centerY + control.hitExtentY > region.bottom + TOLERANCE
            if (outside) {
                findings += TouchLayoutFinding(
                    "$name is outside the interaction area",
                    blocking = true,
                    controlIds = setOf(control.id),
                )
            }
        }

        for (i in controls.indices) {
            for (j in i + 1 until controls.size) {
                val a = controls[i]
                val b = controls[j]
                // Two instances of the SAME output may overlap freely. Whichever
                // one a contact lands on produces the same thing, so there is no
                // ambiguity to report -- and stacking duplicates deliberately is
                // a reasonable way to build a larger target out of two controls.
                if (a.spec.output != TouchOutputControl.Unspecified &&
                    a.spec.output == b.spec.output
                ) continue
                when (overlap(a, b)) {
                    TouchOverlap.None -> Unit
                    // The DRAWN shapes collide. Blocking: the router would have
                    // to let z-order decide what the user pressed, and a control
                    // that answers unpredictably is worse than a layout that
                    // refuses to load and says why.
                    TouchOverlap.Artwork -> findings += TouchLayoutFinding(
                        "${a.spec.displayName} and ${b.spec.displayName} overlap",
                        blocking = true,
                        controlIds = setOf(a.id, b.id),
                    )
                    // Only the courtesy margins meet. Reported, never blocking:
                    // both controls remain reliably pressable by aiming at what
                    // is drawn, and a margin is an invitation rather than a
                    // claim on space. The shipped GameCube layout has exactly
                    // one of these, between `z` and the `Y` bean.
                    TouchOverlap.Margin -> findings += TouchLayoutFinding(
                        "${a.spec.displayName} and ${b.spec.displayName} have touching hit margins",
                        blocking = false,
                        controlIds = setOf(a.id, b.id),
                    )
                }
            }
        }

        return findings
    }

    /**
     * How badly two controls collide.
     *
     * The distinction that matters is the DRAWN shape against the courtesy
     * margin around it. A user aims at what they can see: if the artwork is
     * clear, both controls are reliably pressable and only the invisible
     * expansions are ambiguous. If the artwork itself overlaps, one of the two
     * cannot be pressed on purpose at all.
     */
    private fun overlap(a: ResolvedTouchControl, b: ResolvedTouchControl): TouchOverlap = when {
        !overlaps(a, b, visualOnly = false) -> TouchOverlap.None
        overlaps(a, b, visualOnly = true) -> TouchOverlap.Artwork
        else -> TouchOverlap.Margin
    }

    private enum class TouchOverlap { None, Margin, Artwork }

    /** Broad-phase boxes followed by the real hit shape where the boxes alone are ambiguous. */
    private fun overlaps(
        a: ResolvedTouchControl,
        b: ResolvedTouchControl,
        visualOnly: Boolean,
    ): Boolean {
        // Margins are the difference between the two passes, so the broad phase
        // has to shrink by them too or the visual pass would probe a box its own
        // shapes cannot reach.
        val marginA = a.hitHalfWidth - a.halfWidth
        val marginB = b.hitHalfWidth - b.halfWidth
        val extentAx = if (visualOnly) a.hitExtentX - marginA else a.hitExtentX
        val extentAy = if (visualOnly) a.hitExtentY - marginA else a.hitExtentY
        val extentBx = if (visualOnly) b.hitExtentX - marginB else b.hitExtentX
        val extentBy = if (visualOnly) b.hitExtentY - marginB else b.hitExtentY
        return overlapsWithin(a, b, extentAx, extentAy, extentBx, extentBy, visualOnly)
    }

    private fun overlapsWithin(
        a: ResolvedTouchControl,
        b: ResolvedTouchControl,
        extentAx: Float,
        extentAy: Float,
        extentBx: Float,
        extentBy: Float,
        visualOnly: Boolean,
    ): Boolean {
        val dx = abs(a.centerX - b.centerX)
        val dy = abs(a.centerY - b.centerY)
        // Screen-space extents, so a rotated control's real footprint is what is
        // compared. The unrotated half-extents describe the control's own frame
        // and would miss a corner that has turned into a neighbour.
        val boxesOverlap = dx < (extentAx + extentBx) - TOLERANCE &&
            dy < (extentAy + extentBy) - TOLERANCE
        if (!boxesOverlap) return false

        val halfAw = if (visualOnly) a.halfWidth else a.hitHalfWidth
        val halfAh = if (visualOnly) a.halfHeight else a.hitHalfHeight
        val halfBw = if (visualOnly) b.halfWidth else b.hitHalfWidth
        val halfBh = if (visualOnly) b.halfHeight else b.hitHalfHeight

        // Circular controls can have diagonally intersecting bounding boxes while
        // their actual answerable regions remain disjoint. Use the same shape the
        // input router uses before declaring the layout ambiguous.
        if (a.spec.shape == TouchControlShape.Circle &&
            b.spec.shape == TouchControlShape.Circle &&
            abs(halfAw - halfAh) <= TOLERANCE &&
            abs(halfBw - halfBh) <= TOLERANCE
        ) {
            val minimumDistance = halfAw + halfBw - TOLERANCE
            return dx * dx + dy * dy < minimumDistance * minimumDistance
        }
        val needsExactProbe = a.spec.shape == TouchControlShape.GameCubeContour ||
            b.spec.shape == TouchControlShape.GameCubeContour ||
            // A rotated rectangle's screen-space box is larger than the shape,
            // so two turned controls can have intersecting boxes and disjoint
            // regions exactly as a bean and its neighbour do.
            a.spec.visualRotationDegrees != 0f || b.spec.visualRotationDegrees != 0f
        if (!needsExactProbe) return true

        // A GameCube bean deliberately wraps around A: their boxes overlap in the
        // bean's empty concavity even though their answerable regions do not. Probe
        // only that small box intersection through the same hit tests used by input.
        val left = max(a.centerX - extentAx, b.centerX - extentBx)
        val right = min(a.centerX + extentAx, b.centerX + extentBx)
        val top = max(a.centerY - extentAy, b.centerY - extentBy)
        val bottom = min(a.centerY + extentAy, b.centerY + extentBy)
        val columns = max(1, ceil((right - left) / CONTOUR_PROBE_STEP).toInt())
        val rows = max(1, ceil((bottom - top) / CONTOUR_PROBE_STEP).toInt())
        for (row in 0 until rows) {
            val y = top + (row + 0.5f) * (bottom - top) / rows
            for (column in 0 until columns) {
                val x = left + (column + 0.5f) * (right - left) / columns
                val hitA = if (visualOnly) a.containsVisual(x, y) else a.hitTest(x, y)
                if (!hitA) continue
                val hitB = if (visualOnly) b.containsVisual(x, y) else b.hitTest(x, y)
                if (hitB) return true
            }
        }
        return false
    }

    /** Half a pixel; guards against float placement noise, not against real overlap. */
    private const val TOLERANCE = 0.5f
    private const val CONTOUR_PROBE_STEP = 0.5f
}
