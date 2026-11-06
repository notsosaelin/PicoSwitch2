package dev.picoswitch.bridge.touch

import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.roundToInt

/** A movement in the interaction region's pixel space. */
data class TouchEditorDelta(val x: Float, val y: Float) {
    companion object { val Zero = TouchEditorDelta(0f, 0f) }
}

/** Why a guide is being drawn; hosts may style the kinds differently. */
enum class TouchGuideKind {
    /** The interaction area's horizontal or vertical mid-line. */
    RegionCenter,

    /** Another control's centre on the same axis. */
    ControlAlignment,

    /** The innermost line the selection's own extent can reach. */
    SafeEdge,

    /** A grid line, when the grid is enabled. */
    Grid,
}

/**
 * One infinite line the host draws across the interaction area.
 *
 * [vertical] describes the LINE, not the axis it constrains: a vertical line
 * lives at a constant x.
 */
data class TouchGuideLine(
    val vertical: Boolean,
    val position: Float,
    val kind: TouchGuideKind,
)

/** What the editor's alignment assistance is currently doing. */
data class TouchAlignmentSettings(
    /** Draw the grid. Independent of [snap]: a visible grid is an aid, not a rule. */
    val grid: Boolean = false,
    /** Pull a moved selection onto nearby guides. */
    val snap: Boolean = false,
) {
    companion object { val Off = TouchAlignmentSettings() }
}

/**
 * Optional alignment assistance for a direct-manipulation editor.
 *
 * Two properties are deliberate and both come from the editor design:
 *
 * - Guides ASSIST. They never restrict placement: with snapping on, a movement
 *   larger than [SNAP_TOLERANCE_UNITS] always wins over the nearest guide, so no
 *   position becomes unreachable.
 * - The grid is drawn in the interaction region, not the window. Anchoring it to
 *   the window would put grid lines under the gesture strips and cutouts that
 *   the layout itself already refuses to place controls in, and "aligned to the
 *   grid" would stop meaning "aligned with the other controls".
 *
 * All of this is pure and operates on already-resolved pixel geometry, so it is
 * testable at every aspect ratio without a device and identical on every host.
 */
object TouchEditorAlignment {

    /**
     * Grid pitch in logical units.
     *
     * Half the smallest accessible target ([TouchLayoutAudit.MIN_TARGET_UNITS],
     * 44): fine enough to place a control deliberately, coarse enough that the
     * drawn grid stays a background reference rather than a texture over the
     * controls the user is trying to judge.
     */
    const val GRID_STEP_UNITS = 22f

    /** How near a guide has to be, in logical units, before it pulls. */
    const val SNAP_TOLERANCE_UNITS = 7f

    /** How near a guide has to be before the host draws it as matched. */
    const val MATCH_TOLERANCE_UNITS = 1.5f

    /**
     * Adjust a proposed movement so the selection lands on a nearby guide.
     *
     * The correction is computed from ONE reference control — [primaryId] — and
     * then applied to the whole movement, so a multi-control selection keeps its
     * internal spacing exactly. Snapping the members individually would align
     * each of them to a different guide and pull the cluster apart.
     *
     * Deltas are incremental, one per pointer event, which gives the sticky
     * behaviour a snap is supposed to have: once on a guide, small movements keep
     * resolving back onto it until one is large enough to escape.
     */
    fun snap(
        layout: ResolvedTouchLayout,
        selection: Set<String>,
        primaryId: String,
        delta: TouchEditorDelta,
        settings: TouchAlignmentSettings,
    ): TouchEditorDelta {
        if (!settings.snap) return delta
        if (!delta.x.isFinite() || !delta.y.isFinite()) return TouchEditorDelta.Zero
        val primary = layout.control(primaryId) ?: return delta
        val region = layout.region
        val unit = region.unitScale.takeIf { it > 0f } ?: return delta
        val tolerance = SNAP_TOLERANCE_UNITS * unit

        val targetX = primary.centerX + delta.x
        val targetY = primary.centerY + delta.y
        val snappedX = nearest(candidates(layout, selection, region, settings, vertical = true, primary), targetX, tolerance)
        val snappedY = nearest(candidates(layout, selection, region, settings, vertical = false, primary), targetY, tolerance)
        return TouchEditorDelta(
            x = if (snappedX == null) delta.x else delta.x + (snappedX - targetX),
            y = if (snappedY == null) delta.y else delta.y + (snappedY - targetY),
        )
    }

    /**
     * The guides the current placement is actually sitting on.
     *
     * Returned for DRAWING only. A guide that appears when nothing is aligned is
     * noise, and a guide that never appears when something is aligned makes the
     * user check alignment by eye — which is the problem the feature exists to
     * remove.
     */
    fun matchedGuides(
        layout: ResolvedTouchLayout,
        selection: Set<String>,
        primaryId: String?,
        settings: TouchAlignmentSettings,
    ): List<TouchGuideLine> {
        val primary = primaryId?.let(layout::control) ?: return emptyList()
        val region = layout.region
        val unit = region.unitScale.takeIf { it > 0f } ?: return emptyList()
        val tolerance = MATCH_TOLERANCE_UNITS * unit
        val lines = mutableListOf<TouchGuideLine>()
        candidates(layout, selection, region, settings, vertical = true, primary)
            .filter { abs(it.position - primary.centerX) <= tolerance }
            .forEach { lines += it }
        candidates(layout, selection, region, settings, vertical = false, primary)
            .filter { abs(it.position - primary.centerY) <= tolerance }
            .forEach { lines += it }
        // Two guides of different kinds can coincide (a control that happens to
        // sit on the centre line). Draw the more specific one only.
        return lines
            .groupBy { it.vertical to it.position.roundToInt() }
            .map { (_, group) -> group.minByOrNull { it.kind.ordinal }!! }
    }

    /**
     * The grid lines to draw, inside the interaction region.
     *
     * Anchored to the region's centre rather than its left edge so the grid is
     * symmetric: the centre column and row always exist, and a layout that is
     * mirror-symmetric stays symmetric when both halves are snapped.
     */
    fun gridLines(region: TouchLayoutRegion, settings: TouchAlignmentSettings): List<TouchGuideLine> {
        if (!settings.grid) return emptyList()
        val step = GRID_STEP_UNITS * (region.unitScale.takeIf { it > 0f } ?: return emptyList())
        if (step <= 0f || region.width <= 0f || region.height <= 0f) return emptyList()
        val lines = mutableListOf<TouchGuideLine>()
        gridPositions(region.left, region.right, step).forEach {
            lines += TouchGuideLine(vertical = true, position = it, kind = TouchGuideKind.Grid)
        }
        gridPositions(region.top, region.bottom, step).forEach {
            lines += TouchGuideLine(vertical = false, position = it, kind = TouchGuideKind.Grid)
        }
        return lines
    }

    private fun gridPositions(start: Float, end: Float, step: Float): List<Float> {
        val center = (start + end) / 2f
        val first = center - floor((center - start) / step) * step
        val positions = mutableListOf<Float>()
        var value = first
        var guard = 0
        while (value <= end && guard < MAX_GRID_LINES) {
            positions += value
            value += step
            guard++
        }
        return positions
    }

    private fun candidates(
        layout: ResolvedTouchLayout,
        selection: Set<String>,
        region: TouchLayoutRegion,
        settings: TouchAlignmentSettings,
        vertical: Boolean,
        primary: ResolvedTouchControl,
    ): List<TouchGuideLine> {
        val lines = mutableListOf<TouchGuideLine>()
        val center = if (vertical) (region.left + region.right) / 2f else (region.top + region.bottom) / 2f
        lines += TouchGuideLine(vertical, center, TouchGuideKind.RegionCenter)

        // The innermost position the primary control's own answerable extent can
        // occupy. Offering the raw region edge would propose a placement the
        // audit then blocks.
        val half = if (vertical) primary.hitHalfWidth else primary.hitHalfHeight
        val low = (if (vertical) region.left else region.top) + half
        val high = (if (vertical) region.right else region.bottom) - half
        if (low <= high) {
            lines += TouchGuideLine(vertical, low, TouchGuideKind.SafeEdge)
            lines += TouchGuideLine(vertical, high, TouchGuideKind.SafeEdge)
        }

        layout.controls.forEach { control ->
            if (control.id in selection) return@forEach
            lines += TouchGuideLine(
                vertical,
                if (vertical) control.centerX else control.centerY,
                TouchGuideKind.ControlAlignment,
            )
        }

        if (settings.grid) lines += gridLines(region, settings).filter { it.vertical == vertical }
        return lines
    }

    private fun nearest(
        candidates: List<TouchGuideLine>,
        target: Float,
        tolerance: Float,
    ): Float? = candidates
        .filter { abs(it.position - target) <= tolerance }
        // Nearest first. Kind breaks a tie only when two guides coincide, where
        // sitting on the centre line is the more meaningful statement to make
        // than sitting on whichever grid line happens to be there too.
        .minWithOrNull(
            compareBy({ abs(it.position - target) }, { it.kind.ordinal }),
        )
        ?.position

    /** Bound on grid generation; a degenerate unit scale must not spin here. */
    private const val MAX_GRID_LINES = 512
}
