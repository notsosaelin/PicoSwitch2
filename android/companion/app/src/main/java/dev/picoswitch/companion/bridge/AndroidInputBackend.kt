package dev.picoswitch.companion.bridge

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import dev.picoswitch.bridge.core.AnalogFrame
import dev.picoswitch.bridge.core.AxisRange
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerCandidate
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerSourceIdentity
import dev.picoswitch.bridge.core.DeviceCapabilities
import dev.picoswitch.bridge.core.DpadState
import dev.picoswitch.bridge.core.InputAuthority

/**
 * Android input APIs -> the bridge's normalized controller state.
 *
 * This is the ANDROID half of the input path and nothing else. It knows about
 * `InputDevice`, key codes, sources and `MotionRange`; it produces bridge
 * semantics. [ControllerInputState] owns everything after that — held state,
 * D-pad merging, layout application, publishing — and knows none of this.
 *
 * A Windows or Linux backend implements this same shape against its own APIs and
 * reuses the identical state machine, encoder and session.
 */
class AndroidInputBackend(
    /** The shared state machine this backend feeds. Injectable for tests. */
    val controller: ControllerInputState = ControllerInputState(),
) {
    val state = controller.state

    var selectedDescriptor: String? = null
        private set
    private var selectedDevice: InputDevice? = null

    val requestedFaceLayout: ControllerFaceLayout get() = controller.requestedLayout
    val resolvedFaceLayout get() = controller.resolvedLayout
    val selectedSource: ControllerSourceIdentity? get() = controller.source

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
        val hasAxes = STICK_AXES.any { axis -> motionRanges.any { it.axis == axis } }
        val hasButtons = runCatching { hasKeys(*GAMEPAD_KEYS).any { it } }.getOrDefault(false)
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

    /**
     * What the selected source can do, in bridge terms.
     *
     * Only the SOURCE half of the capability model; the host half (IMU,
     * actuators, battery) is filled in by the session from its own backends.
     */
    fun sourceCapabilities(): DeviceCapabilities {
        val device = selectedDevice ?: return DeviceCapabilities.None
        fun has(axis: Int) = device.motionRanges.any { it.axis == axis }
        val sticks = listOf(
            has(MotionEvent.AXIS_X) || has(MotionEvent.AXIS_Y),
            has(MotionEvent.AXIS_Z) || has(MotionEvent.AXIS_RZ) ||
                has(MotionEvent.AXIS_RX) || has(MotionEvent.AXIS_RY),
        ).count { it }
        return DeviceCapabilities(
            gamepadButtons = runCatching { device.hasKeys(*GAMEPAD_KEYS).any { it } }.getOrDefault(false),
            analogSticks = sticks,
            analogTriggers = has(MotionEvent.AXIS_LTRIGGER) || has(MotionEvent.AXIS_RTRIGGER) ||
                has(MotionEvent.AXIS_BRAKE) || has(MotionEvent.AXIS_GAS),
            dpad = has(MotionEvent.AXIS_HAT_X) || has(MotionEvent.AXIS_HAT_Y) ||
                runCatching { device.hasKeys(KeyEvent.KEYCODE_DPAD_UP).any { it } }.getOrDefault(false),
        )
    }

    fun select(device: InputDevice?) {
        selectedDevice = device
        selectedDescriptor = device?.descriptor
        controller.setSource(
            device?.let { ControllerSourceIdentity(it.descriptor, it.name, it.vendorId, it.productId) },
        )
    }

    fun setFaceLayout(layout: ControllerFaceLayout) = controller.setRequestedLayout(layout)

    /**
     * What the ON-SCREEN controller can do.
     *
     * A touchscreen has no `InputDevice` descriptor, so [sourceCapabilities]
     * correctly reports nothing for it — but "nothing" is the wrong answer for a
     * host whose screen IS the controller. This states the truth about the
     * on-screen controller instead, in exactly the same half of the capability
     * model a physical source fills in: the session still composes the host's own
     * IMU, actuators and battery on top.
     *
     * Deliberately NOT solved by inventing a synthetic input device. Something
     * downstream always ends up trying to resolve a descriptor back to a real
     * device, and a fabricated one resolves to nothing at exactly the moment it
     * matters.
     */
    val touchCapabilities: DeviceCapabilities = DeviceCapabilities(
        gamepadButtons = true,
        analogSticks = 2,
        // The on-screen triggers publish a full-scale analog value alongside
        // their digital bit, matching what a physical trigger produces.
        analogTriggers = true,
        dpad = true,
    )

    /**
     * Press or release a button from the on-screen controls.
     *
     * Deliberately delegated straight through and NOT gated on a selected input
     * device: the touch controls belong to the handheld itself and stay usable
     * even when it has no built-in gamepad to select.
     */
    fun setVirtualButton(button: ControllerButton, pressed: Boolean) =
        controller.setVirtualButton(button, pressed)

    fun neutralize() = controller.neutralize()

    fun onKey(event: KeyEvent): Boolean {
        // The state machine would discard these anyway while the on-screen
        // controller is authoritative. Declining them here as well means they are
        // not silently swallowed on the way to a system that might still want
        // them, and "not consumed" stays an honest answer.
        if (controller.authority != InputAuthority.Physical) return false
        val device = event.device ?: return false
        if (device.descriptor != selectedDescriptor || event.repeatCount > 0) return false
        val pressed = event.action == KeyEvent.ACTION_DOWN
        val button = positionalButtonForKey(event.keyCode)
        if (button != null) {
            controller.pressButton(button, pressed)
            return true
        }
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> controller.pressDpad(up = pressed)
            KeyEvent.KEYCODE_DPAD_RIGHT -> controller.pressDpad(right = pressed)
            KeyEvent.KEYCODE_DPAD_DOWN -> controller.pressDpad(down = pressed)
            KeyEvent.KEYCODE_DPAD_LEFT -> controller.pressDpad(left = pressed)
            else -> return false
        }
        return true
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (controller.authority != InputAuthority.Physical) return false
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

        // Null when the source has no hat axes at all, which the state machine
        // reads as "leave the hat contribution alone" rather than "centered".
        val hasHatX = range(MotionEvent.AXIS_HAT_X) != null
        val hasHatY = range(MotionEvent.AXIS_HAT_Y) != null
        val dpad = if (hasHatX || hasHatY) {
            DpadState.fromAxes(
                x = if (hasHatX) event.getAxisValue(MotionEvent.AXIS_HAT_X) else 0f,
                y = if (hasHatY) event.getAxisValue(MotionEvent.AXIS_HAT_Y) else 0f,
            )
        } else {
            null
        }

        controller.applyAnalog(
            AnalogFrame(
                leftX = stick(MotionEvent.AXIS_X), leftY = stick(MotionEvent.AXIS_Y),
                rightX = stick(MotionEvent.AXIS_Z, fallback = MotionEvent.AXIS_RX),
                rightY = stick(MotionEvent.AXIS_RZ, fallback = MotionEvent.AXIS_RY),
                leftTrigger = trigger(MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE),
                rightTrigger = trigger(MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS),
                dpad = dpad,
            ),
        )
        return true
    }

    private fun InputDevice.supportsSource(source: Int) = sources and source == source

    companion object {
        private val STICK_AXES = intArrayOf(
            MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
            MotionEvent.AXIS_RX, MotionEvent.AXIS_RY,
            MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
            MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS,
        )

        private val GAMEPAD_KEYS = intArrayOf(
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
        )

        /**
         * Android key code -> POSITIONAL controller button, before any face-layout
         * resolution. Pure, so the mapping is pinned by tests rather than needing a
         * real InputDevice.
         *
         * DELIBERATELY INCOMPLETE. Only the standard controls this project has an
         * intentional meaning for are listed. Anything else returns null and is
         * ignored.
         *
         * RULE: unknown or additional physical controller buttons are preserved as
         * candidates for future custom mapping rather than silently assigned to
         * unrelated controller actions.
         *
         * `KEYCODE_BUTTON_C` and `KEYCODE_BUTTON_Z` used to be routed here to
         * Capture. That was arbitrary -- they are extra physical buttons on some
         * handhelds and pads, not Capture keys -- so it produced surprising
         * behavior AND consumed two inputs that the eventual custom-mapping system
         * should own. Removed 2026-08-15. Do not reassign them, including to C /
         * GameChat: C is reachable as an on-screen button and has no physical key
         * on this hardware.
         *
         * Capture consequently has no physical key by default. That is correct:
         * the ADB audits found no dedicated Capture key on either audited handheld,
         * and the on-screen button is how it is reached.
         */
        internal fun positionalButtonForKey(keyCode: Int): ControllerButton? = when (keyCode) {
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
            else -> null
        }
    }
}
