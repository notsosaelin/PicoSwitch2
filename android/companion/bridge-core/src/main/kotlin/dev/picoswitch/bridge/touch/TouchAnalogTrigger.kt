package dev.picoswitch.bridge.touch

import kotlin.math.abs
import kotlin.math.min
import kotlin.math.sqrt

/**
 * Where a trigger's invisible travel axis points, and how far along it the
 * finger has pulled.
 *
 * Pure geometry over the resolved layout: no state, no rendering, no contacts.
 * Everything the analog trigger gesture decides is one of these three answers,
 * so they are separable and pinned by `TouchAnalogTriggerGeometryTest`.
 *
 * ## The trigger is the handle; the screen is the travel space
 *
 * A touchscreen has no trigger travel, and the two obvious substitutes are both
 * wrong. Finger PRESSURE is not reported usefully by ordinary capacitive panels.
 * A visible SLIDER spends permanent gameplay screen space on a control that is
 * idle almost all the time, and stops looking like the trigger it represents.
 *
 * So travel is finger DISPLACEMENT after touching the trigger, projected onto an
 * invisible axis. The control stays a compact `L` or `R`; the travel space costs
 * nothing because it is only gesture space.
 *
 * ## Why the direction is derived and never declared
 *
 * The axis points from the control toward the middle of the playable rectangle,
 * so the gesture is always "pull it into the screen":
 *
 * ```text
 *   L .                       . R          top-left  -> down/right
 *      \                     /             top-right -> down/left
 *       v                   v              left edge -> right
 *              (centre)                    bottom    -> up
 *       ^                   ^
 *      /                     \
 *   L '                       ' R
 * ```
 *
 * Encoding "L means drag down" instead would be correct exactly once — for the
 * shipped layout — and wrong for every user who moves the control, which the
 * editor exists to let them do. Deriving it costs one subtraction and one
 * normalize per gesture and can never disagree with where the control is drawn.
 */
object TouchTriggerTravel {

    /**
     * The direction in which pulling increases travel, as a unit vector in the
     * layout's own coordinate space (`y` grows DOWNWARD, as the platform
     * reports it).
     *
     * ## Derived in NORMALIZED region space, not in pixels
     *
     * "Toward the middle of the playable rectangle" is a statement about the
     * LAYOUT, and the layout's own space is normalized — every control is placed
     * by a `0..1` anchor. Taking the direction in pixels instead makes it a
     * statement about the HANDSET: the same authored control produced an axis
     * 55 degrees off vertical on a 1920x1025 handheld, 51 on a 16:10 tablet and
     * 45 on a 4:3 one, purely because a wider window puts its centre further to
     * the right. The gesture changed shape per device while the layout did not.
     *
     * That was measured, not reasoned: on an Odin 2 Mini in landscape the
     * shipped GameCube `L` resolved to `(0.818, 0.575)`, and a thumb pulling
     * straight DOWN — the natural motion for a trigger at the top of the screen
     * — recovered only `0.575` of its travel while paying a full-travel distance
     * inflated by the horizontal budget it never spent. A full pull cost 985 px
     * of a 1025 px screen. Dividing by the region's own extents first gives
     * `(0.605, 0.796)` for that control on EVERY window shape, which both points
     * where the thumb is already going and stops the feel drifting between
     * devices.
     *
     * The epsilon below is still measured in real pixels, because "parked on the
     * middle of the screen" is a physical fact rather than a normalized one.
     *
     * [centerEpsilonUnits] guards the one degenerate case: a control parked
     * almost exactly on the middle of the rectangle has a near-zero inward
     * vector, which would normalize to noise and let the axis spin between
     * frames. There the axis becomes the inward normal of the NEAREST EDGE
     * instead — still position-derived, still stable, and for the shipped
     * top-of-screen triggers it agrees with the ordinary answer.
     *
     * Never returns a zero-length or non-finite vector. A region with no size
     * has no geometry to derive anything from and resolves to "downward", which
     * is arbitrary but deterministic; a layout in that state does not [fits] and
     * is not being played.
     */
    fun inwardAxis(
        centerX: Float,
        centerY: Float,
        region: TouchLayoutRegion,
        centerEpsilonUnits: Float,
    ): TouchVector {
        if (region.width <= 0f || region.height <= 0f ||
            !centerX.isFinite() || !centerY.isFinite()
        ) {
            return DEGENERATE_AXIS
        }
        val dx = (region.left + region.right) / 2f - centerX
        val dy = (region.top + region.bottom) / 2f - centerY
        val length = sqrt(dx * dx + dy * dy)
        val epsilon = centerEpsilonUnits * region.unitScale
        if (length > 0f && length >= epsilon) {
            // Normalized by the region's own extents, so the direction is the
            // layout's and not the window's; see the doc above.
            val nx = dx / region.width
            val ny = dy / region.height
            val normalized = sqrt(nx * nx + ny * ny)
            if (normalized > 0f) return TouchVector(nx / normalized, ny / normalized)
        }

        // Nearest edge, pointing inward. Ties resolve in a fixed order rather
        // than by whichever comparison happened to run first, so a control on an
        // exact centre line produces the same axis every time it is touched.
        val toTop = centerY - region.top
        val toBottom = region.bottom - centerY
        val toLeft = centerX - region.left
        val toRight = region.right - centerX
        val nearest = minOf(toTop, toBottom, toLeft, toRight)
        return when (nearest) {
            toTop -> TouchVector(0f, 1f)
            toBottom -> TouchVector(0f, -1f)
            toLeft -> TouchVector(1f, 0f)
            else -> TouchVector(-1f, 0f)
        }
    }

    /**
     * How far along [axis] the finger must travel for a full pull, in the
     * layout's real coordinates.
     *
     * ## Two budgets; the pull ends when either one is spent
     *
     * ```text
     *   horizontal budget   Rx = travelFraction * min(width, height)
     *   vertical budget     Ry = Rx * verticalTravelRatio
     *
     *   fullTravel(axis) = min( Rx / |axis.x| , Ry / |axis.y| )
     * ```
     *
     * The guarantee that buys, and the reason it is stated this way: a full pull
     * NEVER displaces the finger by more than `Ry` vertically or more than `Rx`
     * horizontally, whatever direction the axis points. The vertical cost of the
     * gesture is bounded by construction rather than by hoping the axis stays
     * near one of the two pure cases.
     *
     * ## What this replaced, and why
     *
     * A single shared distance came first (`min(width, height) * 0.50` for every
     * direction), and hardware feel testing rejected it: the same pixels are a
     * quarter of the width but half of the height, so near-vertical pulls had to
     * be dragged the whole way down the glass.
     *
     * The repair was a weighted BLEND, `|axis.x| * Rx + |axis.y| * Ry`, and
     * device measurement rejected that too. It charges the horizontal budget in
     * proportion to how much of the AXIS lies along X — but a thumb pulling a
     * top-placed trigger moves DOWN, spending no width at all, and still paid
     * for it. Measured on an Odin 2 Mini in landscape (region 1920x1025, shipped
     * `L`, axis `(0.818, 0.575)`, `Rx = 512.5`, `Ry = 256.25`), full travel came
     * to 566 px, and a straight-down stroke needed `566 / 0.575 = 985` px — 96%
     * of the usable height, which is exactly the reported symptom:
     *
     * ```text
     *   dy for a downward stroke = Rx * tan(tilt from vertical) + Ry
     *                            =      729 px                  + 256 px
     * ```
     *
     * The `Rx * tan(...)` term is the defect: a budget spent on motion that
     * never happened. Taking the MINIMUM removes it — the horizontal budget can
     * only ever end the pull EARLY, never lengthen it.
     *
     * ## Behaviour at the edges
     *
     * A purely horizontal axis still resolves to exactly `Rx` and a purely
     * vertical one to exactly `Ry`, unchanged on every region, so the placements
     * that already felt right did not move. Between them the two branches cross
     * once, where `|axis.x| / |axis.y| == Rx / Ry`; the value is continuous
     * there and only its slope has a corner, which is a far milder thing than
     * the sensitivity cliff a "is this vertical or horizontal" branch would put
     * in the middle of the placements the editor most encourages.
     *
     * Both budgets are scaled by the region's SHORTER side rather than by each
     * axis's own extent, so neither drifts with the handset's aspect ratio: half
     * the WIDTH of a 20:9 panel is a swipe longer than the screen is tall.
     *
     * Named fractions so a future travel setting has one place to change; see
     * [TouchTriggerConfig.travelFraction] and
     * [TouchTriggerConfig.verticalTravelRatio].
     */
    fun fullTravelPx(
        region: TouchLayoutRegion,
        axis: TouchVector,
        travelFraction: Float,
        verticalTravelRatio: Float,
    ): Float {
        val horizontal = min(region.width, region.height) * travelFraction
        if (!horizontal.isFinite() || horizontal <= 0f) return 0f
        val vertical = horizontal * verticalTravelRatio
        val alongX = abs(axis.x)
        val alongY = abs(axis.y)
        // A component of zero cannot exhaust its budget at any distance, so it
        // simply does not bound the pull. Both being zero is not a direction at
        // all; the horizontal budget is the deterministic answer there.
        val byWidth = if (alongX > 0f) horizontal / alongX else Float.POSITIVE_INFINITY
        val byHeight = if (alongY > 0f) vertical / alongY else Float.POSITIVE_INFINITY
        val travel = min(byWidth, byHeight)
        return if (travel.isFinite()) travel else horizontal
    }

    /**
     * How far the contact has pulled along [axis], in the same coordinates.
     *
     * The vector PROJECTION, not the straight-line distance, and that is what
     * makes the invisible axis usable: a thumb sweeps an arc rather than a line,
     * so perpendicular drift has to cost nothing. Motion opposite the axis is
     * clamped away rather than allowed to go negative — backing out returns the
     * trigger toward rest and never past it.
     */
    fun projectedTravelPx(dx: Float, dy: Float, axis: TouchVector): Float {
        if (!dx.isFinite() || !dy.isFinite()) return 0f
        val projected = dx * axis.x + dy * axis.y
        return if (projected > 0f) projected else 0f
    }

    /** [projectedTravelPx] as a `0..1` trigger value. */
    fun analogValue(dx: Float, dy: Float, axis: TouchVector, fullTravelPx: Float): Float {
        if (!fullTravelPx.isFinite() || fullTravelPx <= 0f) return 0f
        return (projectedTravelPx(dx, dy, axis) / fullTravelPx).coerceIn(0f, 1f)
    }

    /**
     * The cardinal direction [vector] leans toward.
     *
     * Four answers rather than a rotated diagonal bar: the fill is drawn inside
     * a small pad whose silhouette is the control's own, and a diagonal wipe
     * across a rounded rectangle reads as a shading artefact rather than as a
     * level.
     *
     * Dominance is the larger component, with the vertical winning an exact tie.
     * Resolving that tie in a stated order rather than by whichever comparison
     * ran first is what keeps a perfectly diagonal input from picking a
     * different answer twice running.
     *
     * ## What to pass, and why it is not the travel axis
     *
     * The vector is the DISPLACEMENT of the swipe in progress, once there is
     * one — see [TouchAnalogTriggerState.swipeFill]. It is deliberately NOT the
     * inward axis, even though that is what the value is projected onto, because
     * the two answer different questions and a diagonal axis makes them
     * disagree. The shipped GameCube `R` has axis `(-0.732, +0.681)`: a straight
     * DOWNWARD swipe projects positively onto it and correctly increases travel,
     * but the axis leans horizontally, so filling from the axis showed a bar
     * growing leftward while the thumb moved down.
     *
     * ```text
     *   value  <- projection onto the inward axis   (where the trigger is)
     *   fill   <- cardinal of the actual swipe      (where the thumb went)
     * ```
     *
     * The inward axis remains the right answer for a control that is NOT being
     * swiped — at rest, or during the part of a press before the slop is
     * crossed — because then there is no swipe to read and the axis is the only
     * statement available about which way this trigger pulls.
     */
    fun fillDirection(vector: TouchVector): TouchFillDirection = when {
        abs(vector.y) >= abs(vector.x) -> if (vector.y >= 0f) {
            TouchFillDirection.Down
        } else {
            TouchFillDirection.Up
        }
        vector.x >= 0f -> TouchFillDirection.Right
        else -> TouchFillDirection.Left
    }

    private val DEGENERATE_AXIS = TouchVector(0f, 1f)
}

/**
 * Which edge of a trigger's pad its fill grows from, as a cardinal direction.
 *
 * [Down] means the fill starts at the top edge and grows downward — the
 * direction the finger travels, so the pad empties toward where the thumb came
 * from and fills toward where it is going.
 *
 * Chosen by [TouchTriggerTravel.fillDirection] from the swipe the user is
 * actually making, and never from a control's identity, so a trigger moved in
 * the editor re-presents itself with nothing to configure.
 */
enum class TouchFillDirection { Down, Up, Right, Left }

/**
 * One analog trigger's live gesture, in the engine's ownership.
 *
 * ## What the finger is doing, and what is published, are different questions
 *
 * ```text
 * pressed, not yet moved       PendingTap    publishes nothing
 * moved past the drag slop     AnalogDrag    publishes the projection; fixes the fill
 * still, past the hold base    a full pull   publishes full travel + detent
 * still, but DEFINING a hold   a selection   publishes nothing until it slides
 * that selection, timed out    a full pull   publishes full travel; arm consumed
 * released without dragging    a tap         publishes a brief full pulse
 * nothing touching it          at rest       publishes the LATCHED level, if any
 * ```
 *
 * ## A press that is choosing a level is not a press that is holding one
 *
 * Those two are the same motion for the first third of a second, and resolving
 * the wrong one first is observable on the wire:
 *
 * ```text
 *   tap, press, hold, slide to 40%, release      <- the user's gesture
 *
 *   wrong:  ... 0 -> 1.0 + DETENT -> 0.9 -> ... -> 0.4      held at 0.4
 *   right:  ... 0 ----------------> 0.1 -> ... -> 0.4      held at 0.4
 * ```
 *
 * The excursion in the first line is not cosmetic. On this personality full
 * travel IS the terminal click, and a GameCube game can act on the click and on
 * partial travel differently — so a 40% hold that fires a click on its way there
 * has sent an input the user never made.
 *
 * So a press the recognizer has already accepted as a hold CANDIDATE — the
 * second press of a double tap — starts with no full-pull resolve pending at
 * all, and gets one only once the gesture ARMS, by which point the slide that
 * chooses the level is available and the arming tick has said so. See [onDown]
 * and [armLatchSelection].
 *
 * The fallback that ends that window is a DECISION, not a timer that merely
 * published something:
 *
 * ```text
 *   armed ---- slides in time ----> selecting -> release commits the level
 *         \
 *          `-- window expires ----> ordinary held trigger, arm consumed
 * ```
 *
 * Once the second branch is taken it is final for this contact. The press has
 * been answered as "you are holding the trigger down", and a slide made after
 * that answer moves the live value like any other pull but can no longer lock
 * anything: a persistent hold must not be reachable through a gesture the
 * recognizer already resolved as something else. See [resolveFullPull].
 *
 * ## Nothing is published on the way down
 *
 * The single most damaging thing this control could do is assert a full pull the
 * instant it is touched, because on the GameCube personality full travel IS the
 * terminal click: every deliberate analog pull would fire the detent before the
 * gesture had said anything at all. So a fresh contact publishes NOTHING, and
 * the press resolves later, one of three ways — it becomes a drag, it stays
 * still long enough to be a deliberate full pull, or it ends as a tap.
 *
 * A tap therefore lands on RELEASE. That is a real cost in feel and it is the
 * side of the trade the spec picks deliberately: a late tap is worse input, a
 * speculative click is WRONG input.
 *
 * ## Why a tap has to last
 *
 * A press and a release published in the same instant collapse to no change at
 * all — the session coalesces onto a 125 Hz report cadence through a conflating
 * mailbox. So a tap holds full travel for a pulse window before returning, for
 * exactly the reason `TouchControlLatch`'s retrigger does, and reuses that same
 * duration.
 *
 * ## The hold is the shared one
 *
 * There is no second latch system here. [TouchControlLatch] decides WHETHER this
 * control is held, using the same tap/dwell/slide gesture every digital control
 * uses; this class only remembers at what LEVEL, which is the one thing a
 * Boolean cannot carry. The specialization is a single line in the recognizer:
 * an analog trigger measures its commit slide along its own inward axis, so a
 * sideways or outward slide cannot lock a trigger to nothing.
 */
internal class TouchAnalogTriggerState {

    /** The contact that owns this trigger, or [NO_POINTER]. */
    var pointerId: Long = NO_POINTER
        private set

    /** Where that contact began, so displacement is measured from its own origin. */
    private var originX = 0f
    private var originY = 0f

    /** When it began, in the host's contact clock; zero when nothing is pressed. */
    private var pressStartNanos = 0L

    /**
     * The travel axis, frozen for the whole gesture.
     *
     * Recomputed per gesture and never mid-gesture: recomposition, an animated
     * inset or a window resize would otherwise rotate the axis under a thumb
     * that is already pulling, and the trigger would move on its own. The NEXT
     * gesture picks up wherever the control now is.
     */
    var axis: TouchVector = DEFAULT_AXIS
        private set

    var fullTravelPx = 0f
        private set

    /** The contact crossed the drag slop; permanent for this gesture. */
    var dragging = false
        private set

    /**
     * Which way this gesture's swipe went, or null before there is a swipe.
     *
     * The picture's direction, and deliberately a different question from
     * [axis], which is the direction that increases the VALUE. On a diagonal
     * axis the two disagree, and the user is right: a thumb that moved down
     * expects the pad to fill down, whatever the projection arithmetic is doing
     * underneath. See [TouchTriggerTravel.fillDirection].
     *
     * Established EXACTLY ONCE, at the moment the contact crosses the same drag
     * slop that turns the press into a pull, from the whole displacement since
     * pointer-down. Frozen from then on: a thumb sweeps an arc, and re-reading
     * it per frame would flip the bar between down and sideways in the middle of
     * a pull. Cleared with the contact, so the next gesture decides again.
     *
     * Presentation only. Nothing here reaches travel, the detent, the latch or
     * ownership.
     */
    var swipeFill: TouchFillDirection? = null
        private set

    /** The press began on a control that was already held. */
    var startedLatched = false
        private set

    /**
     * This press is the second half of a double tap, so it is a candidate for
     * DEFINING a hold rather than for being one.
     *
     * The one flag that separates the two things a still press on an unlatched
     * trigger can mean; see [onDown] for why the difference has to be honoured
     * before either of them publishes anything.
     */
    var latchSelecting = false
        private set

    /** A still press was resolved into a deliberate full pull. */
    private var resolvedFull = false

    /** When a still press becomes a full pull; zero when nothing is waiting. */
    var holdResolveDeadlineNanos = 0L
        private set

    /** What the finger is asking for right now, when it is asking for anything. */
    var physicalValue = 0f
        private set
    var physicalDetent = false
        private set

    /** Whether the finger currently overrides the held level. */
    val physicalActive: Boolean get() = dragging || resolvedFull

    /** What a tap publishes, and until when; zero deadline when idle. */
    var pulseValue = 0f
        private set
    var pulseDeadlineNanos = 0L
        private set
    val pulsing: Boolean get() = pulseDeadlineNanos != 0L

    /**
     * The level this control holds when a latch is holding it.
     *
     * Meaningful only while [TouchControlLatch.latched] — that recognizer, not
     * this field, is what decides whether a hold exists. Cleared whenever the
     * latch is, so a stale level can never be republished by a later hold.
     */
    var latchedValue = 0f
        private set
    var latchedDetent = false
        private set

    /** This contact performed the gesture that created the current hold. */
    private var committedByThisContact = false

    // ------------------------------------------------------------------ gesture

    /**
     * A contact claimed the trigger. Publishes nothing; see the class doc.
     *
     * [latchSelecting] is what stops a partial hold passing through a full
     * trigger on its way to the level the user meant. A press that is the second
     * half of a double tap is on its way to CHOOSING a level, and resolving it
     * into a full pull first would put a `1.0` and the terminal detent on the
     * wire before the slide had said anything — on the GameCube personality that
     * is the click itself, which is a gameplay action with effects a partial
     * pull does not have. So that press starts with no resolve pending at all,
     * and the recognizer re-arms one through [armLatchSelection] once the slide
     * that would use it is actually available.
     *
     * An ordinary press — no leading tap, so no hold to define — is untouched
     * and still becomes a full pull after [holdResolveNanos].
     */
    fun onDown(
        contact: TouchContact,
        control: ResolvedTouchControl,
        region: TouchLayoutRegion,
        latched: Boolean,
        latchSelecting: Boolean,
        config: TouchTriggerConfig,
        holdResolveNanos: Long,
    ) {
        pointerId = contact.id
        originX = contact.x
        originY = contact.y
        pressStartNanos = contact.timeNanos
        axis = TouchTriggerTravel.inwardAxis(
            control.centerX,
            control.centerY,
            region,
            config.centerEpsilonUnits,
        )
        fullTravelPx = TouchTriggerTravel.fullTravelPx(
            region,
            axis,
            config.travelFraction,
            config.verticalTravelRatio,
        )
        dragging = false
        swipeFill = null
        resolvedFull = false
        committedByThisContact = false
        startedLatched = latched
        this.latchSelecting = latchSelecting
        physicalValue = 0f
        physicalDetent = false
        // A new press supersedes the previous one's tail: the finger is back on
        // the control, so the pulse has already said what it had to say.
        pulseDeadlineNanos = 0L
        // Suppressed on a latched control, where a still press is already the
        // gesture that REMOVES the hold and must not also mean "pull it fully",
        // and on a latch-defining press, which is on its way to selecting a
        // level and must not publish one it did not select.
        holdResolveDeadlineNanos =
            if (!latched && !latchSelecting && contact.timeNanos > 0L) {
                contact.timeNanos + holdResolveNanos
            } else {
                0L
            }
    }

    /**
     * The hold gesture armed on this contact: a slide from here selects a level.
     *
     * Restarts the deliberate-hold wait that [onDown] deliberately did not, so
     * the two things a still press can mean stay ORDERED rather than raced. The
     * user has just been told, by the arming tick, that a slide will now choose
     * a value; if they take it, the value they see is the one they chose, and
     * nothing full ever reached the wire. If they do not, the press was an
     * ordinary "tap it, then keep holding it" after all and resolves into a full
     * pull one base later — late, but the same answer, and no press that stays
     * down forever is left publishing nothing.
     *
     * Ignored once the contact is already saying something: a pull in progress
     * owns the value, and a resolve on top of it would snap the trigger to full
     * out from under a thumb that is mid-slide.
     */
    fun armLatchSelection(nowNanos: Long, holdResolveNanos: Long) {
        // [latchSelecting] is the guard, not a note: only the press that
        // WITHHELD a resolve may be given one, so no path can hand a second
        // deadline to a press that already has one running.
        if (!latchSelecting || pointerId == NO_POINTER || dragging || resolvedFull ||
            nowNanos <= 0L
        ) {
            return
        }
        holdResolveDeadlineNanos = nowNanos + holdResolveNanos
    }

    /**
     * The contact moved. Returns the travel that counts toward committing a
     * hold, which is the projection and not the raw distance.
     *
     * Ownership is unconditional once [dragging]: leaving the visible trigger,
     * drifting sideways, or wandering back over a neighbour changes nothing. The
     * control is a handle on a travel surface half the screen across, so bounds
     * have no part in it.
     */
    fun onMove(contact: TouchContact, config: TouchTriggerConfig, slopPx: Float): Float {
        if (pointerId != contact.id) return 0f
        val dx = contact.x - originX
        val dy = contact.y - originY
        if (!dragging) {
            if (dx * dx + dy * dy <= slopPx * slopPx) return 0f
            dragging = true
            // The press turned out to be a pull, so it is no longer a candidate
            // for either of the two things a still press can become.
            holdResolveDeadlineNanos = 0L
            // And the swipe has now declared which way it went. Read here rather
            // than on the first pixel of movement because below the slop the
            // direction is jitter, and read only here because after this the
            // thumb is arcing.
            swipeFill = TouchTriggerTravel.fillDirection(TouchVector(dx, dy))
        }
        val value = TouchTriggerTravel.analogValue(dx, dy, axis, fullTravelPx)
        physicalValue = value
        physicalDetent = detentWithHysteresis(value, physicalDetent, config)
        return TouchTriggerTravel.projectedTravelPx(dx, dy, axis)
    }

    /**
     * The still press outlasted the deliberate-hold base: it is a full pull.
     *
     * Returns true when this press was a latch CANDIDATE that has just lost it.
     * The fallback is a state transition and not merely an output: a press that
     * has been answered as an ordinary held trigger must not still be able to
     * lock a partial hold if the finger moves afterwards, because that hold is
     * persistent state reached through a gesture already resolved as something
     * else. The caller consumes the recognizer's arm on a true return; see
     * [TouchControlLatch.abandonArm].
     *
     * Whatever the finger does next behaves like any other live pull — the value
     * follows it, and lifting off simply ends the press.
     */
    fun resolveFullPull(): Boolean {
        holdResolveDeadlineNanos = 0L
        resolvedFull = true
        physicalValue = 1f
        physicalDetent = true
        val wasSelecting = latchSelecting
        latchSelecting = false
        return wasSelecting
    }

    /**
     * The latch gesture committed on this contact. Records the level so a
     * cancelled contact still leaves a sensible hold behind.
     */
    fun commitLatch() {
        committedByThisContact = true
        latchedValue = physicalValue
        latchedDetent = physicalDetent
    }

    /**
     * A contact ended. Returns true when the release should publish a tap pulse.
     *
     * A tap is a press that never became a pull, never resolved into a full one,
     * did not remove the hold it started on, and was short enough to be a tap at
     * all. The last clause is what stops a long still press — an abandoned latch
     * attempt, say — ending in a full trigger click nobody asked for.
     */
    fun onEnd(
        contact: TouchContact,
        cancelled: Boolean,
        /** Whether a hold is on the control NOW, after whatever this press did. */
        latched: Boolean,
        maxTapDurationNanos: Long,
        pulseNanos: Long,
    ): Boolean {
        val duration = contact.timeNanos - pressStartNanos
        val removedTheHold = startedLatched && !latched
        val tapped = !cancelled && !dragging && !resolvedFull && !removedTheHold &&
            contact.timeNanos > 0L && pressStartNanos > 0L && duration in 0..maxTapDurationNanos

        if (committedByThisContact && !cancelled) {
            // The value at RELEASE is the held level: the whole point of the
            // slide is that the user is choosing it while they can see it.
            latchedValue = physicalValue
            latchedDetent = physicalDetent
        }
        if (!latched) {
            latchedValue = 0f
            latchedDetent = false
        }

        pointerId = NO_POINTER
        pressStartNanos = 0L
        dragging = false
        swipeFill = null
        resolvedFull = false
        committedByThisContact = false
        startedLatched = false
        latchSelecting = false
        holdResolveDeadlineNanos = 0L
        physicalValue = 0f
        physicalDetent = false

        if (!tapped) return false
        // A hold already at full travel cannot be re-fired by pulling harder, so
        // the pulse becomes a RELEASE edge instead, exactly as the digital
        // retrigger mask does. Either way the console sees an edge.
        pulseValue = if (latched && latchedDetent) 0f else 1f
        pulseDeadlineNanos = contact.timeNanos + pulseNanos
        return true
    }

    /** The pulse window expired. */
    fun endPulse() {
        pulseDeadlineNanos = 0L
    }

    /** The hold was removed by any path; the level it carried goes with it. */
    fun clearLatch() {
        latchedValue = 0f
        latchedDetent = false
        committedByThisContact = false
    }

    /** The earliest timed transition this trigger is waiting for, if any. */
    fun nextDeadlineNanos(): Long? = when {
        holdResolveDeadlineNanos == 0L -> pulseDeadlineNanos.takeIf { it != 0L }
        pulseDeadlineNanos == 0L -> holdResolveDeadlineNanos
        else -> min(holdResolveDeadlineNanos, pulseDeadlineNanos)
    }

    // ----------------------------------------------------------------- publish

    /**
     * What this trigger publishes right now, given whether a latch holds it.
     *
     * Priority is pulse, then finger, then hold. A finger temporarily overriding
     * a hold is the point of allowing both: a trigger held at 55% is still a
     * trigger, and pulling it to 85% has to reach the console while the thumb is
     * down and go back to 55% when it lifts.
     */
    fun effectiveValue(latched: Boolean): Float = when {
        pulsing -> pulseValue
        physicalActive -> physicalValue
        latched -> latchedValue
        else -> 0f
    }

    fun effectiveDetent(latched: Boolean, config: TouchTriggerConfig): Boolean = when {
        pulsing -> pulseValue >= config.detentEngageFraction
        physicalActive -> physicalDetent
        latched -> latchedDetent
        else -> false
    }

    companion object {
        const val NO_POINTER = -1L
        private val DEFAULT_AXIS = TouchVector(0f, 1f)

        /**
         * The detent, with the chatter guard the wire cannot supply.
         *
         * Equal thresholds would flicker the terminal click while a thumb sits
         * on the boundary, and on this personality that click is a gameplay
         * button.
         */
        fun detentWithHysteresis(
            value: Float,
            engaged: Boolean,
            config: TouchTriggerConfig,
        ): Boolean =
            if (engaged) value > config.detentReleaseFraction else value >= config.detentEngageFraction
    }
}
