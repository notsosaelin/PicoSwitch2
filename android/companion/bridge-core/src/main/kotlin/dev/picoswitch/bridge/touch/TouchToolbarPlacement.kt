package dev.picoswitch.bridge.touch

import kotlin.math.max
import kotlin.math.min

/** Which safe edge the editor toolbar can dock to. */
enum class TouchToolbarEdge(val key: String, val title: String) {
    Top("top", "Top"),
    Bottom("bottom", "Bottom"),
    Left("left", "Left"),
    Right("right", "Right");

    /** Docked left or right, the toolbar lays its buttons out in a column. */
    val vertical: Boolean get() = this == Left || this == Right

    companion object {
        fun fromKey(value: String?): TouchToolbarEdge? = entries.firstOrNull { it.key == value }
    }
}

/**
 * Where the editor's toolbar sits.
 *
 * Docked and floating are genuinely different states rather than one position
 * with a flag: a docked toolbar follows its edge when the window changes shape,
 * and a floating one keeps the place the user put it. Collapsing them would mean
 * choosing which of those two behaviours to get wrong.
 */
sealed interface TouchToolbarPlacement {
    data class Docked(val edge: TouchToolbarEdge) : TouchToolbarPlacement

    /**
     * Free position, normalized within the interaction-safe region.
     *
     * Normalized rather than in pixels so the toolbar lands in the same PLACE on
     * a re-open at a different size, and clamped on the way out so it can never
     * come back somewhere the user cannot reach.
     */
    data class Floating(val x: Float, val y: Float) : TouchToolbarPlacement

    companion object {
        val Default: TouchToolbarPlacement = Docked(TouchToolbarEdge.Bottom)
    }
}

/**
 * The geometry behind a draggable, dockable toolbar.
 *
 * Pure, and in the shared module, because every rule here is a rule about
 * REACHABILITY — which edge is close enough to dock to, whether a remembered
 * position still lies inside the window — and a reachability rule that lives in
 * one platform's UI layer is a rule the next platform gets wrong.
 */
object TouchToolbarLayout {

    /**
     * How close a dragged toolbar must come to a safe edge to offer docking, as
     * a fraction of the toolbar's own shorter side.
     *
     * Derived from the toolbar rather than a raw pixel constant: the zone has to
     * be about one control wide on every density, and a magic number would be a
     * different physical distance on each device.
     */
    const val SNAP_ZONE_FACTOR = 1.0f

    /**
     * Which edge a toolbar at this position is offering to dock to, if any.
     *
     * [x] and [y] are the toolbar's top-left in the region's own coordinates.
     * The nearest edge inside the zone wins, and a tie resolves to the smaller
     * gap on the axis the toolbar is shorter along — which is the edge it will
     * visibly line up with.
     */
    fun dockCandidate(
        x: Float,
        y: Float,
        toolbarWidth: Float,
        toolbarHeight: Float,
        region: TouchLayoutRegion,
    ): TouchToolbarEdge? {
        if (region.width <= 0f || region.height <= 0f) return null
        val zone = min(toolbarWidth, toolbarHeight) * SNAP_ZONE_FACTOR
        if (zone <= 0f) return null
        val gaps = listOf(
            TouchToolbarEdge.Left to (x - region.left),
            TouchToolbarEdge.Right to (region.right - (x + toolbarWidth)),
            TouchToolbarEdge.Top to (y - region.top),
            TouchToolbarEdge.Bottom to (region.bottom - (y + toolbarHeight)),
        ).filter { it.second <= zone }
        return gaps.minByOrNull { it.second }?.first
    }

    /**
     * Bring a placement back inside the region, keeping its kind.
     *
     * A dock is always valid — the edge exists whatever the window does — so only
     * a floating position can need repair. Called after every geometry change,
     * because a toolbar the user cannot reach is a toolbar with no Done button.
     */
    fun clamp(
        placement: TouchToolbarPlacement,
        toolbarWidth: Float,
        toolbarHeight: Float,
        region: TouchLayoutRegion,
    ): TouchToolbarPlacement {
        if (placement !is TouchToolbarPlacement.Floating) return placement
        // A value that is not a number has no position to clamp TO, and letting
        // one through puts the toolbar at NaN, which draws nowhere and cannot be
        // dragged back. The codec already refuses these on the way in; this is
        // the guard for every other way one could be constructed.
        if (!placement.x.isFinite() || !placement.y.isFinite()) {
            return TouchToolbarPlacement.Default
        }
        if (region.width <= 0f || region.height <= 0f) return placement
        val maxX = max(0f, region.width - toolbarWidth)
        val maxY = max(0f, region.height - toolbarHeight)
        val x = (placement.x * region.width).coerceIn(0f, maxX)
        val y = (placement.y * region.height).coerceIn(0f, maxY)
        return TouchToolbarPlacement.Floating(
            x = if (region.width > 0f) x / region.width else 0f,
            y = if (region.height > 0f) y / region.height else 0f,
        )
    }

    /**
     * Where the toolbar's top-left goes, for ANY placement.
     *
     * Docked positions are computed here rather than left to a host's own
     * "align to the edge of the screen", and that is the whole point: the edge a
     * toolbar docks to is the INTERACTION-SAFE edge, not the physical one. A
     * host aligning to the window would put a docked toolbar under the system
     * gesture strip or behind a cutout — the same mistake the layout resolver
     * exists to prevent for controls.
     */
    fun topLeft(
        placement: TouchToolbarPlacement,
        toolbarWidth: Float,
        toolbarHeight: Float,
        region: TouchLayoutRegion,
    ): Pair<Float, Float> {
        val raw = when (val safe = clamp(placement, toolbarWidth, toolbarHeight, region)) {
            is TouchToolbarPlacement.Floating ->
                (region.left + safe.x * region.width) to
                    (region.top + safe.y * region.height)
            is TouchToolbarPlacement.Docked -> {
                val centredX = region.left + (region.width - toolbarWidth) / 2f
                val centredY = region.top + (region.height - toolbarHeight) / 2f
                when (safe.edge) {
                    TouchToolbarEdge.Top -> centredX to region.top
                    TouchToolbarEdge.Bottom -> centredX to (region.bottom - toolbarHeight)
                    TouchToolbarEdge.Left -> region.left to centredY
                    TouchToolbarEdge.Right -> (region.right - toolbarWidth) to centredY
                }
            }
        }
        return reachable(raw.first, raw.second, toolbarWidth, toolbarHeight, region)
    }

    /**
     * THE invariant: the toolbar's leading corner is always inside the region.
     *
     * Every other rule above assumes the toolbar fits. When it does not — a
     * freeform window dragged small, a large font scale, an inset that grew —
     * the arithmetic that centres or right-aligns it produces a coordinate
     * outside the safe rectangle, and a right-docked toolbar whose leading edge
     * has gone off-screen has taken the drag handle with it. There is then no
     * gesture that brings it back and no Done button to leave by.
     *
     * So the top-left is coerced last, unconditionally, into the region. When the
     * toolbar is genuinely wider than the window it overflows the FAR edge, where
     * wrapping has already put the least-critical buttons, and the handle stays
     * where a finger can reach it.
     */
    private fun reachable(
        x: Float,
        y: Float,
        toolbarWidth: Float,
        toolbarHeight: Float,
        region: TouchLayoutRegion,
    ): Pair<Float, Float> {
        if (region.width <= 0f || region.height <= 0f) return x to y
        val maxX = max(region.left, region.right - toolbarWidth)
        val maxY = max(region.top, region.bottom - toolbarHeight)
        return x.coerceIn(region.left, maxX) to y.coerceIn(region.top, maxY)
    }

    /**
     * Turn a dragged pixel position into the placement a release would produce.
     *
     * One function so the docking PREVIEW and the docking RESULT cannot disagree:
     * the surface highlights whatever this returns while the drag is live and
     * commits the same value when the finger lifts.
     */
    fun placementFor(
        x: Float,
        y: Float,
        toolbarWidth: Float,
        toolbarHeight: Float,
        region: TouchLayoutRegion,
    ): TouchToolbarPlacement {
        val edge = dockCandidate(x, y, toolbarWidth, toolbarHeight, region)
        if (edge != null) return TouchToolbarPlacement.Docked(edge)
        val normalized = TouchToolbarPlacement.Floating(
            x = if (region.width > 0f) (x - region.left) / region.width else 0f,
            y = if (region.height > 0f) (y - region.top) / region.height else 0f,
        )
        return clamp(normalized, toolbarWidth, toolbarHeight, region)
    }
}
