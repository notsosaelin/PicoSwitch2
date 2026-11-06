package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerLayoutResolver
import dev.picoswitch.bridge.core.FaceButtonPosition
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

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

    /**
     * A trigger, and whether the personality behind it has real travel.
     *
     * [analog] is a statement about the CONSOLE-FACING controller, not about the
     * on-screen control: only the NSO GameCube personality carries a continuous
     * trigger byte the console acts on (`switch_gc_encode` writes it, and the
     * firmware's GameCube seam derives the terminal detent from it). Pro
     * Controller 2 and Joy-Con triggers are digital on the far side however hard
     * they are pulled, so giving them a travel gesture would let a stray drag
     * silently send nothing at all.
     *
     * The profile catalog is where this is decided, because the profile is what
     * knows which console controller a binding produces. See
     * [TouchAnalogTriggerState] for what the flag switches on.
     */
    data class Trigger(val side: ControlSide, val analog: Boolean = false) : TouchControlAction
}

/**
 * How a control is driven by a contact.
 *
 * [Stick] and [Dpad] track the contact's position for as long as they own it;
 * everything else is a press that lasts while the contact is held.
 */
enum class TouchControlKind { Button, FaceButton, Dpad, Stick, Trigger }

/**
 * Whether a persistent hold is even meaningful for this kind of control.
 *
 * Digital only. A stick and the unified D-pad are CONTINUOUS controls whose
 * value is the contact's position, so there is no single state to hold; a
 * latched one would also be the most disruptive thing on the layout, because a
 * direction the user cannot see themselves holding walks the character into a
 * wall. Excluded structurally rather than by configuration so no stored document
 * can ask for it.
 */
val TouchControlKind.supportsLatch: Boolean
    get() = this == TouchControlKind.Button ||
        this == TouchControlKind.FaceButton ||
        this == TouchControlKind.Trigger

/** Hit-region shape. Visual rendering may differ; this is what the router tests. */
enum class TouchControlShape { Circle, Rectangle, GameCubeContour }

/**
 * Platform-neutral drawing role.
 *
 * The role says what silhouette the control has, not how a host draws it.  In
 * particular it never contains a resource id or asset path.  This keeps shipped
 * templates portable while still allowing the GameCube and Joy-Con profiles to
 * look like the controller they actually produce.
 */
enum class TouchVisualRole {
    Default,
    RoundButton,
    RectangularButton,
    UnifiedDpad,
    AnalogStick,
    GameCubeLargeA,
    GameCubeSmallB,
    GameCubeBeanX,
    GameCubeBeanY,
    JoyConButton,
    JoyConDirectionButton,
    Utility,
}

/**
 * Platform-neutral glyph role. The host renderer owns the actual drawing paths;
 * no platform resource id or asset path crosses into `:bridge-core`.
 */
enum class TouchControlGlyph { Capture, Home }

/**
 * One control, described declaratively.
 *
 * Three things are kept apart on purpose, because collapsing them is what makes
 * a touch controller impossible to re-lay-out later:
 *
 * - [action] — the semantic effect on controller state;
 * - the geometry fields — where the hit region is and how big;
 * - [label]/[glyph] — portable visual content which the host renderer implements.
 *
 * Geometry is platform-neutral. [anchorX]/[anchorY] are normalized within the
 * interaction region so the same layout describes a phone and a tablet, and
 * sizes are in LOGICAL UNITS — a density-independent unit the platform adapter
 * converts — so a control is a thumb-sized thing rather than a pixel count from
 * whichever device it was authored on.
 */
data class TouchControlSpec(
    /** The INSTANCE id. Unique within a layout; not a binding and not a kind. */
    val id: String,
    /** Which catalog entry this instance was made from. Not unique. */
    val catalogId: String = id,
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
    /**
     * Draw and hit order within the layout, low to high.
     *
     * A real layout property rather than incidental list position, because once
     * duplicate and freely placed instances exist, "which one is in front" is a
     * question the user can answer and the arrangement has to remember. The
     * router reads the same number the renderer does, so the control drawn on
     * top is the control a thumb lands on.
     */
    val zIndex: Int = 0,
    /** Drawn legend, when the control's label is not derived from a face layout. */
    val label: String = "",
    /** Optional symbol in place of [label]. */
    val glyph: TouchControlGlyph? = null,
    /** The personality-visible output this control represents. */
    val output: TouchOutputControl = TouchOutputControl.Unspecified,
    /** Portable visual treatment; the host owns the actual paths and colours. */
    val visualRole: TouchVisualRole = TouchVisualRole.Default,
    /**
     * TOTAL clockwise rotation: the catalog entry's authored orientation plus
     * whatever the user has turned this instance by.
     *
     * One number because rendering and hit testing must never disagree about
     * how far the silhouette is turned, and a renderer that had to remember to
     * add two fields is a renderer that will eventually add one.
     */
    val visualRotationDegrees: Float = 0f,
    /**
     * The catalog entry's own orientation, carried alongside the total so the
     * editor can offer "reset orientation" and can snap to the authored angle
     * rather than blindly to zero.
     */
    val authoredRotationDegrees: Float = 0f,
    /** Instances sharing this id are transformed together by an editor. */
    val editGroupId: String? = null,
    /** Logical-unit offset from the group's normalized anchor. */
    val groupOffsetXUnits: Float = 0f,
    val groupOffsetYUnits: Float = 0f,
    /**
     * The user's hold-to-latch choice for this control.
     *
     * Tri-state on purpose. `null` means "whatever the global setting says", so
     * changing that setting moves every control the user has not had an opinion
     * about — which is what a global setting is for. `true`/`false` are explicit
     * per-control answers that survive it.
     */
    val latch: Boolean? = null,
)

/**
 * What to call a control in words a person reads.
 *
 * ```text
 * a face binding      the letter it is DRAWN with   A, B, X, Y
 * an authored legend  that legend                   ZL, L3, GL, Z
 * anything else       its id, made readable         Stick left, Dpad
 * a second instance   the same name, numbered       B (2)
 * ```
 *
 * In the shared module, and used by BOTH the editor's labels and the audit's
 * messages, because those are the same claim made twice. Pro Controller 2's face
 * controls carry no authored legend — their letter is resolved at draw time —
 * so naming them from their ids produced "face-north", "face-east" and so on in
 * every sentence the user was shown. Cardinal slots are an internal way to keep
 * a position separate from the bit it sends; nobody has ever pressed a button
 * called Face East.
 */
object TouchControlNaming {

    /**
     * The presentation every drawn diamond uses.
     *
     * Fixed rather than chosen: every controller this surface can emulate is a
     * Nintendo one, and the retired Nintendo/Xbox mode is not coming back.
     */
    val FACE_LAYOUT = ControllerFaceLayout.Nintendo

    /**
     * [instanceId] contributes only the copy number. Two instances of the same
     * catalog entry are the same control to a user, so they get the same name —
     * but a message about one of them still has to say WHICH, and "B (2)" is
     * the readable form of the `b#2` the document actually stores.
     */
    fun nameFor(action: TouchControlAction?, label: String, instanceId: String): String {
        val base = when {
            action is TouchControlAction.Face ->
                ControllerLayoutResolver.faceLabel(action.position, FACE_LAYOUT)
            label.isNotBlank() -> label
            else -> instanceId.substringBefore('#')
                .replace('-', ' ')
                .replaceFirstChar { it.uppercase() }
        }
        val copy = instanceId.substringAfter('#', "")
        return if (copy.isBlank()) base else "$base ($copy)"
    }
}

/** What to call this control in a message or a label. See [TouchControlNaming]. */
val TouchControlSpec.displayName: String
    get() = TouchControlNaming.nameFor(action, label, id)

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
    val profileId: TouchProfileId? = null,
    val templateId: String? = null,
    val templateRevision: Int = 1,
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

    /**
     * Axis-aligned half-extents of the answerable region AFTER rotation.
     *
     * Not the same as [hitHalfWidth]/[hitHalfHeight] once a control is turned:
     * those are the control's own frame. Every screen-space question — is this
     * inside the safe rectangle, can these two possibly overlap, how far may
     * this be dragged — has to be asked in screen space, and asking it with the
     * unrotated extents is how a rotated control ends up half under a system
     * gesture strip.
     *
     * Computed per shape rather than with one conservative circumscribed radius,
     * because the GameCube beans are authored at a non-zero angle and a
     * needlessly loose bound there would report overlaps the shipped layout does
     * not have.
     */
    val hitExtentX: Float get() = rotatedExtents.first
    val hitExtentY: Float get() = rotatedExtents.second

    // Computed once per resolve, never per contact: geometry changes only when
    // the window or the layout does.
    private val rotatedExtents: Pair<Float, Float> = computeRotatedExtents()

    private fun computeRotatedExtents(): Pair<Float, Float> {
        val degrees = spec.visualRotationDegrees
        if (degrees == 0f && spec.shape != TouchControlShape.GameCubeContour) {
            return hitHalfWidth to hitHalfHeight
        }
        val radians = degrees.toDouble() * PI / 180.0
        val cosine = abs(cos(radians)).toFloat()
        val sine = abs(sin(radians)).toFloat()
        return when (spec.shape) {
            TouchControlShape.Rectangle ->
                (hitHalfWidth * cosine + hitHalfHeight * sine) to
                    (hitHalfWidth * sine + hitHalfHeight * cosine)
            TouchControlShape.Circle -> {
                val x = hitHalfWidth * cosine
                val y = hitHalfHeight * sine
                val u = hitHalfWidth * sine
                val v = hitHalfHeight * cosine
                sqrt(x * x + y * y) to sqrt(u * u + v * v)
            }
            // A bean is inscribed in its box and already rotated by the shared
            // contour helper, so its real extent comes from the contour itself.
            // The touch margin expands it in every direction.
            TouchControlShape.GameCubeContour -> {
                val margin = hitHalfWidth - halfWidth
                val points = TouchGameCubeGeometry.orientedContour(
                    role = spec.visualRole,
                    width = halfWidth * 2f,
                    height = halfHeight * 2f,
                    rotationDegrees = degrees,
                )
                val x = points.maxOfOrNull { abs(it.x) } ?: hitHalfWidth
                val y = points.maxOfOrNull { abs(it.y) } ?: hitHalfHeight
                (x + margin) to (y + margin)
            }
        }
    }

    /**
     * How far the control's axis-aligned box must be turned to describe its
     * screen-space region — which is NOT always [visualRotationDegrees].
     *
     * The distinction is whether the shape test rotates the POINT or whether the
     * shape itself already carries the angle:
     *
     * ```text
     * Rectangle          the box IS the region      -> turn it
     * Circle, round      rotation-invariant         -> do not
     * Circle, elliptical the box IS the region      -> turn it
     * GameCubeContour    the contour is rotated
     *                    INSIDE an upright box      -> do not
     * ```
     *
     * Here rather than in a renderer because a selection outline, a debug
     * overlay and any future chrome all have to agree with the router about
     * where a control actually is. Drawing a tilted box around an upright
     * bean — or around a circle, where it means nothing at all — is how an
     * editor starts lying about its own geometry.
     */
    val outlineRotationDegrees: Float get() = when (spec.shape) {
        TouchControlShape.Rectangle -> spec.visualRotationDegrees
        TouchControlShape.Circle ->
            if (hitHalfWidth == hitHalfHeight) 0f else spec.visualRotationDegrees
        TouchControlShape.GameCubeContour -> 0f
    }

    /**
     * Hit test in the control's own frame.
     *
     * A rotated control is tested by rotating the POINT backwards rather than by
     * building a rotated polygon: one inverse transform reuses the same local
     * shape test the unrotated case uses, so the two can never drift apart, and
     * nothing is allocated on the contact path.
     */
    fun hitTest(x: Float, y: Float): Boolean = contains(x, y, includeMargin = true)

    /**
     * The DRAWN shape, without the courtesy touch margin around it.
     *
     * Used by the layout audit to tell two genuinely colliding controls from two
     * whose margins merely meet; see [TouchLayoutAudit]. Never used for routing
     * a contact — a control answers to its margin, which is the whole point of
     * having one.
     */
    fun containsVisual(x: Float, y: Float): Boolean = contains(x, y, includeMargin = false)

    private fun contains(x: Float, y: Float, includeMargin: Boolean): Boolean {
        val halfW = if (includeMargin) hitHalfWidth else halfWidth
        val halfH = if (includeMargin) hitHalfHeight else halfHeight
        if (halfW <= 0f || halfH <= 0f) return false
        val dx = x - centerX
        val dy = y - centerY
        return when (spec.shape) {
            // Rotation-invariant when the two half-extents are equal, which is
            // every circular control in the shipped catalog. Rotating the point
            // anyway costs a sin/cos per contact for no change in the answer.
            TouchControlShape.Circle -> {
                val (lx, ly) = if (halfW == halfH) dx to dy else localPoint(dx, dy)
                val nx = lx / halfW
                val ny = ly / halfH
                nx * nx + ny * ny <= 1f
            }
            TouchControlShape.Rectangle -> {
                val (lx, ly) = localPoint(dx, dy)
                abs(lx) <= halfW && abs(ly) <= halfH
            }
            // The contour test does its own inverse rotation from the same total
            // angle, so the point arrives here unrotated. The contour is always
            // the DRAWN one; the margin is what the courtesy expansion adds.
            TouchControlShape.GameCubeContour -> TouchGameCubeGeometry.contains(
                role = spec.visualRole,
                x = dx,
                y = dy,
                width = halfWidth * 2f,
                height = halfHeight * 2f,
                rotationDegrees = spec.visualRotationDegrees,
                margin = if (includeMargin) hitHalfWidth - halfWidth else 0f,
            )
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
        if (hitHalfWidth <= 0f || hitHalfHeight <= 0f) return Float.MAX_VALUE
        val (lx, ly) = localPoint(x - centerX, y - centerY)
        return max(abs(lx / hitHalfWidth), abs(ly / hitHalfHeight))
    }

    /** A screen-space offset from the centre, expressed in the control's frame. */
    private fun localPoint(dx: Float, dy: Float): Pair<Float, Float> {
        val degrees = spec.visualRotationDegrees
        if (degrees == 0f) return dx to dy
        val radians = (-degrees).toDouble() * PI / 180.0
        val cosine = cos(radians).toFloat()
        val sine = sin(radians).toFloat()
        return (dx * cosine - dy * sine) to (dx * sine + dy * cosine)
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
    /**
     * The rectangle itself is below [TouchLayoutResolver.MIN_REGION_WIDTH_UNITS]
     * or [TouchLayoutResolver.MIN_REGION_HEIGHT_UNITS].
     *
     * Distinct from a merely failing audit because no EDIT can clear it: moving
     * or shrinking controls does not make the window bigger. A surface that
     * offers layout editing here would be offering a repair that cannot work.
     */
    val regionTooSmall: Boolean = false,
    /**
     * Everything the audit said about this exact geometry.
     *
     * Carried on the resolved layout rather than recomputed by whoever wants it,
     * so an editor highlighting a broken control and the runtime deciding
     * whether to play the layout are reading one answer. Recomputing invited the
     * failure where the canvas says a control fits and the validator refuses it.
     */
    val findings: List<TouchLayoutFinding> = emptyList(),
) {
    /** Built once so the router's per-move owner lookup is not a list scan. */
    private val byId: Map<String, ResolvedTouchControl> = controls.associateBy { it.id }

    fun control(id: String): ResolvedTouchControl? = byId[id]

    /**
     * The instances a blocking finding names, for a surface to mark as broken.
     *
     * Blocking only: a non-blocking finding is information, and painting a
     * control red for one would teach the user to ignore the colour.
     */
    val invalidControlIds: Set<String> = findings
        .filter { it.blocking }
        .flatMapTo(mutableSetOf()) { it.controlIds }

    /** The shortest true thing to say about [id], when it is broken. */
    fun problemFor(id: String): String? = findings
        .firstOrNull { it.blocking && id in it.controlIds }
        ?.message

    companion object {
        val Empty = ResolvedTouchLayout(
            layout = TouchLayout("empty", TouchLayoutV1.SCHEMA_VERSION, emptyList()),
            region = TouchLayoutRegion(0f, 0f, 0f, 0f, 1f),
            controls = emptyList(),
            scale = 1f,
            fits = false,
            problem = "No interaction area has been measured yet",
            regionTooSmall = true,
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

    fun resolve(
        layout: TouchLayout,
        region: TouchLayoutRegion,
        auditMode: TouchLayoutAuditMode = TouchLayoutAuditMode.Runtime,
    ): ResolvedTouchLayout {
        if (region.unitScale <= 0f || region.width <= 0f || region.height <= 0f) {
            return ResolvedTouchLayout(
                layout, region, emptyList(), 1f, fits = false,
                problem = "The interaction area has no usable size",
                regionTooSmall = true,
            )
        }

        val scale = min(
            region.widthUnits / REFERENCE_WIDTH_UNITS,
            region.heightUnits / REFERENCE_HEIGHT_UNITS,
        ).coerceIn(MIN_SCALE, MAX_SCALE)

        val controls = layout.controls.map { spec -> place(spec, region, scale) }

        val tooSmall = region.widthUnits < MIN_REGION_WIDTH_UNITS ||
            region.heightUnits < MIN_REGION_HEIGHT_UNITS
        val profile = layout.profileId?.let { TouchProfileCatalog.profiles[it] }
        val findings = when {
            layout.profileId != null && profile == null -> listOf(
                TouchLayoutFinding("No touch profile is registered for ${layout.profileId}", true),
            )
            profile != null -> TouchLayoutAudit.audit(layout, controls, region, profile, auditMode)
            else -> TouchLayoutAudit.audit(controls, region)
        }
        val problem = when {
            tooSmall -> "This window is too small for the on-screen controller"
            else -> findings.firstOrNull { it.blocking }?.message
        }

        return ResolvedTouchLayout(
            layout = layout,
            region = region,
            controls = controls,
            scale = scale,
            fits = problem == null,
            problem = problem,
            regionTooSmall = tooSmall,
            findings = findings,
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

        // Do not silently repair an out-of-bounds user override. The audit must
        // see the authored result and block it; otherwise the persisted geometry
        // says one thing while the control is drawn and hit-tested somewhere else.
        val centerX = region.left + spec.anchorX * region.width + spec.groupOffsetXUnits * unit
        val centerY = region.top + spec.anchorY * region.height + spec.groupOffsetYUnits * unit

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
