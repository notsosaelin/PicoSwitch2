package dev.picoswitch.companion.controller

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class AndroidInputRouter {
    private val _state = MutableStateFlow(ControllerState.Neutral)
    val state: StateFlow<ControllerState> = _state.asStateFlow()
    var selectedDescriptor: String? = null
        private set
    var requestedFaceLayout: ControllerFaceLayout = ControllerFaceLayout.Auto
        private set
    var resolvedFaceLayout: ResolvedControllerLayout = ControllerLayoutResolver.resolve(requestedFaceLayout, null)
        private set
    private var selectedIdentity: ControllerSourceIdentity? = null
    private var keyUp = false
    private var keyRight = false
    private var keyDown = false
    private var keyLeft = false
    private var hatUp = false
    private var hatRight = false
    private var hatDown = false
    private var hatLeft = false
    private val heldButtonKeys = mutableSetOf<Int>()

    /**
     * Buttons held from the on-screen touch controls.
     *
     * Most Android handhelds have no Home, Capture, or C/GameChat button, so those
     * console functions are otherwise unreachable when the handheld is the active
     * controller. They are kept separate from [heldButtonKeys] because a physical
     * key and a touch press are independent sources for the same button: releasing
     * one must not cancel the other.
     */
    private val virtualButtons = mutableSetOf<ControllerButton>()

    /**
     * Every input device the app can see that plausibly relates to a controller,
     * including ones later excluded. Diagnostics shows the excluded entries with
     * their reason, so a wrongly-hidden device is identifiable from the field.
     */
    fun candidateDevices(): List<Pair<InputDevice, ControllerCandidate>> =
        InputDevice.getDeviceIds().asList().mapNotNull(InputDevice::getDevice)
            .filter {
                it.supportsSource(InputDevice.SOURCE_GAMEPAD) ||
                    it.supportsSource(InputDevice.SOURCE_JOYSTICK) ||
                    it.supportsSource(InputDevice.SOURCE_DPAD)
            }
            .map { it to it.toCandidate() }

    /** Devices that can actually serve as a controller source. */
    fun eligibleDevices(): List<InputDevice> =
        candidateDevices().filter { it.second.isUsable }.map { it.first }

    private fun InputDevice.toCandidate(): ControllerCandidate {
        val gamepadSource = supportsSource(InputDevice.SOURCE_GAMEPAD) ||
            supportsSource(InputDevice.SOURCE_JOYSTICK)
        // Sticks and triggers are the axes that make a device drivable; a hat
        // alone is not enough, since keyboard-like devices report D-pad hats.
        val stickAxes = intArrayOf(
            MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
            MotionEvent.AXIS_RX, MotionEvent.AXIS_RY,
            MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
            MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS,
        )
        val hasAxes = stickAxes.any { axis -> motionRanges.any { it.axis == axis } }
        val gamepadKeys = intArrayOf(
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
        )
        val hasButtons = runCatching { hasKeys(*gamepadKeys).any { it } }.getOrDefault(false)
        return ControllerCandidate(
            id = id,
            descriptor = descriptor,
            name = name.take(120),
            vendorId = vendorId,
            productId = productId,
            hasMotionAxes = hasAxes,
            hasGamepadButtons = hasButtons,
            isVirtual = isVirtual,
            hasGamepadSource = gamepadSource,
        )
    }

    fun select(device: InputDevice?) {
        selectedDescriptor = device?.descriptor
        selectedIdentity = device?.let {
            ControllerSourceIdentity(it.descriptor, it.name, it.vendorId, it.productId)
        }
        resolvedFaceLayout = ControllerLayoutResolver.resolve(requestedFaceLayout, selectedIdentity)
        clearDpad()
        heldButtonKeys.clear()
        // The published state resets to neutral here, so the touch set has to reset
        // with it or a held button would survive only in this object.
        virtualButtons.clear()
        _state.value = ControllerState.Neutral
    }

    fun setFaceLayout(layout: ControllerFaceLayout) {
        requestedFaceLayout = layout
        resolvedFaceLayout = ControllerLayoutResolver.resolve(layout, selectedIdentity)
        // A key held while the mapping changes must never survive under its old semantic.
        neutralize()
    }

    fun onKey(event: KeyEvent): Boolean {
        val device = event.device ?: return false
        if (device.descriptor != selectedDescriptor || event.repeatCount > 0) return false
        val pressed = event.action == KeyEvent.ACTION_DOWN
        val button = buttonForKey(event.keyCode)
        if (button != null) {
            if (pressed) heldButtonKeys += event.keyCode else heldButtonKeys -= event.keyCode
            publishButtons()
            return true
        }
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> keyUp = pressed
            KeyEvent.KEYCODE_DPAD_RIGHT -> keyRight = pressed
            KeyEvent.KEYCODE_DPAD_DOWN -> keyDown = pressed
            KeyEvent.KEYCODE_DPAD_LEFT -> keyLeft = pressed
            else -> return false
        }
        publishDpad()
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        val device = event.device ?: return false
        val controllerMotion = event.isFromSource(InputDevice.SOURCE_JOYSTICK) ||
            event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_DPAD)
        if (device.descriptor != selectedDescriptor || !controllerMotion) return false
        fun range(axis: Int) = device.getMotionRange(axis, event.source)?.let { AxisRange(it.min, it.max, it.flat) }
        fun stick(axis: Int, invert: Boolean = false, fallback: Int? = null): Int {
            val actual = if (range(axis) != null) axis else fallback ?: axis
            return range(actual)?.stick(event.getAxisValue(actual), invert) ?: 128
        }
        fun trigger(primary: Int, fallback: Int): Int {
            val actual = if (range(primary) != null) primary else fallback
            return range(actual)?.trigger(event.getAxisValue(actual)) ?: 0
        }
        if (range(MotionEvent.AXIS_HAT_X) != null) {
            val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            hatLeft = hatX < -0.5f
            hatRight = hatX > 0.5f
        }
        if (range(MotionEvent.AXIS_HAT_Y) != null) {
            val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
            hatUp = hatY < -0.5f
            hatDown = hatY > 0.5f
        }
        _state.value = _state.value.copy(
            leftX = stick(MotionEvent.AXIS_X), leftY = stick(MotionEvent.AXIS_Y),
            rightX = stick(MotionEvent.AXIS_Z, fallback = MotionEvent.AXIS_RX),
            rightY = stick(MotionEvent.AXIS_RZ, fallback = MotionEvent.AXIS_RY),
            leftTrigger = trigger(MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE),
            rightTrigger = trigger(MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS),
            dpadLeft = keyLeft || hatLeft,
            dpadRight = keyRight || hatRight,
            dpadUp = keyUp || hatUp,
            dpadDown = keyDown || hatDown,
        )
        return true
    }

    /**
     * Press or release a button from the on-screen controls.
     *
     * Unlike physical keys this is not gated on a selected input device: the touch
     * controls belong to the handheld itself, and they stay usable even when the
     * handheld has no built-in gamepad to select.
     */
    fun setVirtualButton(button: ControllerButton, pressed: Boolean) {
        if (pressed) virtualButtons += button else virtualButtons -= button
        publishButtons()
    }

    private fun publishButtons() {
        val buttons = ControllerButton.entries.filterTo(mutableSetOf()) { candidate ->
            candidate in virtualButtons ||
                heldButtonKeys.any { keyCode -> buttonForKey(keyCode) == candidate }
        }
        _state.value = _state.value.copy(buttons = buttons)
    }

    fun neutralize() {
        clearDpad(); heldButtonKeys.clear(); virtualButtons.clear()
        _state.value = ControllerState.Neutral
    }

    private fun buttonForKey(keyCode: Int): ControllerButton? {
        val positional = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> ControllerButton.A
        KeyEvent.KEYCODE_BUTTON_B -> ControllerButton.B
        KeyEvent.KEYCODE_BUTTON_X -> ControllerButton.X
        KeyEvent.KEYCODE_BUTTON_Y -> ControllerButton.Y
        KeyEvent.KEYCODE_BUTTON_L1 -> ControllerButton.L1
        KeyEvent.KEYCODE_BUTTON_R1 -> ControllerButton.R1
        KeyEvent.KEYCODE_BUTTON_L2 -> ControllerButton.L2
        KeyEvent.KEYCODE_BUTTON_R2 -> ControllerButton.R2
        KeyEvent.KEYCODE_BUTTON_SELECT -> ControllerButton.Select
        KeyEvent.KEYCODE_BUTTON_START -> ControllerButton.Start
        KeyEvent.KEYCODE_BUTTON_THUMBL -> ControllerButton.LeftStick
        KeyEvent.KEYCODE_BUTTON_THUMBR -> ControllerButton.RightStick
        KeyEvent.KEYCODE_BUTTON_MODE -> ControllerButton.Home
        KeyEvent.KEYCODE_BUTTON_C, KeyEvent.KEYCODE_BUTTON_Z -> ControllerButton.Capture
        else -> null
        }
        return positional?.let { ControllerLayoutResolver.mapFaceButton(it, resolvedFaceLayout.layout) }
    }

    private fun publishDpad() {
        _state.value = _state.value.copy(
            dpadUp = keyUp || hatUp, dpadRight = keyRight || hatRight,
            dpadDown = keyDown || hatDown, dpadLeft = keyLeft || hatLeft,
        )
    }

    private fun clearDpad() {
        keyUp = false; keyRight = false; keyDown = false; keyLeft = false
        hatUp = false; hatRight = false; hatDown = false; hatLeft = false
    }

    private fun InputDevice.supportsSource(source: Int) = sources and source == source
}
