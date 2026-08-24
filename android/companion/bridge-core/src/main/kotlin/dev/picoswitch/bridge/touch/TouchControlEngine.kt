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
) {
    var config: TouchControlConfig = config
        private set

    private var layout: ResolvedTouchLayout = ResolvedTouchLayout.Empty

    private val contactToControl = mutableMapOf<Long, String>()
    private val controlToContact = mutableMapOf<String, Long>()

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

    /** What touch is holding right now, independent of whether it is authoritative. */
    val contribution: TouchContribution get() = published

    fun setFeedbackBackend(backend: TouchFeedbackBackend) {
        feedback = backend
    }

    fun setConfig(next: TouchControlConfig) {
        config = next
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
            else -> Unit
        }
        return true
    }

    private fun onEnd(contact: TouchContact, cancelled: Boolean): Boolean {
        if (cancelled) contactsCancelled++
        val controlId = contactToControl.remove(contact.id) ?: return false
        controlToContact.remove(controlId)
        val control = layout.control(controlId)
        if (control != null) {
            disengage(control)
            if (!cancelled) {
                // Buttons only. A stick or D-pad release is the end of a
                // continuous gesture, and buzzing there turns ordinary play into
                // a rattle.
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
        contactToControl.clear()
        controlToContact.clear()
        facePresses.clear()
        logicalPresses.clear()
        leftStick = TouchVector.Zero
        rightStick = TouchVector.Zero
        leftTrigger = 0f
        rightTrigger = 0f
        dpad = DpadState.None
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
        lastContactTimeNanos = lastContactTimeNanos,
        leftStick = leftStick,
        rightStick = rightStick,
        dpad = dpad,
    )

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

    private fun publish() {
        val next = TouchContribution(
            leftX = TouchAxis.toBridge(leftStick.x),
            leftY = TouchAxis.toBridge(leftStick.y),
            rightX = TouchAxis.toBridge(rightStick.x),
            rightY = TouchAxis.toBridge(rightStick.y),
            leftTrigger = TouchAxis.triggerToBridge(leftTrigger),
            rightTrigger = TouchAxis.triggerToBridge(rightTrigger),
            dpad = dpad,
            positionalButtons = facePresses.mapTo(mutableSetOf()) { it.positional },
            logicalButtons = logicalPresses.toSet(),
        )
        if (next == published) return
        published = next
        onContribution(next)
    }
}
