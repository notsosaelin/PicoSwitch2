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
    /** Holds taken back off by sliding to where the committing gesture began. */
    val latchesCancelled: Long = 0,
    /** Analog triggers that reached the terminal click. */
    val triggerDetents: Long = 0,
    /** Taps on an analog trigger that published a brief full pull. */
    val triggerPulses: Long = 0,
    /**
     * What each analog trigger is publishing, `0..1`, by control id.
     *
     * A value rather than a counter because it is the only part of this feature
     * a user can see being wrong: a trigger held at some level with nothing
     * touching it looks identical to one at rest in every other diagnostic.
     */
    val analogTriggers: Map<String, Float> = emptyMap(),
    /**
     * Which way each analog trigger's fill should grow, by control id.
     *
     * Reported next to the levels, and by the engine rather than by the
     * renderer, for one reason: while a gesture is live this is the axis FROZEN
     * at pointer-down, and only the engine has that. A surface deriving it from
     * the layout every frame would agree with the engine right up until the
     * moment the two could differ.
     */
    val analogTriggerFills: Map<String, TouchFillDirection> = emptyMap(),
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
 * ## Triggers with real travel
 *
 * The GameCube personality's `L` and `R` are the same gesture with a value
 * attached. A pull along an invisible position-derived axis is the trigger's
 * travel; the same slide that locks a digital button locks a trigger AT THE
 * LEVEL IT ENDS ON, which is the one thing a Boolean hold cannot express. The
 * recognizer is unchanged and shared — only the distance that counts toward
 * committing is projected onto the trigger's own axis, so a sideways slide
 * cannot lock a trigger to nothing. See [TouchAnalogTriggerState].
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

    /**
     * Per-control travel state for the triggers that have any, created on first
     * touch and bounded and cleared exactly like [latches].
     *
     * Keyed by CONTROL, not by side, so two triggers pulled at once are two
     * independent gestures with two frozen axes and two owning contacts. One
     * shared "the trigger gesture" would make the second finger fight the first.
     */
    private val analogTriggers = mutableMapOf<String, TouchAnalogTriggerState>()

    /**
     * What each INSTANCE is contributing, never what each binding is doing.
     *
     * This is the whole of duplicate safety. Two A buttons are two keys in
     * [facePresses]; releasing one removes one key and the other still maps to
     * the same position, so the aggregate at [publish] stays pressed. Keyed by
     * binding — which is what a set of positions is — the second release would
     * have taken the first one's press with it.
     */
    private val facePresses = mutableMapOf<String, FaceButtonPosition>()
    private val logicalPresses = mutableMapOf<String, ControllerButton>()

    /** Per-instance trigger level, by side, aggregated with `max` at publish. */
    private val leftTriggerLevels = mutableMapOf<String, Float>()
    private val rightTriggerLevels = mutableMapOf<String, Float>()

    /**
     * Per-instance stick and direction values, plus which instance currently
     * SPEAKS for each logical control.
     *
     * Sticks and the D-pad cannot be aggregated the way buttons can: two full
     * deflections in different directions have no meaningful sum, and averaging
     * them would invent a third direction the user never asked for. Ownership
     * instead — the first instance to move one wins it until its contact ends,
     * then the next instance still being touched takes over.
     */
    private val stickVectors = mutableMapOf<String, TouchVector>()
    private val stickOwners = mutableMapOf<ControlSide, String>()
    private val dpadStates = mutableMapOf<String, DpadState>()
    private var dpadOwner: String? = null

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
    private var latchesCancelled = 0L
    private var triggerDetents = 0L
    private var triggerPulses = 0L

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
        // AFTER the latch, because whether the control is already held is what
        // decides whether a still press here means "pull it fully" or "let go of
        // it", and only one of those two may be armed at a time.
        beginAnalogTrigger(target, contact)
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
            // An analog trigger tracks movement like a vector control AND
            // recognizes the hold gesture like a button, because for it they are
            // the same motion: the slide that locks the control is the slide
            // that chooses what it locks at.
            else -> if (control.isAnalogTrigger) {
                moveAnalogTrigger(control, contact)
            } else {
                // A button's movement means one of exactly three things: a
                // drag that abandons a pending dwell, the slide that commits an
                // armed one, or the slide back that takes it off again. NONE of
                // them changes what is published — the control is already
                // pressed by this very contact, and stays pressed either way —
                // so nothing is published here.
                when (
                    latches[controlId]?.onMove(
                        contact.x,
                        contact.y,
                        gestureSlop(),
                        latchCommitDistance(),
                        latchCancelDistance(),
                    )
                ) {
                    TouchLatchMove.Committed -> commitLatch(controlId)
                    TouchLatchMove.Cancelled -> cancelLatch(controlId)
                    else -> Unit
                }
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
            endAnalogTrigger(control, contact, cancelled)
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
     * How close to its origin a committed contact must return to take the hold
     * back off, in this layout's real coordinates. Same conversion as the two
     * above, and for the same reason.
     */
    private fun latchCancelDistance(): Float =
        config.latch.latchCancelDistanceUnits * layout.region.unitScale

    /** How far a contact must move to become a trigger pull, in real coordinates. */
    private fun analogDragSlop(): Float =
        config.trigger.dragSlopUnits * layout.region.unitScale

    /**
     * The slide crossed the commit distance.
     *
     * Nothing is published: the control is already pressed by the contact that
     * performed the gesture, so the hold changes only what happens when that
     * contact eventually lifts. The surface refreshes its picture after every
     * batch and picks the badge up from there. (An analog trigger publishes from
     * its own path either way, because the same slide is still moving its
     * value.)
     */
    private fun commitLatch(controlId: String, analogValue: Float? = null) {
        latchesEngaged++
        feedback.perform(TouchFeedbackEvent.LatchEngaged)
        latchObserver.onLatchEvent(TouchLatchEvent.Engaged(controlId, analogValue))
    }

    /**
     * The committing contact came back to where it started; the hold is off.
     *
     * Nothing is published, and that is the point: the finger is still down, so
     * the control is still physically pressed and the console sees no edge at
     * all. Only the hold that would have outlived the finger is gone, and
     * lifting off now ends the press like any other.
     *
     * The recognizer has already put the contact back to ARMED, so the open
     * padlock reappears and a slide out would lock it again. Deliberately only
     * ONE tick for that — the same [TouchFeedbackEvent.LatchReleased] a
     * press-and-hold unlatch gives, because "the hold is gone" is what the user
     * needs to feel. Adding the arming tick on top of it would put two events on
     * one action, which reads as one blurred buzz rather than two states.
     */
    private fun cancelLatch(controlId: String) {
        latchesCancelled++
        feedback.perform(TouchFeedbackEvent.LatchReleased)
        latchObserver.onLatchEvent(TouchLatchEvent.Cancelled(controlId))
    }

    private fun noteLatchDown(control: ResolvedTouchControl, timeNanos: Long, x: Float, y: Float) {
        if (!latchEnabled(control)) {
            // A control whose latch was turned off while it was held would
            // otherwise keep the hold with no gesture left to release it, and any
            // pending dwell or pulse would be work nothing can complete.
            latches.remove(control.id)?.takeIf { it.latched }?.let {
                latchesCleared++
                analogTriggers[control.id]?.clearLatch()
                latchObserver.onLatchEvent(
                    TouchLatchEvent.Cleared(setOf(control.id), TouchReleaseReason.SettingsChanged),
                )
            }
            return
        }
        val latch = latches.getOrPut(control.id) { TouchControlLatch() }
        // The control is already published as pressed, so `engage` above found
        // nothing to add and stayed silent. It is still a press the user made.
        // An analog trigger is excluded because it acknowledges EVERY press from
        // its own path — it never publishes on the way down, latched or not — and
        // two ticks for one touch read as one blurred buzz.
        if (latch.latched && !control.isAnalogTrigger) feedback.perform(TouchFeedbackEvent.Press)
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
        // An analog trigger re-fires by PUBLISHING a value rather than by having
        // its hold masked away, so it runs its own pulse and must not also take
        // the digital mask; two of them would cancel out. That is also why
        // `retriggering` is structurally always false for an analog trigger, and
        // why `publish` does not have to exclude it from the mask.
        if (control.isAnalogTrigger) return
        if (retrigger && latch.beginRetrigger(timeNanos, config.latch)) retriggerPulses++
    }

    // ------------------------------------------------------------ analog trigger

    /**
     * Whether this control has real travel on the far side; see
     * [TouchControlAction.Trigger].
     */
    private val ResolvedTouchControl.isAnalogTrigger: Boolean
        get() = (spec.action as? TouchControlAction.Trigger)?.analog == true

    /**
     * A contact claimed an analog trigger.
     *
     * Deliberately publishes nothing at all — see [TouchAnalogTriggerState]. The
     * press haptic still fires, because the user did hit a control and a control
     * that feels dead to the touch reads as broken.
     */
    private fun beginAnalogTrigger(control: ResolvedTouchControl, contact: TouchContact) {
        if (!control.isAnalogTrigger) return
        val state = analogTriggers.getOrPut(control.id) { TouchAnalogTriggerState() }
        val latch = latches[control.id]
        state.onDown(
            contact = contact,
            control = control,
            region = layout.region,
            latched = latch.isLatched,
            // The recognizer has already classified this press; read its answer
            // rather than re-deriving one. An Engage candidate is the second
            // press of a double tap, so it is on its way to CHOOSING a held
            // level and must not resolve into a full pull first. See
            // [TouchAnalogTriggerState.onDown].
            latchSelecting = latch?.dwell == TouchDwell.Engage,
            config = config.trigger,
            // The SAME deliberate-hold base both latch dwells derive from: a
            // press held that long is deliberate, whichever gesture it turns out
            // to belong to. A second copy of the number would be a second thing
            // to keep in step.
            holdResolveNanos = config.latch.holdThresholdNanos,
        )
        feedback.perform(TouchFeedbackEvent.Press)
        applyAnalogTrigger(control)
    }

    private fun moveAnalogTrigger(control: ResolvedTouchControl, contact: TouchContact) {
        val state = analogTriggers[control.id] ?: return
        val detentWas = state.physicalDetent
        val travel = state.onMove(contact, config.trigger, analogDragSlop())
        if (state.physicalDetent && !detentWas) {
            triggerDetents++
            feedback.perform(TouchFeedbackEvent.TriggerDetent)
        }
        when (
            latches[control.id]?.onMove(
                contact.x,
                contact.y,
                gestureSlop(),
                latchCommitDistance(),
                latchCancelDistance(),
                commitTravel = travel,
            )
        ) {
            TouchLatchMove.Committed -> {
                state.commitLatch()
                commitLatch(control.id, state.latchedValue)
            }
            // The level goes with the hold. Nothing published moves: the finger
            // is still down and a live pull outranks a held level anyway, so the
            // console sees the same byte across the transition.
            TouchLatchMove.Cancelled -> {
                state.clearLatch()
                cancelLatch(control.id)
            }
            else -> Unit
        }
        applyAnalogTrigger(control)
        publish()
    }

    private fun endAnalogTrigger(
        control: ResolvedTouchControl,
        contact: TouchContact,
        cancelled: Boolean,
    ) {
        if (!control.isAnalogTrigger) return
        val state = analogTriggers[control.id] ?: return
        val latched = latches[control.id].isLatched
        val pulsed = state.onEnd(
            contact = contact,
            cancelled = cancelled,
            // Read AFTER the recognizer has seen the release, so a press that
            // removed the hold is recognizable as the release gesture it was and
            // does not also fire a trigger click on the way out.
            latched = latched,
            maxTapDurationNanos = config.latch.maxTapDurationNanos,
            pulseNanos = config.latch.retriggerReleaseNanos,
        )
        if (pulsed) triggerPulses++
        applyAnalogTrigger(control)
    }

    /**
     * Push one analog trigger's effective state into the published accumulators.
     *
     * Assigns rather than adjusts, exactly like [disengage]: the trigger's value
     * and its terminal click are both derived from one place every time, so no
     * path can leave half of the pair behind.
     */
    private fun applyAnalogTrigger(control: ResolvedTouchControl) {
        val action = control.spec.action as? TouchControlAction.Trigger ?: return
        if (!action.analog) return
        val state = analogTriggers[control.id]
        val latched = latches[control.id].isLatched
        val detent = state?.effectiveDetent(latched, config.trigger) ?: false
        // Capped below the detent, because on this personality the published
        // BYTE is the only thing that says whether the trigger clicked; see
        // [TouchTriggerConfig].
        val value = when {
            state == null -> 0f
            detent -> 1f
            else -> minOf(state.effectiveValue(latched), config.trigger.subDetentCeiling)
        }
        // Per INSTANCE, exactly like every other contributor: two L triggers on
        // screen are two independent gestures, and the console is told the
        // deeper of the two rather than whichever moved last.
        triggerLevels(action.side)[control.id] = value
        val button =
            if (action.side == ControlSide.Left) ControllerButton.L2 else ControllerButton.R2
        if (detent) logicalPresses[control.id] = button else logicalPresses -= control.id
    }

    /** Every analog trigger's published level, for the surface and diagnostics. */
    private fun analogTriggerLevels(): Map<String, Float> {
        if (analogTriggers.isEmpty()) return emptyMap()
        val levels = mutableMapOf<String, Float>()
        analogTriggers.forEach { (id, state) ->
            val control = layout.control(id) ?: return@forEach
            if (!control.isAnalogTrigger) return@forEach
            val latched = latches[id].isLatched
            levels[id] = if (state.effectiveDetent(latched, config.trigger)) {
                1f
            } else {
                minOf(state.effectiveValue(latched), config.trigger.subDetentCeiling)
            }
        }
        return levels
    }

    /**
     * Which way each analog trigger's fill grows, for the surface.
     *
     * Every analog trigger in the layout, not only the ones that have been
     * touched: a control at rest still has to know which way it WOULD fill, and
     * a trigger the user has just dragged somewhere else must re-present itself
     * without waiting to be pressed first.
     *
     * Three sources, in order, and the order is the point:
     *
     * ```text
     *   a swipe has declared itself   the SWIPE's own direction, frozen
     *   a contact but no swipe yet    the axis frozen at pointer-down
     *   nothing touching it           the axis for where the control now is
     * ```
     *
     * The first is what a user actually sees themselves doing; the other two are
     * the only statement available when there is no swipe to read. Routing all
     * three through here rather than letting a surface re-derive them is what
     * makes the freeze real: a thumb arcing across the dominance boundary
     * mid-pull cannot flip the picture, because what is being drawn from stopped
     * moving when the swipe was recognized.
     */
    private fun analogTriggerFills(): Map<String, TouchFillDirection> {
        val fills = mutableMapOf<String, TouchFillDirection>()
        layout.controls.forEach { control ->
            if (!control.isAnalogTrigger) return@forEach
            val live = analogTriggers[control.id]?.takeIf {
                it.pointerId != TouchAnalogTriggerState.NO_POINTER
            }
            fills[control.id] = live?.swipeFill ?: TouchTriggerTravel.fillDirection(
                live?.axis ?: TouchTriggerTravel.inwardAxis(
                    control.centerX,
                    control.centerY,
                    layout.region,
                    config.trigger.centerEpsilonUnits,
                ),
            )
        }
        return fills
    }

    /** Drop every trigger's travel state and the levels any hold was carrying. */
    private fun clearAnalogTriggers() {
        if (analogTriggers.isEmpty()) return
        analogTriggers.keys.forEach { id ->
            leftTriggerLevels -= id
            rightTriggerLevels -= id
            logicalPresses -= id
        }
        analogTriggers.clear()
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
        var best: Long? = null
        fun consider(deadline: Long?) {
            if (deadline == null) return
            if (best == null || deadline < best!!) best = deadline
        }
        latches.values.forEach { consider(it.nextDeadlineNanos()) }
        analogTriggers.values.forEach { consider(it.nextDeadlineNanos()) }
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
        if (latches.isEmpty() && analogTriggers.isEmpty()) return
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
                        // A trigger's arming press deferred its full-pull
                        // resolve so the level the slide selects is the first
                        // thing it ever publishes. Restart it from HERE: the
                        // slide is available now, and a press that never takes
                        // it is an ordinary hold that still has to end up
                        // pulled. Nothing published changes at this instant.
                        analogTriggers[id]?.armLatchSelection(
                            nowNanos,
                            config.latch.holdThresholdNanos,
                        )
                    }
                    TouchDwell.Release -> {
                        latch.completeRelease()
                        latchesReleased++
                        feedback.perform(TouchFeedbackEvent.LatchReleased)
                        latchObserver.onLatchEvent(TouchLatchEvent.Released(id))
                        val control = layout.control(id)
                        analogTriggers[id]?.clearLatch()
                        // The finger that performed the release gesture is still
                        // down, and stays authoritative until it lifts: dropping
                        // the button at this instant would be a release edge the
                        // user never made. Only a latch with nothing touching it
                        // — which a boundary clear can produce — rests here.
                        if (controlToContact[id] == null) {
                            control?.let(::disengage)
                        } else if (control != null && control.isAnalogTrigger) {
                            // The hold is gone but the finger that removed it is
                            // still on the trigger. Re-arm, so the SAME contact
                            // can slide to a new level: replacing a held value
                            // must not mean lifting off and starting the whole
                            // engage gesture again. Silently — the release tick
                            // has just fired, and a second identical tick on top
                            // of it reads as one blurred buzz rather than two
                            // states. The open padlock says what a slide would
                            // now do.
                            latch.armEngage()
                            latchesArmed++
                        }
                        control?.takeIf { it.isAnalogTrigger }?.let(::applyAnalogTrigger)
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
        analogTriggers.forEach { (id, state) ->
            val control = layout.control(id) ?: return@forEach
            if (state.holdResolveDeadlineNanos in 1..nowNanos) {
                // Runs for an arming press too, but only AFTER it has armed:
                // [TouchAnalogTriggerState.onDown] withholds the deadline and
                // the Engage branch above restarts it. "Tap it, then keep
                // holding it" is ordinary play that games ask for directly, and
                // the digital design's whole answer to it is that an arming
                // press stays an ordinary held button; a trigger that published
                // nothing for as long as the thumb stayed down would break
                // exactly that. Ordering it after the arm is what stops the
                // other half — a press on its way to selecting a PARTIAL level —
                // sending a full pull and the detent first.
                //
                // The residual cost is a user who arms, waits out a further
                // base without moving, and only then slides to a lower level:
                // they see the trigger drop to it. That is honest rather than
                // guessed — by then the press really had become a still hold.
                // The fallback WINNING is what consumes the hold candidate: the
                // press has now been answered as an ordinary held trigger, and a
                // slide made after that answer must not still be able to lock a
                // partial level. Nothing else can re-arm this contact.
                if (state.resolveFullPull()) latches[id]?.abandonArm()
                triggerDetents++
                feedback.perform(TouchFeedbackEvent.TriggerDetent)
                applyAnalogTrigger(control)
                changed = true
            }
            if (state.pulseDeadlineNanos in 1..nowNanos) {
                state.endPulse()
                applyAnalogTrigger(control)
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
            // The level goes with the hold. A trigger that kept it would republish
            // it the moment any later gesture latched the same control.
            analogTriggers[id]?.clearLatch()
            if (controlToContact[id] == null) layout.control(id)?.let(::disengage)
            layout.control(id)?.takeIf { it.isAnalogTrigger }?.let(::applyAnalogTrigger)
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
        clearAnalogTriggers()
        contactToControl.clear()
        controlToContact.clear()
        facePresses.clear()
        logicalPresses.clear()
        leftTriggerLevels.clear()
        rightTriggerLevels.clear()
        stickVectors.clear()
        stickOwners.clear()
        dpadStates.clear()
        dpadOwner = null
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
        latchesCancelled = latchesCancelled,
        triggerDetents = triggerDetents,
        triggerPulses = triggerPulses,
        analogTriggers = analogTriggerLevels(),
        analogTriggerFills = analogTriggerFills(),
        lastContactTimeNanos = lastContactTimeNanos,
        leftStick = publishedStick(ControlSide.Left),
        rightStick = publishedStick(ControlSide.Right),
        dpad = publishedDpad(),
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
     * ```text
     * 1  highest declared priority        authored, rarely used
     * 2  highest z-order                  the control drawn in front
     * 3  most plainly inside              a deterministic last resort
     * ```
     *
     * Z-order sits above centrality on purpose: once instances may be freely
     * placed and stacked, the control the user can SEE on top is the one they
     * believe they are pressing, and any other answer is a surprise. Two
     * controls of different bindings reaching this at all still fails
     * [TouchLayoutAudit], so the tie-break is a guarantee of determinism rather
     * than a substitute for a layout that makes sense.
     */
    private fun hitTest(x: Float, y: Float): ResolvedTouchControl? {
        var best: ResolvedTouchControl? = null
        var bestDistance = Float.MAX_VALUE
        layout.controls.forEach { control ->
            if (!control.hitTest(x, y)) return@forEach
            val current = best
            if (current == null) {
                best = control
                bestDistance = control.normalizedDistance(x, y)
                return@forEach
            }
            val order = compareValuesBy(control, current, { it.spec.priority }, { it.spec.zIndex })
            if (order > 0) {
                best = control
                bestDistance = control.normalizedDistance(x, y)
                return@forEach
            }
            if (order < 0) return@forEach
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
        val id = control.id
        when (val action = control.spec.action) {
            is TouchControlAction.Face -> {
                if (facePresses.put(id, action.position) == null) {
                    feedback.perform(TouchFeedbackEvent.Press)
                }
            }
            is TouchControlAction.Logical -> {
                if (logicalPresses.put(id, action.button) == null) {
                    feedback.perform(TouchFeedbackEvent.Press)
                }
            }
            is TouchControlAction.Trigger -> {
                // A trigger with real travel says nothing on the way down: what
                // the press means is not known yet, and full travel IS the
                // terminal click on the one personality that has one. Its whole
                // lifecycle lives in [TouchAnalogTriggerState] instead.
                if (action.analog) return
                // Both halves, coherently. A physical trigger publishes its
                // digital bit AND its analog value, and the adapter's seam reads
                // either; a touch trigger that published only one would be a
                // second contract for the same control.
                triggerLevels(action.side)[id] = 1f
                val button =
                    if (action.side == ControlSide.Left) ControllerButton.L2 else ControllerButton.R2
                if (logicalPresses.put(id, button) == null) {
                    feedback.perform(TouchFeedbackEvent.Press)
                }
            }
            is TouchControlAction.Stick -> {
                stickVectors[id] = TouchStick.resolve(
                    dx = x - control.centerX,
                    dy = y - control.centerY,
                    radius = control.trackingRadius,
                    deadzone = config.stickDeadzone,
                )
                // First mover wins, and keeps it until its contact ends. A
                // second stick instance being touched at the same time is not an
                // error and is not a steal: it simply says nothing yet.
                stickOwners.getOrPut(action.side) { id }
            }
            TouchControlAction.Directions -> {
                val owned = dpadOwner == null || dpadOwner == id
                val next = TouchDpad.resolve(
                    dx = x - control.centerX,
                    dy = y - control.centerY,
                    radius = control.trackingRadius,
                    config = config,
                    // Hysteresis is a property of the gesture in progress, so it
                    // reads THIS instance's previous direction rather than the
                    // published one, which may belong to a different instance.
                    previous = dpadStates[id] ?: DpadState.None,
                )
                val changed = dpadStates.put(id, next) != next
                if (dpadOwner == null) dpadOwner = id
                // Only the instance the console is listening to may buzz; a
                // second D-pad being brushed must not rattle.
                if (changed && owned) {
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
        val id = control.id
        when (val action = control.spec.action) {
            is TouchControlAction.Face -> facePresses -= id
            is TouchControlAction.Logical -> logicalPresses -= id
            // An analog trigger's rest value is not necessarily zero — a hold may
            // still be on it — and [applyAnalogTrigger] is the one place that
            // decides. Two writers for one axis is how half a state survives.
            is TouchControlAction.Trigger -> if (action.analog) {
                applyAnalogTrigger(control)
            } else {
                triggerLevels(action.side) -= id
                logicalPresses -= id
            }
            is TouchControlAction.Stick -> {
                // Exact centre immediately. A knob may animate home for looks,
                // but the axis is neutral the instant the thumb leaves.
                stickVectors -= id
                if (stickOwners[action.side] == id) {
                    stickOwners -= action.side
                    handOffStick(action.side)
                }
            }
            TouchControlAction.Directions -> {
                dpadStates -= id
                if (dpadOwner == id) {
                    dpadOwner = null
                    handOffDpad()
                }
            }
        }
    }

    private fun triggerLevels(side: ControlSide) =
        if (side == ControlSide.Left) leftTriggerLevels else rightTriggerLevels

    /**
     * Give a released stick to another instance that is still being held.
     *
     * In layout order, so the answer is deterministic rather than whatever the
     * hash map happened to iterate first. Without this, letting go of one of two
     * duplicated sticks would leave the other one dead until the thumb on it
     * moved again.
     */
    private fun handOffStick(side: ControlSide) {
        val next = layout.controls.firstOrNull { candidate ->
            val action = candidate.spec.action
            action is TouchControlAction.Stick && action.side == side &&
                controlToContact.containsKey(candidate.id)
        } ?: return
        stickOwners[side] = next.id
    }

    private fun handOffDpad() {
        dpadOwner = layout.controls.firstOrNull { candidate ->
            candidate.spec.action == TouchControlAction.Directions &&
                controlToContact.containsKey(candidate.id)
        }?.id
    }

    /** What the console is being told each vector control is doing. */
    private fun publishedStick(side: ControlSide): TouchVector =
        stickOwners[side]?.let { stickVectors[it] } ?: TouchVector.Zero

    private fun publishedDpad(): DpadState =
        dpadOwner?.let { dpadStates[it] } ?: DpadState.None

    /** Highest level any live instance of this trigger is asking for. */
    private fun publishedTrigger(side: ControlSide, masked: Set<String>): Float {
        val levels = triggerLevels(side)
        if (levels.isEmpty()) return 0f
        var best = 0f
        levels.forEach { (id, value) -> if (id !in masked && value > best) best = value }
        return best
    }

    /**
     * Compose and emit the whole contribution.
     *
     * ## Aggregation
     *
     * ```text
     * digital   any live instance holding a binding keeps that binding pressed
     * trigger   the deepest live instance of that side wins
     * stick     the owning instance speaks; the others say nothing
     * D-pad     the owning instance speaks; the others say nothing
     * ```
     *
     * The digital rule is what makes duplicates behave: pressing a second A and
     * then releasing the first leaves one contributor, so the console never sees
     * a release edge the user did not make.
     *
     * ## The retrigger mask
     *
     * Applied HERE as a set of INSTANCE ids to skip, rather than by mutating the
     * accumulators. Ownership, latch state and the press/release bookkeeping all
     * stay exactly as they were; a pulse is purely a statement about what is
     * published. Nothing has to be undone when it expires, no boundary has to
     * know it existed, and — because the mask is per instance — tapping one
     * held A to re-fire it cannot silence the other one.
     */
    private fun publish() {
        val masked: Set<String> = if (latches.values.none { it.retriggering }) {
            emptySet()
        } else {
            latches.filterValues { it.retriggering }.keys
        }

        val faces = mutableSetOf<FaceButtonPosition>()
        facePresses.forEach { (id, position) -> if (id !in masked) faces += position }
        val logicals = mutableSetOf<ControllerButton>()
        logicalPresses.forEach { (id, button) -> if (id !in masked) logicals += button }

        val leftStick = publishedStick(ControlSide.Left)
        val rightStick = publishedStick(ControlSide.Right)

        val next = TouchContribution(
            leftX = TouchAxis.toBridge(leftStick.x),
            leftY = TouchAxis.toBridge(leftStick.y),
            rightX = TouchAxis.toBridge(rightStick.x),
            rightY = TouchAxis.toBridge(rightStick.y),
            leftTrigger = TouchAxis.triggerToBridge(publishedTrigger(ControlSide.Left, masked)),
            rightTrigger = TouchAxis.triggerToBridge(publishedTrigger(ControlSide.Right, masked)),
            dpad = publishedDpad(),
            positionalButtons = faces.mapTo(mutableSetOf()) { it.positional },
            logicalButtons = logicals,
        )
        if (next == published) return
        published = next
        onContribution(next)
    }
}
