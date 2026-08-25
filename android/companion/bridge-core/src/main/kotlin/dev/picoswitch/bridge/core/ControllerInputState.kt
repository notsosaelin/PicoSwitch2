package dev.picoswitch.bridge.core

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * The platform-neutral controller state machine.
 *
 * A platform backend translates its own events into calls on this object; this
 * object owns everything that is the same on every platform:
 *
 * - which buttons are held, and from which independent origin,
 * - merging D-pad keys with a D-pad/hat axis pair,
 * - applying the face-layout mapping,
 * - publishing one complete [ControllerState] snapshot per input event,
 * - neutralizing on every boundary (source change, layout change, teardown).
 *
 * ## Why buttons are held AS REPORTED
 *
 * [pressButton] takes the button exactly as the platform named it, before the
 * face layout is applied, and the layout is applied at publish time. That is
 * what makes a layout change safe while keys are held: the same physical key
 * resolves to its new logical meaning instead of a stale one being stuck down.
 * (The state is neutralized on a layout change anyway — belt and braces, because
 * a stuck button on a console is one of the worst failure modes this bridge has.)
 *
 * ## Why touch buttons are a separate set
 *
 * A physical key and an on-screen press are independent origins for the same
 * logical button; releasing one must not cancel the other. Virtual buttons are
 * also NOT gated on a selected input source: they belong to the host device
 * itself and stay usable when the host has no gamepad to select. Most host
 * devices have no Home, Capture or C/GameChat key, so for those three the
 * virtual path is the only path.
 *
 * ## The three gameplay origins
 *
 * ```text
 * physical controls   ---\
 *                         >--  [authority] picks ONE  --\
 * on-screen controller ---/                              >-- published state
 * software/meta buttons  ------- always contribute ------/
 * ```
 *
 * [authority] is the explicit answer to "which host control set is the
 * controller right now". A merge would be indefensible — a physical stick left
 * and a touch stick right have no combined meaning — so the inactive origin's
 * mutations are discarded rather than retained, and switching authority
 * neutralizes. See [InputAuthority].
 *
 * Not thread-safe by design; drive it from one input thread, exactly as every
 * platform delivers input events.
 */
class ControllerInputState {
    private val _state = MutableStateFlow(ControllerState.Neutral)
    val state: StateFlow<ControllerState> = _state.asStateFlow()

    var source: ControllerSourceIdentity? = null
        private set
    var requestedLayout: ControllerFaceLayout = ControllerFaceLayout.Auto
        private set
    var resolvedLayout: ResolvedControllerLayout =
        ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, null)
        private set

    /** Which host control set drives gameplay input. See [InputAuthority]. */
    var authority: InputAuthority = InputAuthority.Physical
        private set

    private val heldPhysicalButtons = mutableSetOf<ControllerButton>()
    private val virtualButtons = mutableSetOf<ControllerButton>()
    private var keyDpad = DpadState.None
    private var hatDpad = DpadState.None
    private var physicalAnalog = NEUTRAL_ANALOG
    private var touch = TouchContribution.Neutral

    /**
     * The on-screen controller's current contribution, whether or not it is
     * currently authoritative.
     *
     * Exposed so a diagnostic can distinguish "touch is holding nothing" from
     * "touch is holding something the authority is discarding" — two states that
     * look identical in the published [state] and mean completely different
     * things when a control appears not to work.
     */
    val touchContribution: TouchContribution get() = touch

    /**
     * Point the state machine at a new input source, or at none.
     *
     * Re-resolves the face layout for the new identity and neutralizes, because
     * nothing held on the previous source has any meaning on this one.
     */
    fun setSource(identity: ControllerSourceIdentity?) {
        source = identity
        resolvedLayout = ControllerLayoutResolver.resolve(requestedLayout, identity)
        neutralize()
    }

    /** Apply the user's layout preference. Held input is cleared; see the class doc. */
    fun setRequestedLayout(layout: ControllerFaceLayout) {
        requestedLayout = layout
        resolvedLayout = ControllerLayoutResolver.resolve(layout, source)
        neutralize()
    }

    /**
     * Hand gameplay input to a different host control set.
     *
     * Always neutralizes, even when nothing appears to be held: a control that
     * was down at the moment of the switch belongs to the origin being left, and
     * carrying it across the boundary is exactly how a console ends up walking
     * into a wall after the user opened a different screen.
     */
    fun setAuthority(next: InputAuthority) {
        if (next == authority) return
        authority = next
        neutralize()
    }

    /**
     * A physical button, named exactly as the source device reported it.
     *
     * Face keys are still in the source's own dialect here — positional on an
     * Xbox-style pad, printed-legend on a Nintendo-labelled handheld — and are
     * translated by [ControllerLayoutResolver.mapPhysicalFaceKey] at publish time.
     */
    fun pressButton(reported: ControllerButton, pressed: Boolean) {
        if (authority != InputAuthority.Physical) return
        if (pressed) heldPhysicalButtons += reported else heldPhysicalButtons -= reported
        publish()
    }

    /** An on-screen / software button, already in logical bridge semantics. */
    fun setVirtualButton(button: ControllerButton, pressed: Boolean) {
        if (pressed) virtualButtons += button else virtualButtons -= button
        publish()
    }

    /** A discrete D-pad key. Merged with any hat axes; opposites cancel at encode time. */
    fun pressDpad(up: Boolean? = null, right: Boolean? = null, down: Boolean? = null, left: Boolean? = null) {
        if (authority != InputAuthority.Physical) return
        keyDpad = DpadState(
            up = up ?: keyDpad.up,
            right = right ?: keyDpad.right,
            down = down ?: keyDpad.down,
            left = left ?: keyDpad.left,
        )
        publish()
    }

    /**
     * One complete analog event: both sticks, both triggers, and the hat.
     *
     * Applied as a single state change so one physical event produces one
     * observable snapshot rather than three.
     */
    fun applyAnalog(frame: AnalogFrame) {
        if (authority != InputAuthority.Physical) return
        frame.dpad?.let { hatDpad = it }
        physicalAnalog = frame
        publish()
    }

    /**
     * One complete on-screen controller event: both sticks, both triggers, the
     * D-pad and every held button.
     *
     * Whole rather than incremental for the same reason as [applyAnalog]: one
     * contact event can change several controls at once, and a half-applied
     * snapshot must never be observable.
     */
    fun applyTouch(contribution: TouchContribution) {
        if (authority != InputAuthority.Touch) return
        touch = contribution
        publish()
    }

    /**
     * Drop every held input and publish neutral.
     *
     * Called on source change, layout change, authority change, link loss and
     * teardown. A held input that outlives its own boundary reaches the console
     * as a stuck button.
     */
    fun neutralize() {
        heldPhysicalButtons.clear()
        virtualButtons.clear()
        keyDpad = DpadState.None
        hatDpad = DpadState.None
        physicalAnalog = NEUTRAL_ANALOG
        touch = TouchContribution.Neutral
        _state.value = ControllerState.Neutral
    }

    /**
     * Compose the published snapshot from the currently authoritative gameplay
     * origin plus the always-allowed software/meta buttons.
     *
     * One function rather than one per input kind: every mutator ends here, so a
     * new origin cannot accidentally publish a state that omits another origin's
     * contribution. [MutableStateFlow] drops an unchanged value, so recomposing
     * the whole snapshot costs no extra emission.
     */
    private fun publish() {
        val logical = mutableSetOf<ControllerButton>()
        logical += virtualButtons

        val analog: AnalogFrame
        val dpad: DpadState
        // Each origin brings its face buttons in its own dialect and gets the
        // mapper for that dialect. They are NOT interchangeable: a physical key
        // needs the source device's legend corrected, an on-screen slot needs the
        // drawn presentation honoured, and those are opposite under the same
        // layout. See the ControllerLayout.kt header.
        when (authority) {
            InputAuthority.Physical -> {
                heldPhysicalButtons.mapTo(logical) {
                    ControllerLayoutResolver.mapPhysicalFaceKey(it, resolvedLayout.layout)
                }
                analog = physicalAnalog
                dpad = DpadState(
                    up = keyDpad.up || hatDpad.up,
                    right = keyDpad.right || hatDpad.right,
                    down = keyDpad.down || hatDpad.down,
                    left = keyDpad.left || hatDpad.left,
                )
            }
            InputAuthority.Touch -> {
                logical += touch.logicalButtons
                touch.positionalButtons.mapTo(logical) {
                    ControllerLayoutResolver.mapTouchFacePosition(it, resolvedLayout.layout)
                }
                analog = AnalogFrame(
                    leftX = touch.leftX, leftY = touch.leftY,
                    rightX = touch.rightX, rightY = touch.rightY,
                    leftTrigger = touch.leftTrigger, rightTrigger = touch.rightTrigger,
                )
                dpad = touch.dpad
            }
        }

        _state.value = ControllerState(
            leftX = analog.leftX, leftY = analog.leftY,
            rightX = analog.rightX, rightY = analog.rightY,
            leftTrigger = analog.leftTrigger, rightTrigger = analog.rightTrigger,
            buttons = logical,
            dpadUp = dpad.up, dpadRight = dpad.right,
            dpadDown = dpad.down, dpadLeft = dpad.left,
        )
    }

    private companion object {
        val NEUTRAL_ANALOG = AnalogFrame(
            leftX = 128, leftY = 128, rightX = 128, rightY = 128,
            leftTrigger = 0, rightTrigger = 0,
        )
    }
}
