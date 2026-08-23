package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/** Which side of a two-of-a-kind control this is. */
enum class ControlSide { Left, Right }

/**
 * What a control does to the controller state.
 *
 * Separated from [TouchControlKind] because kind describes INTERACTION (does it
 * track a vector, or is it a press?) while action describes MEANING. A trigger
 * and a shoulder are both pressed the same way and mean different things; a
 * D-pad and a stick mean different things and are tracked the same way.
 */
sealed interface TouchControlAction {
    /** Already-logical bridge button: shoulders, `-`/`+`, stick clicks, Home, Capture, C. */
    data class Logical(val button: ControllerButton) : TouchControlAction

    /** A face-diamond POSITION; the shared layout resolver decides label and bit. */
    data class Face(val position: FaceButtonPosition) : TouchControlAction

    /** The eight-way directional control. */
    data object Directions : TouchControlAction

    data class Stick(val side: ControlSide) : TouchControlAction

    data class Trigger(val side: ControlSide) : TouchControlAction

    /**
     * Opens the on-screen controller's own menu (settings, exit).
     *
     * A HOST action, not a controller action: it changes nothing in the
     * controller state and never reaches the bridge. It lives in the layout
     * anyway so its hit region is placed, audited for overlap and reserved
     * against the gameplay controls by the same mechanism as everything else —
     * an exit affordance that a stray thumb can shadow is not an exit.
     */
    data object SystemMenu : TouchControlAction
}

/**
 * How a control is driven by a contact.
 *
 * [Stick] and [Dpad] track the contact's position for as long as they own it;
 * everything else is a press that lasts while the contact is held.
 */
enum class TouchControlKind { Button, FaceButton, Dpad, Stick, Trigger }

/** Hit-region shape. Visual rendering may differ; this is what the router tests. */
enum class TouchControlShape { Circle, Rectangle }

/**
 * One control, described declaratively.
 *
 * Three things are kept apart on purpose, because collapsing them is what makes
 * a touch controller impossible to re-lay-out later:
 *
 * - [action] — the semantic effect on controller state;
 * - the geometry fields — where the hit region is and how big;
 * - everything visual, which lives in the renderer and is not here at all.
 *
 * Geometry is platform-neutral. [anchorX]/[anchorY] are normalized within the
 * interaction region so the same layout describes a phone and a tablet, and
 * sizes are in LOGICAL UNITS — a density-independent unit the platform adapter
 * converts — so a control is a thumb-sized thing rather than a pixel count from
 * whichever device it was authored on.
 */
data class TouchControlSpec(
    val id: String,
    val kind: TouchControlKind,
    val action: TouchControlAction,
    /** Centre within the interaction region, `0..1`. */
    val anchorX: Float,
    val anchorY: Float,
    /** Nominal size in logical units, before layout scaling. */
    val widthUnits: Float,
    val heightUnits: Float,
    val shape: TouchControlShape = TouchControlShape.Circle,
    /**
     * Hit margin added around the visual bounds, in logical units.
     *
     * Artwork may be smaller than the target it answers to, but an expanded
     * target that overlaps a neighbour's makes z-order decide what the user
     * pressed. [TouchLayoutAudit] rejects that.
     */
    val hitMarginUnits: Float = 0f,
    /** Higher wins when two hit regions still overlap. */
    val priority: Int = 0,
    /** Drawn legend, when the control's label is not derived from a face layout. */
    val label: String = "",
)

/**
 * A complete on-screen controller, versioned.
 *
 * [schemaVersion] exists from the first release even though nothing migrates
 * yet: the alternative is discovering later that a stored layout blob has no way
 * to say which shape it is, and the only safe response then is to throw every
 * user's configuration away.
 */
data class TouchLayout(
    val id: String,
    val schemaVersion: Int,
    val controls: List<TouchControlSpec>,
)

/**
 * The rectangle a layout is resolved into, plus the platform's unit scale.
 *
 * This is the interaction-safe area, NOT the window: system gesture strips,
 * cutouts and caption bars are subtracted by the platform adapter before the
 * layout ever sees a number. A background may still be drawn edge to edge; that
 * is decoration and never changes hit geometry.
 *
 * [unitScale] is pixels per logical unit — the one place the portable layer is
 * told how big a physical thumb is on this display.
 */
data class TouchLayoutRegion(
    val left: Float,
    val top: Float,
    val right: Float,
    val bottom: Float,
    val unitScale: Float,
) {
    val width: Float get() = right - left
    val height: Float get() = bottom - top
    val widthUnits: Float get() = if (unitScale > 0f) width / unitScale else 0f
    val heightUnits: Float get() = if (unitScale > 0f) height / unitScale else 0f
}

/** One control placed in real coordinates. */
data class ResolvedTouchControl(
    val spec: TouchControlSpec,
    val centerX: Float,
    val centerY: Float,
    /** Visual half-extent. */
    val halfWidth: Float,
    val halfHeight: Float,
    /** Hit half-extent; never smaller than the visual one. */
    val hitHalfWidth: Float,
    val hitHalfHeight: Float,
) {
    val id: String get() = spec.id

    /** Travel radius for the vector controls; the smaller half-extent keeps it circular. */
    val trackingRadius: Float get() = min(halfWidth, halfHeight)

    fun hitTest(x: Float, y: Float): Boolean {
        val dx = x - centerX
        val dy = y - centerY
        if (hitHalfWidth <= 0f || hitHalfHeight <= 0f) return false
        return when (spec.shape) {
            TouchControlShape.Circle -> {
                val nx = dx / hitHalfWidth
                val ny = dy / hitHalfHeight
                nx * nx + ny * ny <= 1f
            }
            TouchControlShape.Rectangle -> abs(dx) <= hitHalfWidth && abs(dy) <= hitHalfHeight
        }
    }

    /**
     * How central the hit is, `0` at the centre and `1` at the edge.
     *
     * Used only to break a tie between two controls that both accept the point,
     * so an overlap resolves to the one the thumb is more plainly on rather than
     * to whichever happens to be later in the list.
     */
    fun normalizedDistance(x: Float, y: Float): Float {
        val nx = if (hitHalfWidth > 0f) (x - centerX) / hitHalfWidth else Float.MAX_VALUE
        val ny = if (hitHalfHeight > 0f) (y - centerY) / hitHalfHeight else Float.MAX_VALUE
        return max(abs(nx), abs(ny))
    }
}

/**
 * A layout placed into a region.
 *
 * [fits] is the layout's own verdict on whether it can be played. A window can
 * genuinely become too small — a freeform window dragged narrow, a very short
 * landscape at a large font scale — and drawing overlapping controls there would
 * send the console input the user did not choose. The surface is expected to
 * neutralize and say so instead.
 */
data class ResolvedTouchLayout(
    val layout: TouchLayout,
    val region: TouchLayoutRegion,
    val controls: List<ResolvedTouchControl>,
    val scale: Float,
    val fits: Boolean,
    val problem: String? = null,
) {
    /** Built once so the router's per-move owner lookup is not a list scan. */
    private val byId: Map<String, ResolvedTouchControl> = controls.associateBy { it.id }

    fun control(id: String): ResolvedTouchControl? = byId[id]

    companion object {
        val Empty = ResolvedTouchLayout(
            layout = TouchLayout("empty", TouchLayoutV1.SCHEMA_VERSION, emptyList()),
            region = TouchLayoutRegion(0f, 0f, 0f, 0f, 1f),
            controls = emptyList(),
            scale = 1f,
            fits = false,
            problem = "No interaction area has been measured yet",
        )
    }
}

/**
 * Normalized layout + real rectangle -> real control geometry.
 *
 * The scaling rule is deliberately not "multiply everything by the window
 * width". A twelve-inch tablet does not have larger thumbs, so past a point the
 * controls stop growing and the SPACE between them absorbs the extra room. That
 * keeps every control at the edge where a thumb can reach it and preserves the
 * quiet centre, which is the whole ergonomic argument for this arrangement.
 */
object TouchLayoutResolver {

    /**
     * The window this layout was authored against, in logical units. Scale is
     * relative to this, so `1.0` is the shape the numbers were chosen for.
     */
    const val REFERENCE_WIDTH_UNITS = 800f
    const val REFERENCE_HEIGHT_UNITS = 400f

    /** Bounds on that scale; see the class doc for why the top one is low. */
    const val MIN_SCALE = 0.78f
    const val MAX_SCALE = 1.25f

    /**
     * Smallest region a controller can be laid out in at all.
     *
     * Chosen as the reference shape at [MIN_SCALE], because that is the point
     * below which the controls stop shrinking with the window and start closing
     * the gaps between each other. Below it the audit would fail anyway; refusing
     * here gives the surface one truthful thing to say instead of a screenful of
     * overlapping targets.
     */
    const val MIN_REGION_WIDTH_UNITS = REFERENCE_WIDTH_UNITS * MIN_SCALE
    const val MIN_REGION_HEIGHT_UNITS = REFERENCE_HEIGHT_UNITS * MIN_SCALE

    fun resolve(layout: TouchLayout, region: TouchLayoutRegion): ResolvedTouchLayout {
        if (region.unitScale <= 0f || region.width <= 0f || region.height <= 0f) {
            return ResolvedTouchLayout(
                layout, region, emptyList(), 1f, fits = false,
                problem = "The interaction area has no usable size",
            )
        }

        val scale = min(
            region.widthUnits / REFERENCE_WIDTH_UNITS,
            region.heightUnits / REFERENCE_HEIGHT_UNITS,
        ).coerceIn(MIN_SCALE, MAX_SCALE)

        val controls = layout.controls.map { spec -> place(spec, region, scale) }

        val tooSmall = region.widthUnits < MIN_REGION_WIDTH_UNITS ||
            region.heightUnits < MIN_REGION_HEIGHT_UNITS
        val problem = when {
            tooSmall -> "This window is too small for the on-screen controller"
            else -> TouchLayoutAudit.audit(controls, region).firstOrNull { it.blocking }?.message
        }

        return ResolvedTouchLayout(
            layout = layout,
            region = region,
            controls = controls,
            scale = scale,
            fits = problem == null,
            problem = problem,
        )
    }

    private fun place(
        spec: TouchControlSpec,
        region: TouchLayoutRegion,
        scale: Float,
    ): ResolvedTouchControl {
        val unit = region.unitScale * scale
        val halfWidth = spec.widthUnits * unit / 2f
        val halfHeight = spec.heightUnits * unit / 2f
        val margin = spec.hitMarginUnits * unit

        // Keep the whole visual bounds inside the safe rectangle. An anchor near
        // an edge is an intent ("hug the left"), not a licence to place half a
        // control under a gesture strip.
        val centerX = (region.left + spec.anchorX * region.width)
            .coerceIn(region.left + halfWidth, max(region.left + halfWidth, region.right - halfWidth))
        val centerY = (region.top + spec.anchorY * region.height)
            .coerceIn(region.top + halfHeight, max(region.top + halfHeight, region.bottom - halfHeight))

        return ResolvedTouchControl(
            spec = spec,
            centerX = centerX,
            centerY = centerY,
            halfWidth = halfWidth,
            halfHeight = halfHeight,
            hitHalfWidth = halfWidth + margin,
            hitHalfHeight = halfHeight + margin,
        )
    }
}
