package dev.picoswitch.bridge.touch

/** Which gesture the press in progress is a candidate for. */
internal enum class TouchDwell { None, Engage, Release }

/** What a movement did to the hold, if anything. */
internal enum class TouchLatchMove {
    None,

    /** The slide reached the commit distance and the control is now held. */
    Committed,

    /**
     * The same contact came back to where it started and took the hold away
     * again.
     *
     * Distinct from a release: the finger is still down and the control is still
     * physically pressed, so nothing about what the console sees changes here.
     * Only the hold that would have OUTLIVED the finger is gone.
     */
    Cancelled,
}

/** No first tap is pending, so no gap can fall inside the double-tap window. */
private const val NO_PENDING_TAP = Long.MAX_VALUE

/**
 * One control's hold state: whether it is latched, and the timed and spatial
 * gestures that can change what it publishes.
 *
 * Three facts, deliberately kept apart:
 *
 * ```text
 * touchPressed    a contact owns the control right now   (TouchControlEngine's ownership map)
 * latchedPressed  the control was toggled into a hold    (this)
 * effectivePressed = touchPressed || latchedPressed      (masked while retriggering)
 * ```
 *
 * ## The gesture vocabulary
 *
 * ```text
 * UNLATCHED   tap                                        ordinary press
 *             tap, press, hold                           armed; STILL an ordinary press
 *             tap, press, hold, slide away               latch
 *             ... and slide back to where it began       cancel; armed again
 *
 * LATCHED     quick tap                                  retrigger: release edge, then held again
 *             press held 1x the base                     unlatch
 * ```
 *
 * ## The slide is reversible until the finger lifts
 *
 * Committing on the way OUT and cancelling on the way BACK costs nothing and
 * removes the one moment this gesture was unforgiving: a slide that has already
 * passed the commit distance used to be final, so a user who changed their mind
 * had to lift off and then press and hold the control to undo a hold they never
 * wanted. Now the same continuous motion undoes it.
 *
 * Two distances rather than one, and that is the whole of the hysteresis:
 *
 * ```text
 *   0                    cancel                     commit
 *   |----- cancelled -----|------ unchanged ---------|----- committed ----->
 *          (<= 24u)              (the band)                (>= 64u)
 * ```
 *
 * A single threshold would flap: a thumb resting exactly on it produces a
 * stream of lock/unlock transitions, each one a real change to what the console
 * is told after the finger lifts. With the band, returning has to be as
 * deliberate as leaving was.
 *
 * ## Why timing alone cannot create a hold
 *
 * Two earlier gestures were tried and both collided with real play. A plain
 * double tap collides with mashing, because mashing IS a stream of double taps.
 * A double tap whose second press is merely HELD collides with the very ordinary
 * "double tap, then keep holding" that games ask for directly — and no dwell,
 * however long, separates those two, because they are the same input.
 *
 * So the committing act is not a duration at all. After the dwell the gesture is
 * ARMED and nothing has changed: the control is still an ordinary physically
 * held button, and letting go simply ends the press. Only a deliberate SLIDE
 * away from where that press began commits the latch. Nothing a game asks a
 * player to do involves pressing a button and dragging off it, which is exactly
 * why that motion is safe to spend on this.
 *
 * Removing a hold stays a plain press-and-hold. Creating persistent state should
 * be the harder half of the pair; undoing something the user can see is wrong
 * should not be.
 *
 * ## Nothing here delays input
 *
 * The recognizer OBSERVES presses the engine has already applied — [onDown],
 * [onMove] and [onEnd] are called after `engage`/before `disengage` — so it can
 * never delay, swallow or reorder ordinary input. A detector that waited to see
 * what the user meant would make every first tap late.
 *
 * The one thing that IS deferred is the retrigger pulse, and only on a control
 * that is already latched. A press there is ambiguous until it ends — quick tap
 * means "press it again", held means "stop holding it" — so the pulse is decided
 * at release. That costs nothing: the game is seeing the button as held
 * throughout the decision window, which is exactly what the user asked for.
 *
 * ## Deliberately not Boolean-shaped
 *
 * Nothing here knows what the control DOES. The recognizer is about contacts,
 * distance and time only, so the planned analog-trigger hold — where the armed
 * contact's displacement will select a held trigger value rather than simply
 * commit — can reuse this same gesture rather than growing a parallel one.
 *
 * Scoped per control: two quick taps on two DIFFERENT buttons are two first
 * taps, never one gesture.
 */
internal class TouchControlLatch {

    /** The control is toggled into a persistent hold. */
    var latched: Boolean = false
        private set

    /**
     * The engage gesture is armed: the dwell elapsed and a slide would commit it.
     *
     * Nothing about the published state changes here. This is the window in
     * which the control is simultaneously an ordinary held button and one
     * movement away from becoming a hold.
     */
    var armed: Boolean = false
        private set

    /**
     * Absolute host time at which the press in progress completes its dwell, or
     * zero when it is not a candidate. Same clock as [TouchContact.timeNanos].
     */
    var holdDeadlineNanos = 0L
        private set

    /** What reaching [holdDeadlineNanos] would do. */
    var dwell = TouchDwell.None
        private set

    /** Absolute host time at which a retrigger pulse reasserts the hold; zero when idle. */
    var retriggerDeadlineNanos = 0L
        private set

    /** The hold is currently masked so a fresh press edge can be observed. */
    val retriggering: Boolean get() = retriggerDeadlineNanos != 0L

    /** When the last QUALIFYING tap released; zero when no tap is pending. */
    private var lastTapEndNanos = 0L

    /** When the current press began; zero when nothing is pressed. */
    private var pressStartNanos = 0L

    /** Where it began, so displacement is measured from the contact's own origin. */
    private var originX = 0f
    private var originY = 0f

    /**
     * Whether the press in progress may become half of a double tap.
     *
     * Cleared by the press that became a candidate, so one gesture attempt
     * consumes one sequence: a candidate the user abandoned does not leave a
     * half-open recognizer for the next press to trip over.
     */
    private var tapSequenceOpen = true

    /**
     * The hold currently standing was committed by the contact still down.
     *
     * The gate on cancelling, and it is load-bearing rather than bookkeeping: a
     * press on an ALREADY-held control begins inside the cancel radius by
     * definition — the finger is on the control it is about to press — so
     * without this, the smallest jitter would silently drop a hold the user made
     * earlier and is now merely touching. Only a hold this gesture created may
     * be undone by this gesture.
     */
    private var committedHere = false

    /**
     * A press began. Returns true when it is a candidate for either gesture,
     * which starts the corresponding dwell.
     *
     * Nothing toggles here, nothing is armed here and nothing is masked here.
     * The press is an ordinary press until a gesture completes, which is what
     * makes rapid tapping ordinary rapid tapping.
     */
    fun onDown(nowNanos: Long, x: Float, y: Float, config: TouchLatchConfig): Boolean {
        val hasClock = nowNanos > 0L
        val gap = if (lastTapEndNanos > 0L) nowNanos - lastTapEndNanos else NO_PENDING_TAP
        pressStartNanos = nowNanos
        originX = x
        originY = y
        lastTapEndNanos = 0L
        armed = false
        committedHere = false

        if (latched) {
            // Already held, so there is nothing to build up to: one press is the
            // whole release gesture, and no slide is required. Its outcome —
            // unlatch or retrigger — is not decided until it ends.
            tapSequenceOpen = false
            dwell = if (hasClock) TouchDwell.Release else TouchDwell.None
            holdDeadlineNanos =
                if (hasClock) nowNanos + config.latchReleaseThresholdNanos else 0L
            return hasClock
        }

        val qualifies = hasClock && gap >= config.minTapGapNanos && gap <= config.doubleTapWindowNanos
        tapSequenceOpen = !qualifies
        dwell = if (qualifies) TouchDwell.Engage else TouchDwell.None
        holdDeadlineNanos = if (qualifies) nowNanos + config.latchEngageThresholdNanos else 0L
        return qualifies
    }

    /** The dwell elapsed on an engage candidate: armed, but nothing is held yet. */
    fun armEngage() {
        cancelDwell()
        armed = true
    }

    /**
     * The armed candidate was spent on something else; no slide may commit it.
     *
     * The one caller is an analog trigger whose full-pull fallback won: the
     * press waited out the selection window without moving, so it has already
     * been answered as an ordinary held trigger. Leaving the arm standing would
     * let a slide made AFTER that answer still lock a partial hold, which is a
     * persistent state reached through a gesture the recognizer had already
     * resolved as something else.
     *
     * Final for this contact by construction rather than by a flag: [armEngage]
     * is only ever reached from a dwell, and this cancels the dwell too, so
     * nothing short of a new press can arm the control again.
     */
    fun abandonArm() {
        cancelDwell()
        armed = false
    }

    /** The dwell elapsed on a release candidate. */
    fun completeRelease() {
        cancelDwell()
        latched = false
    }

    /**
     * The contact moved. Returns true at the instant the latch COMMITS.
     *
     * Two different jobs either side of arming, both measured as displacement
     * from the contact's own origin:
     *
     * - before arming, travelling past [slop] abandons the candidate, because a
     *   drag is not the hold the gesture asked for;
     * - after arming, travelling past [commitDistance] commits the latch.
     *
     * Direction is irrelevant — any deliberate slide will do — and neither
     * distance has anything to do with the control's bounds. A bounds test would
     * make the gesture easy on a large button, hard on a small one, and
     * unreliable anywhere near an edge; none of which the user asked for.
     *
     * Commit is immediate rather than confirmed on lift, so the moment the
     * button locks is the moment the user feels it. Sliding back to where the
     * press began undoes it again — see [cancelDistance] — but only inside a
     * radius small enough that the return is as deliberate as the departure.
     *
     * [commitTravel] is how much of the slide COUNTS toward committing, when
     * that is not simply how far the contact moved. Null — every digital control
     * — means straight-line displacement, because for a button any deliberate
     * slide will do. An analog trigger supplies its projection onto its own
     * inward axis instead: there the slide is not merely a confirmation, it is
     * the act that CHOOSES the held level, so a sideways or outward slide must
     * not be able to lock a trigger to a level nobody selected.
     *
     * [cancelDistance] undoes a hold this same contact just made, and is always
     * a plain RADIUS around the origin even for an analog trigger. "Back where I
     * started" is a question about the finger's position, not about the
     * trigger's axis: a slide that returned along the axis but ended half a
     * screen sideways is not the user putting the control back. Deliberately
     * smaller than [commitDistance]; see the class doc for why the band exists.
     */
    fun onMove(
        x: Float,
        y: Float,
        slop: Float,
        commitDistance: Float,
        cancelDistance: Float,
        commitTravel: Float? = null,
    ): TouchLatchMove {
        val dx = x - originX
        val dy = y - originY
        val travelled = dx * dx + dy * dy
        if (armed) {
            val reached = commitTravel?.let { it >= commitDistance }
                ?: (travelled >= commitDistance * commitDistance)
            if (!reached) return TouchLatchMove.None
            armed = false
            latched = true
            committedHere = true
            return TouchLatchMove.Committed
        }
        if (committedHere && latched) {
            if (travelled > cancelDistance * cancelDistance) return TouchLatchMove.None
            latched = false
            committedHere = false
            // Straight back to ARMED rather than to nothing, so the user who
            // overshot can simply slide out again. That is what makes the band
            // above a hysteresis rather than a one-way door.
            armed = true
            return TouchLatchMove.Cancelled
        }
        if (holdDeadlineNanos == 0L) return TouchLatchMove.None
        if (travelled > slop * slop) cancelDwell()
        return TouchLatchMove.None
    }

    /**
     * A press ended. Returns true when it was a quick tap on a latched control,
     * which is the caller's cue to start a retrigger pulse.
     *
     * Also records the press as a pending tap if it really was one, and abandons
     * any dwell or arming. Releasing while armed but before the slide is not a
     * latch and never was: the press simply ends, exactly as an ordinary held
     * button would.
     *
     * A cancelled contact is never a tap and never a retrigger: the platform took
     * the gesture away, so the user did not release anything.
     */
    fun onEnd(nowNanos: Long, cancelled: Boolean, config: TouchLatchConfig): Boolean {
        val retrigger = !cancelled && dwell == TouchDwell.Release
        cancelDwell()
        armed = false
        committedHere = false
        val duration = nowNanos - pressStartNanos
        val tapped = !cancelled && tapSequenceOpen && nowNanos > 0L && pressStartNanos > 0L &&
            duration in 0..config.maxTapDurationNanos
        lastTapEndNanos = if (tapped) nowNanos else 0L
        pressStartNanos = 0L
        tapSequenceOpen = true
        return retrigger
    }

    fun cancelDwell() {
        holdDeadlineNanos = 0L
        dwell = TouchDwell.None
    }

    /**
     * Start masking the hold so a fresh press edge exists. Returns false when a
     * pulse is already in flight — a burst faster than the mask would otherwise
     * read as one long release instead of repeated presses.
     */
    fun beginRetrigger(nowNanos: Long, config: TouchLatchConfig): Boolean {
        if (retriggering || nowNanos <= 0L) return false
        retriggerDeadlineNanos = nowNanos + config.retriggerReleaseNanos
        return true
    }

    /** The mask expired; the hold reasserts itself. */
    fun endRetrigger() {
        retriggerDeadlineNanos = 0L
    }

    /** The earliest timed transition this control is waiting for, if any. */
    fun nextDeadlineNanos(): Long? = when {
        holdDeadlineNanos == 0L -> retriggerDeadlineNanos.takeIf { it != 0L }
        retriggerDeadlineNanos == 0L -> holdDeadlineNanos
        else -> minOf(holdDeadlineNanos, retriggerDeadlineNanos)
    }
}

/**
 * A change to what the on-screen controller is holding by itself.
 *
 * Emitted only on latch transitions — never per contact, never per frame, and
 * NOT for retrigger pulses, which are ordinary presses of an already-held
 * control. This is the one part of the touch path that can leave the console
 * believing a button is down with nothing on screen touching it, and the log
 * that explains a stuck button has to stay readable.
 */
sealed interface TouchLatchEvent {
    /**
     * The user slid an armed contact far enough to commit [controlId] into a
     * hold.
     *
     * [analogValue] is the level an analog trigger was locked at, and null for
     * every digital control, where "held" has no level to state. Reported at
     * COMMIT rather than at release, so the log records the moment the console
     * started being told something no finger is doing; the slide may still move
     * the level before the thumb lifts.
     */
    data class Engaged(val controlId: String, val analogValue: Float? = null) : TouchLatchEvent

    /** The user pressed and held [controlId] out of its hold. */
    data class Released(val controlId: String) : TouchLatchEvent

    /**
     * The user slid back to where the committing gesture began and took the hold
     * off again, without ever lifting the finger.
     *
     * Kept apart from [Released] because the two answer different questions
     * about a control that is no longer held: one is a deliberate press-and-hold
     * some time later, the other is the same gesture being taken back. A log
     * that conflated them could not tell "the user changed their mind" from "the
     * user came back and undid it".
     */
    data class Cancelled(val controlId: String) : TouchLatchEvent

    /**
     * Every hold was dropped at a boundary.
     *
     * [reason] is the same vocabulary every other global release uses, so
     * "the session ended" and "the link dropped" stay distinguishable after the
     * fact.
     */
    data class Cleared(val controlIds: Set<String>, val reason: TouchReleaseReason) : TouchLatchEvent
}

/**
 * Host-side observer for latch transitions.
 *
 * Deliberately shaped like [TouchFeedbackBackend]: the portable engine knows
 * when something happened, and the host knows which of its own diagnostic
 * facilities should hear about it.
 */
fun interface TouchLatchObserver {
    fun onLatchEvent(event: TouchLatchEvent)

    companion object {
        val None = TouchLatchObserver { }
    }
}
