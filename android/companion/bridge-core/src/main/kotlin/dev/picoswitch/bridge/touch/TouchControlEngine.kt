package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.DpadState
import dev.picoswitch.bridge.core.FaceButtonPosition
import dev.picoswitch.bridge.core.TouchContribution

/**
 * Live picture of what the on-screen controller is doing, for diagnostics.
 *
 * Counters and last-known values only. Nothing here is written per contact
 * move beyond incrementing an int, because the failure this feature is most
 * likely to have is a stall, and a diagnostic that samples at the contact rate
 * would be the thing causing it.
 */
data class TouchDiagnosticsSnapshot(
    val ownedControls: Int = 0,
    val activeContacts: Int = 0,
    val contactsClaimed: Long = 0,
    val contactsUnclaimed: Long = 0,
    val contactsContested: Long = 0,
    val contactsCancelled: Long = 0,
    val releaseAllCount: Long = 0,
    val lastReleaseReason: TouchReleaseReason? = null,
    /** Controls currently held by a latch rather than by a finger. */
    val latchedControls: Set<String> = emptySet(),
    /** Controls whose engage gesture is armed and awaiting a slide. */
    val armedControls: Set<String> = emptySet(),
    val latchesArmed: Long = 0,
    val latchesEngaged: Long = 0,
    val latchesReleased: Long = 0,
    val latchesCleared: Long = 0,
    /** Taps on an already-latched control that produced a fresh press edge. */
    val retriggerPulses: Long = 0,
    val lastContactTimeNanos: Long = 0,
    val leftStick: TouchVector = TouchVector.Zero,
    val rightStick: TouchVector = TouchVector.Zero,
    val dpad: DpadState = DpadState.None,
)

/**
 * The on-screen controller itself: contact ownership, control state, and one
 * coherent contribution per event.
 *
 * ## Ownership
 *
 * ```text
 * contact id  ->  control id      (one control per contact)
 * control id  ->  contact id      (one contact per control)
 * ```
 *
 * Keyed by the contact's STABLE id, never by its position in whatever array the
 * platform delivered. That distinction is invisible with two fingers and is the
 * single most common way an on-screen controller breaks: platforms are free to
 * reorder contacts between events, so an implementation keyed on the index
 * silently swaps which control a thumb is holding the moment a third contact
 * arrives or the first one lifts.
 *
 * A claim is made once, on Down, against the resolved layout. After that the
 * contact belongs to that control until it ends. In particular a stick keeps its
 * contact when the thumb leaves the visual circle — the stick clamps to full
 * deflection instead — because the alternative is the thumb wandering into a
 * face button mid-turn.
 *
 * A second contact landing on an already-owned control is ignored rather than
 * stealing it: two contradictory positions for one stick have no correct answer,
 * and silently taking the newest would make a resting palm beat a deliberate
 * thumb. Independent buttons each take their own contact, so chords are ordinary.
 *
 * ## Holding without a finger
 *
 * A digital control may be double-tapped-AND-HELD into a persistent hold, after
 * which
 *
 * ```text
 * effectivePressed = (touchPressed || latchedPressed) && !retriggering
 * ```
 *
 * ```text
 * UNLATCHED   tap                                  ordinary press
 *             tap, press, hold 2x the base         armed; STILL an ordinary press
 *             tap, press, hold, slide away         latch
 *
 * LATCHED     quick tap                            retrigger: release edge, then held again
 *             press held 1x the base               unlatch
 * ```
 *
 * The dwell only ARMS the engage gesture; a deliberate slide commits it, because
 * timing alone cannot be told apart from the ordinary "double tap, then keep
 * holding" a game may ask for. Creating a hold is deliberately the harder half
 * of the pair. See [TouchControlLatch].
 *
 * The recognizer OBSERVES presses that have already been applied, so it cannot
 * delay or swallow one; see [TouchControlLatch]. Ownership is unchanged — a
 * latched control still takes and releases contacts normally — and every global
 * release drops latches with everything else, because a hold nothing is touching
 * is exactly the state that must not survive a boundary.
 *
 * ## Time
 *
 * Two parts of that are TIMED rather than event-driven: the deliberate dwell
 * that completes a latch gesture, and the brief mask that gives a retrigger its
 * release edge. A still finger produces no events, so the engine cannot discover
 * either on its own.
 *
 * It does not own a clock either. [nextDeadlineNanos] states when it next has
 * work, the host sleeps until then and calls [onTick]. A pull model rather than
 * a scheduler on purpose: there is no queued closure that could carry stale
 * state across a teardown, so a tick that arrives after [releaseAll] finds
 * nothing to do and is a no-op. That is the whole of the retrigger's
 * session-safety.
 *
 * ## Publishing
 *
 * One [TouchContribution] per event, computed after every change that event
 * caused. The sink sees a complete controller, never a half-applied one.
 *
 * ## Threading
 *
 * Not thread-safe, by the same rule as the state machine it feeds: drive it from
 * the one thread the host delivers contacts on.
 */
class TouchControlEngine(
    config: TouchControlConfig = TouchControlConfig.Default,
    private val onContribution: (TouchContribution) -> Unit,
    private var feedback: TouchFeedbackBackend = TouchFeedbackBackend.None,
    private var latchObserver: TouchLatchObserver = TouchLatchObserver.None,
) {
    var config: TouchControlConfig = config
        private set

    private var layout: ResolvedTouchLayout = ResolvedTouchLayout.Empty

    private val contactToControl = mutableMapOf<Long, String>()
    private val controlToContact = mutableMapOf<String, Long>()

    /**
     * Per-control latch state and tap recognition, created on first tap.
     *
     * Bounded by the layout's control count and cleared at every boundary that
     * clears input, so it cannot accumulate across sessions or personalities.
     */
    private val latches = mutableMapOf<String, TouchControlLatch>()

    private val facePresses = mutableSetOf<FaceButtonPosition>()
    private val logicalPresses = mutableSetOf<ControllerButton>()
    private var leftStick = TouchVector.Zero
    private var rightStick = TouchVector.Zero
    private var leftTrigger = 0f
    private var rightTrigger = 0f
    private var dpad = DpadState.None

    private var published = TouchContribution.Neutral

    private var contactsClaimed = 0L
    private var contactsUnclaimed = 0L
    private var contactsContested = 0L
    private var contactsCancelled = 0L
    private var releaseAllCount = 0L
    private var lastReleaseReason: TouchReleaseReason? = null
    private var lastContactTimeNanos = 0L
    private var latchesArmed = 0L
    private var latchesEngaged = 0L
    private var latchesReleased = 0L
    private var latchesCleared = 0L
    private var retriggerPulses = 0L

    /** What touch is holding right now, independent of whether it is authoritative. */
    val contribution: TouchContribution get() = published

    fun setFeedbackBackend(backend: TouchFeedbackBackend) {
        feedback = backend
    }

    fun setLatchObserver(observer: TouchLatchObserver) {
        latchObserver = observer
    }

    /**
     * Retune the engine.
     *
     * Changing the LATCH configuration drops whatever is currently latched. A
     * user who has just turned hold-to-latch off means the button that is
     * stuck down, and a window where the setting says off while a control is
     * still held would be the exact confusion the setting exists to end.
     */
    fun setConfig(next: TouchControlConfig) {
        val latchChanged = next.latch != config.latch
        config = next
        if (latchChanged) clearLatches(TouchReleaseReason.SettingsChanged)
    }

    /**
     * Point the engine at freshly resolved geometry.
     *
     * Always releases first. Every retained contact position was measured
     * against the old rectangle, so after a rotation or a window resize the
     * engine's idea of where a thumb is has no relationship to where the control
     * now is.
     */
    fun setLayout(resolved: ResolvedTouchLayout) {
        releaseAll(TouchReleaseReason.GeometryInvalidated)
        installLayout(resolved)
    }

    /** Install geometry after [TouchContactTracker] has released and quarantined contacts. */
    internal fun installLayout(resolved: ResolvedTouchLayout) {
        layout = resolved
    }

    val resolvedLayout: ResolvedTouchLayout get() = layout

    /**
     * Handle one contact event.
     *
     * Returns true when the event was consumed by a control, so a platform
     * adapter can decide whether to let it continue to whatever is behind.
     */
    fun onContact(contact: TouchContact): Boolean {
        lastContactTimeNanos = contact.timeNanos
        return when (contact.phase) {
            TouchPhase.Down -> onDown(contact)
            TouchPhase.Move -> onMove(contact)
            TouchPhase.Up -> onEnd(contact, cancelled = false)
            TouchPhase.Cancel -> onEnd(contact, cancelled = true)
        }
    }

    private fun onDown(contact: TouchContact): Boolean {
        if (!layout.fits) {
            contactsUnclaimed++
            return false
        }
        val target = hitTest(contact.x, contact.y)
        if (target == null) {
            contactsUnclaimed++
            return false
        }
        if (controlToContact.containsKey(target.id)) {
            // Exclusivity: the control already has an owner. Not an error, and
            // not a steal -- the second thumb simply does nothing here.
            contactsContested++
            return false
        }
        contactToControl[contact.id] = target.id
        controlToContact[target.id] = contact.id
        contactsClaimed++
        engage(target, contact.x, contact.y)
        // AFTER the press has been applied, so a gesture can only ever change
        // what happens LATER, never what this press itself sends.
        noteLatchDown(target, contact.timeNanos, contact.x, contact.y)
        publish()
        return true
    }

    private fun onMove(contact: TouchContact): Boolean {
        val controlId = contactToControl[contact.id] ?: return false
        val control = layout.control(controlId) ?: return false
        // Only the vector controls track movement. A held button stays held for
        // as long as its contact lives, including outside its own bounds: lifting
        // the thumb is how a button is released, not sliding off it.
        when (control.spec.action) {
            is TouchControlAction.Stick, is TouchControlAction.Directions -> {
                engage(control, contact.x, contact.y)
                publish()
            }
            // A button's movement means one of exactly two things: a drag that
            // abandons a pending dwell, or the slide that commits an armed one.
            // Neither changes what is published — the control is already pressed
            // by this very contact — so nothing is published here.
            else -> if (latches[controlId]?.onMove(
                    contact.x,
                    contact.y,
                    gestureSlop(),
                    latchCommitDistance(),
                ) == true
            ) {
                commitLatch(controlId)
            }
        }
        return true
    }

    private fun onEnd(contact: TouchContact, cancelled: Boolean): Boolean {
        if (cancelled) contactsCancelled++
        val controlId = contactToControl.remove(contact.id) ?: return false
        controlToContact.remove(controlId)
        val control = layout.control(controlId)
        if (control != null) {
            noteLatchEnd(control, contact.timeNanos, cancelled)
            // A latched control keeps its value when the finger leaves; that is
            // the entire feature, so nothing is disengaged.
            if (!latches[controlId].isLatched) disengage(control)
            if (!cancelled) {
                // Buttons only. A stick or D-pad release is the end of a
                // continuous gesture, and buzzing there turns ordinary play into
                // a rattle.
                //
                // Fired whether or not anything was disengaged: lifting off a
                // held button is still a release the user performed, and a
                // latched control that felt dead to the touch would be the
                // clearest possible way to say "this control is broken now".
                if (control.spec.kind != TouchControlKind.Stick &&
                    control.spec.kind != TouchControlKind.Dpad
                ) {
                    feedback.perform(TouchFeedbackEvent.Release)
                }
            }
        }
        publish()
        return true
    }

    // -------------------------------------------------------------------- latch

    /**
     * Whether this control may be locked into a hold at all.
     *
     * Two gates that answer different questions: [supportsLatch] is structural —
     * a stick has no state to hold — and the spec's own tri-state is the user's
     * answer, falling back to the global setting when they have not given one.
     */
    private fun latchEnabled(control: ResolvedTouchControl): Boolean =
        control.spec.kind.supportsLatch &&
            (control.spec.latch ?: config.latch.enabledByDefault)

    private val TouchControlLatch?.isLatched: Boolean get() = this?.latched == true

    /**
     * How far a dwelling contact may drift, in this layout's real coordinates.
     *
     * Converted from logical units through the region's density scale and NOT
     * through the layout scale: the tolerance is a distance on the glass, so it
     * must not shrink because the controller was laid out smaller.
     */
    private fun gestureSlop(): Float =
        config.latch.gestureSlopUnits * layout.region.unitScale

    /**
     * How far an armed contact must slide to lock, in this layout's real
     * coordinates. Same conversion as [gestureSlop], and for the same reason:
     * a deliberate motion is a distance on the glass, not a fraction of a
     * control the user may have resized.
     */
    private fun latchCommitDistance(): Float =
        config.latch.latchCommitDistanceUnits * layout.region.unitScale

    /**
     * The slide crossed the commit distance.
     *
     * Nothing is published: the control is already pressed by the contact that
     * performed the gesture, so the hold changes only what happens when that
     * contact eventually lifts. The surface refreshes its picture after every
     * batch and picks the badge up from there.
     */
    private fun commitLatch(controlId: String) {
        latchesEngaged++
        feedback.perform(TouchFeedbackEvent.LatchEngaged)
        latchObserver.onLatchEvent(TouchLatchEvent.Engaged(controlId))
    }

    private fun noteLatchDown(control: ResolvedTouchControl, timeNanos: Long, x: Float, y: Float) {
        if (!latchEnabled(control)) {
            // A control whose latch was turned off while it was held would
            // otherwise keep the hold with no gesture left to release it, and any
            // pending dwell or pulse would be work nothing can complete.
            latches.remove(control.id)?.takeIf { it.latched }?.let {
                latchesCleared++
                latchObserver.onLatchEvent(
                    TouchLatchEvent.Cleared(setOf(control.id), TouchReleaseReason.SettingsChanged),
                )
            }
            return
        }
        val latch = latches.getOrPut(control.id) { TouchControlLatch() }
        // The control is already published as pressed, so `engage` above found
        // nothing to add and stayed silent. It is still a press the user made.
        if (latch.latched) feedback.perform(TouchFeedbackEvent.Press)
        // Arms whichever dwell this press is a candidate for. Nothing toggles
        // and nothing is masked here; see [TouchControlLatch].
        latch.onDown(timeNanos, x, y, config.latch)
    }

    /**
     * A contact ended. A quick tap on a latched control becomes a retrigger.
     *
     * The pulse is started HERE rather than on the press because a press on a
     * latched control is ambiguous until it ends: quick means "press it again",
     * held means "stop holding it". Pulsing on the way down would make every
     * unlatch emit a pointless release/press first. Deferring costs nothing —
     * the game sees the button as held throughout, which is what the hold is
     * for — and it does not touch the unlatched path at all, so an ordinary
     * gameplay press is never delayed.
     */
    private fun noteLatchEnd(control: ResolvedTouchControl, timeNanos: Long, cancelled: Boolean) {
        if (!latchEnabled(control)) return
        val latch = latches[control.id] ?: return
        val retrigger = latch.onEnd(timeNanos, cancelled, config.latch)
        if (retrigger && latch.beginRetrigger(timeNanos, config.latch)) retriggerPulses++
    }

    /**
     * The next moment the engine has timed work, in the same clock as
     * [TouchContact.timeNanos]. Null when it is purely event-driven.
     *
     * A deadline can only ever APPEAR as a result of a contact event, so a host
     * that consults this after every contact batch — and again after each
     * [onTick] — cannot miss one.
     */
    fun nextDeadlineNanos(): Long? {
        if (latches.isEmpty()) return null
        var best: Long? = null
        latches.values.forEach { latch ->
            val deadline = latch.nextDeadlineNanos() ?: return@forEach
            if (best == null || deadline < best!!) best = deadline
        }
        return best
    }

    /**
     * Advance timed gesture work to [nowNanos].
     *
     * Safe to call at any time, from anywhere in the host's schedule: it reads
     * live engine state rather than anything captured when the work was
     * scheduled, so a tick that lands after a release, a layout change or a
     * teardown finds an empty map and does nothing. That is deliberately the
     * only mechanism preventing a pending retrigger from resurrecting a button
     * after the session ended — there is no queue to invalidate.
     */
    fun onTick(nowNanos: Long) {
        if (latches.isEmpty()) return
        var changed = false
        latches.forEach { (id, latch) ->
            if (latch.holdDeadlineNanos in 1..nowNanos) {
                when (latch.dwell) {
                    // Armed only. The control is still an ordinary held button
                    // and letting go now simply ends the press; the slide is what
                    // commits. Nothing published changes, so nothing is published.
                    TouchDwell.Engage -> {
                        latch.armEngage()
                        latchesArmed++
                        feedback.perform(TouchFeedbackEvent.LatchArmed)
                    }
                    TouchDwell.Release -> {
                        latch.completeRelease()
                        latchesReleased++
                        feedback.perform(TouchFeedbackEvent.LatchReleased)
                        latchObserver.onLatchEvent(TouchLatchEvent.Released(id))
                        // The finger that performed the release gesture is still
                        // down, and stays authoritative until it lifts: dropping
                        // the button at this instant would be a release edge the
                        // user never made. Only a latch with nothing touching it
                        // — which a boundary clear can produce — rests here.
                        if (controlToContact[id] == null) layout.control(id)?.let(::disengage)
                        changed = true
                    }
                    TouchDwell.None -> latch.cancelDwell()
                }
            }
            if (latch.retriggerDeadlineNanos in 1..nowNanos) {
                latch.endRetrigger()
                changed = true
            }
        }
        if (changed) publish()
    }

    /**
     * Drop every latch without touching contact ownership.
     *
     * A control a finger is still on stays pressed — that finger is now the only
     * thing holding it, and it will release normally on lift. Anything else
     * returns to rest here, which is the point.
     */
    private fun clearLatches(reason: TouchReleaseReason) {
        if (latches.isEmpty()) return
        val cleared = latches.filterValues { it.latched }.keys.toSet()
        latches.clear()
        cleared.forEach { id ->
            if (controlToContact[id] == null) layout.control(id)?.let(::disengage)
        }
        if (cleared.isNotEmpty()) {
            latchesCleared += cleared.size
            latchObserver.onLatchEvent(TouchLatchEvent.Cleared(cleared, reason))
        }
        // Unconditional: an in-flight retrigger mask that was dropped here would
        // otherwise leave the published state suppressed with nothing left to
        // lift it.
        publish()
    }

    /**
     * Drop every touch contribution and every ownership, for a stated reason.
     *
     * The one operation every invalidating boundary calls. Idempotent by
     * construction: it assigns rather than toggles, so calling it twice cannot
     * invent a press, and re-publishing an already-neutral contribution is a
     * no-op at the sink.
     */
    fun releaseAll(reason: TouchReleaseReason) {
        releaseAllCount++
        lastReleaseReason = reason
        val clearedLatches = latches.filterValues { it.latched }.keys.toSet()
        latches.clear()
        contactToControl.clear()
        controlToContact.clear()
        facePresses.clear()
        logicalPresses.clear()
        leftStick = TouchVector.Zero
        rightStick = TouchVector.Zero
        leftTrigger = 0f
        rightTrigger = 0f
        dpad = DpadState.None
        if (clearedLatches.isNotEmpty()) {
            latchesCleared += clearedLatches.size
            latchObserver.onLatchEvent(TouchLatchEvent.Cleared(clearedLatches, reason))
        }
        publish()
    }

    fun diagnostics(): TouchDiagnosticsSnapshot = TouchDiagnosticsSnapshot(
        ownedControls = controlToContact.size,
        activeContacts = contactToControl.size,
        contactsClaimed = contactsClaimed,
        contactsUnclaimed = contactsUnclaimed,
        contactsContested = contactsContested,
        contactsCancelled = contactsCancelled,
        releaseAllCount = releaseAllCount,
        lastReleaseReason = lastReleaseReason,
        latchedControls = latchedControlIds(),
        armedControls = armedControlIds(),
        latchesArmed = latchesArmed,
        latchesEngaged = latchesEngaged,
        latchesReleased = latchesReleased,
        latchesCleared = latchesCleared,
        retriggerPulses = retriggerPulses,
        lastContactTimeNanos = lastContactTimeNanos,
        leftStick = leftStick,
        rightStick = rightStick,
        dpad = dpad,
    )

    /** The controls a latch is holding down right now. */
    fun latchedControlIds(): Set<String> =
        if (latches.isEmpty()) emptySet() else latches.filterValues { it.latched }.keys.toSet()

    /** The controls one deliberate slide away from becoming a hold. */
    fun armedControlIds(): Set<String> =
        if (latches.isEmpty()) emptySet() else latches.filterValues { it.armed }.keys.toSet()

    /** Which control, if any, currently owns [contactId]. For tests and diagnostics. */
    fun ownerOf(contactId: Long): String? = contactToControl[contactId]

    /** Which contact, if any, owns [controlId]. For tests and diagnostics. */
    fun contactOn(controlId: String): Long? = controlToContact[controlId]

    // ------------------------------------------------------------------ internals

    /**
     * Pick the control a Down at this point claims.
     *
     * Highest declared priority wins; if two still tie, the one the point is most
     * plainly inside wins. A layout that reaches the tie-break at all has already
     * failed [TouchLayoutAudit], so this is a deterministic fallback rather than
     * a design: the alternative is letting draw order decide what the user
     * pressed.
     */
    private fun hitTest(x: Float, y: Float): ResolvedTouchControl? {
        var best: ResolvedTouchControl? = null
        var bestDistance = Float.MAX_VALUE
        layout.controls.forEach { control ->
            if (!control.hitTest(x, y)) return@forEach
            val current = best
            if (current == null || control.spec.priority > current.spec.priority) {
                best = control
                bestDistance = control.normalizedDistance(x, y)
                return@forEach
            }
            if (control.spec.priority < current.spec.priority) return@forEach
            val distance = control.normalizedDistance(x, y)
            if (distance < bestDistance) {
                best = control
                bestDistance = distance
            }
        }
        return best
    }

    /** Apply a control's value for a contact at this point. */
    private fun engage(control: ResolvedTouchControl, x: Float, y: Float) {
        when (val action = control.spec.action) {
            is TouchControlAction.Face -> {
                if (facePresses.add(action.position)) feedback.perform(TouchFeedbackEvent.Press)
            }
            is TouchControlAction.Logical -> {
                if (logicalPresses.add(action.button)) feedback.perform(TouchFeedbackEvent.Press)
            }
            is TouchControlAction.Trigger -> {
                // Both halves, coherently. A physical trigger publishes its
                // digital bit AND its analog value, and the adapter's seam reads
                // either; a touch trigger that published only one would be a
                // second contract for the same control.
                if (action.side == ControlSide.Left) {
                    leftTrigger = 1f
                    if (logicalPresses.add(ControllerButton.L2)) feedback.perform(TouchFeedbackEvent.Press)
                } else {
                    rightTrigger = 1f
                    if (logicalPresses.add(ControllerButton.R2)) feedback.perform(TouchFeedbackEvent.Press)
                }
            }
            is TouchControlAction.Stick -> {
                val vector = TouchStick.resolve(
                    dx = x - control.centerX,
                    dy = y - control.centerY,
                    radius = control.trackingRadius,
                    deadzone = config.stickDeadzone,
                )
                if (action.side == ControlSide.Left) leftStick = vector else rightStick = vector
            }
            TouchControlAction.Directions -> {
                val next = TouchDpad.resolve(
                    dx = x - control.centerX,
                    dy = y - control.centerY,
                    radius = control.trackingRadius,
                    config = config,
                    previous = dpad,
                )
                if (next != dpad) {
                    dpad = next
                    feedback.perform(
                        if (next == DpadState.None) TouchFeedbackEvent.Release
                        else TouchFeedbackEvent.DirectionChange,
                    )
                }
            }
        }
    }

    /** Return a control to rest. Assigns rest values rather than undoing a delta. */
    private fun disengage(control: ResolvedTouchControl) {
        when (val action = control.spec.action) {
            is TouchControlAction.Face -> facePresses -= action.position
            is TouchControlAction.Logical -> logicalPresses -= action.button
            is TouchControlAction.Trigger -> if (action.side == ControlSide.Left) {
                leftTrigger = 0f
                logicalPresses -= ControllerButton.L2
            } else {
                rightTrigger = 0f
                logicalPresses -= ControllerButton.R2
            }
            is TouchControlAction.Stick -> if (action.side == ControlSide.Left) {
                // Exact centre immediately. A knob may animate home for looks,
                // but the axis is neutral the instant the thumb leaves.
                leftStick = TouchVector.Zero
            } else {
                rightStick = TouchVector.Zero
            }
            TouchControlAction.Directions -> dpad = DpadState.None
        }
    }

    /**
     * Compose and emit the whole contribution.
     *
     * The retrigger mask is applied HERE rather than by mutating the
     * accumulators, and that placement is the point: ownership, latch state and
     * the press/release bookkeeping all stay exactly as they were, and a pulse
     * is purely a statement about what is published. Nothing has to be undone
     * when it expires, and no boundary has to know it existed.
     */
    private fun publish() {
        var faces: Set<FaceButtonPosition> = facePresses
        var logicals: Set<ControllerButton> = logicalPresses
        var maskedLeftTrigger = leftTrigger
        var maskedRightTrigger = rightTrigger

        if (latches.values.any { it.retriggering }) {
            val remainingFaces = facePresses.toMutableSet()
            val remainingLogical = logicalPresses.toMutableSet()
            latches.forEach { (id, latch) ->
                if (!latch.retriggering) return@forEach
                when (val action = layout.control(id)?.spec?.action) {
                    is TouchControlAction.Face -> remainingFaces -= action.position
                    is TouchControlAction.Logical -> remainingLogical -= action.button
                    is TouchControlAction.Trigger -> if (action.side == ControlSide.Left) {
                        maskedLeftTrigger = 0f
                        remainingLogical -= ControllerButton.L2
                    } else {
                        maskedRightTrigger = 0f
                        remainingLogical -= ControllerButton.R2
                    }
                    // Vector controls never latch, so they never retrigger.
                    else -> Unit
                }
            }
            faces = remainingFaces
            logicals = remainingLogical
        }

        val next = TouchContribution(
            leftX = TouchAxis.toBridge(leftStick.x),
            leftY = TouchAxis.toBridge(leftStick.y),
            rightX = TouchAxis.toBridge(rightStick.x),
            rightY = TouchAxis.toBridge(rightStick.y),
            leftTrigger = TouchAxis.triggerToBridge(maskedLeftTrigger),
            rightTrigger = TouchAxis.triggerToBridge(maskedRightTrigger),
            dpad = dpad,
            positionalButtons = faces.mapTo(mutableSetOf()) { it.positional },
            logicalButtons = logicals.toSet(),
        )
        if (next == published) return
        published = next
        onContribution(next)
    }
}
