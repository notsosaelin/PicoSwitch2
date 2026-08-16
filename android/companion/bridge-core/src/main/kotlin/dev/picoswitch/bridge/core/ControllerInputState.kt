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
 * ## Why buttons are held POSITIONALLY
 *
 * [pressButton] takes the button in its physical POSITION, before the face
 * layout is applied, and the layout is applied at publish time. That is what
 * makes a layout change safe while keys are held: the same physical key resolves
 * to its new logical meaning instead of a stale one being stuck down. (The state
 * is neutralized on a layout change anyway — belt and braces, because a stuck
 * button on a console is one of the worst failure modes this bridge has.)
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

    private val heldPositionalButtons = mutableSetOf<ControllerButton>()
    private val virtualButtons = mutableSetOf<ControllerButton>()
    private var keyDpad = DpadState.None
    private var hatDpad = DpadState.None

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

    /** A physical button, identified by its POSITION on the pad. */
    fun pressButton(positional: ControllerButton, pressed: Boolean) {
        if (pressed) heldPositionalButtons += positional else heldPositionalButtons -= positional
        publishButtons()
    }

    /** An on-screen / software button, already in logical bridge semantics. */
    fun setVirtualButton(button: ControllerButton, pressed: Boolean) {
        if (pressed) virtualButtons += button else virtualButtons -= button
        publishButtons()
    }

    /** A discrete D-pad key. Merged with any hat axes; opposites cancel at encode time. */
    fun pressDpad(up: Boolean? = null, right: Boolean? = null, down: Boolean? = null, left: Boolean? = null) {
        keyDpad = DpadState(
            up = up ?: keyDpad.up,
            right = right ?: keyDpad.right,
            down = down ?: keyDpad.down,
            left = left ?: keyDpad.left,
        )
        publishDpad()
    }

    /**
     * One complete analog event: both sticks, both triggers, and the hat.
     *
     * Applied as a single state change so one physical event produces one
     * observable snapshot rather than three.
     */
    fun applyAnalog(frame: AnalogFrame) {
        frame.dpad?.let { hatDpad = it }
        _state.value = _state.value.copy(
            leftX = frame.leftX, leftY = frame.leftY,
            rightX = frame.rightX, rightY = frame.rightY,
            leftTrigger = frame.leftTrigger, rightTrigger = frame.rightTrigger,
            dpadUp = keyDpad.up || hatDpad.up,
            dpadRight = keyDpad.right || hatDpad.right,
            dpadDown = keyDpad.down || hatDpad.down,
            dpadLeft = keyDpad.left || hatDpad.left,
        )
    }

    /**
     * Drop every held input and publish neutral.
     *
     * Called on source change, layout change, link loss and teardown. A held
     * input that outlives its own boundary reaches the console as a stuck button.
     */
    fun neutralize() {
        heldPositionalButtons.clear()
        virtualButtons.clear()
        keyDpad = DpadState.None
        hatDpad = DpadState.None
        _state.value = ControllerState.Neutral
    }

    private fun publishButtons() {
        val logical = mutableSetOf<ControllerButton>()
        logical += virtualButtons
        heldPositionalButtons.mapTo(logical) {
            ControllerLayoutResolver.mapFaceButton(it, resolvedLayout.layout)
        }
        _state.value = _state.value.copy(buttons = logical)
    }

    private fun publishDpad() {
        _state.value = _state.value.copy(
            dpadUp = keyDpad.up || hatDpad.up,
            dpadRight = keyDpad.right || hatDpad.right,
            dpadDown = keyDpad.down || hatDpad.down,
            dpadLeft = keyDpad.left || hatDpad.left,
        )
    }
}
