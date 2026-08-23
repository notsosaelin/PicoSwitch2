package dev.picoswitch.companion.bridge

import android.view.KeyEvent
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerState
import dev.picoswitch.bridge.core.FaceButtonPosition
import dev.picoswitch.bridge.core.InputAuthority
import dev.picoswitch.bridge.core.TouchContribution
import dev.picoswitch.bridge.touch.ResolvedTouchLayout
import dev.picoswitch.bridge.touch.TouchContact
import dev.picoswitch.bridge.touch.TouchGamepad
import dev.picoswitch.bridge.touch.TouchLayoutRegion
import dev.picoswitch.bridge.touch.TouchLayoutResolver
import dev.picoswitch.bridge.touch.TouchLayoutV1
import dev.picoswitch.bridge.touch.TouchPhase
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The Android half of the on-screen controller, where it can be reached without
 * a device.
 *
 * [AndroidInputBackend] is constructible in a plain JVM test because it only
 * touches platform APIs inside its event handlers, which is itself the property
 * worth keeping: everything about what the on-screen controller CAN do and when
 * physical input is allowed through is decidable without an `InputDevice`.
 */
class TouchGamepadBackendTest {

    private fun resolvedLayout() = TouchLayoutResolver.resolve(
        TouchLayoutV1.layout,
        TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
    )

    // -------------------------------------------------------------- capabilities

    /**
     * A touchscreen has no input-device descriptor, so the physical capability
     * probe correctly reports nothing for it. "Nothing" is the wrong answer for a
     * host whose screen IS the controller, and this is the honest one.
     */
    @Test fun `the on-screen controller reports a complete controller`() {
        val backend = AndroidInputBackend()
        val capabilities = backend.touchCapabilities
        assertTrue(capabilities.gamepadButtons)
        assertEquals(2, capabilities.analogSticks)
        assertTrue(capabilities.analogTriggers)
        assertTrue(capabilities.dpad)
    }

    /**
     * Only the SOURCE half. The host's own IMU, actuators and battery are the
     * session's to fill in, and claiming them here would be a second answer to a
     * question something else already owns.
     */
    @Test fun `the on-screen controller claims nothing about the host itself`() {
        val capabilities = AndroidInputBackend().touchCapabilities
        assertFalse(capabilities.gyroscope)
        assertFalse(capabilities.accelerometer)
        assertFalse(capabilities.battery)
        assertEquals(0, capabilities.rumbleMotors)
    }

    // ------------------------------------------------------------- authority gate

    /**
     * Physical events are declined rather than swallowed while the on-screen
     * controller is authoritative. The state machine would discard them anyway;
     * returning false as well keeps "not consumed" an honest answer to whatever
     * is behind this app.
     */
    @Test fun `physical key and motion events are declined under touch authority`() {
        val backend = AndroidInputBackend()
        backend.controller.setAuthority(InputAuthority.Touch)
        // A null device would also decline, so the assertion that matters is the
        // published state: nothing physical may reach it.
        assertFalse(backend.onKey(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)))
        assertEquals(ControllerState.Neutral, backend.state.value)
    }

    @Test fun `virtual buttons still work under touch authority`() {
        val backend = AndroidInputBackend()
        backend.controller.setAuthority(InputAuthority.Touch)
        backend.setVirtualButton(ControllerButton.Home, true)
        assertEquals(setOf(ControllerButton.Home), backend.state.value.buttons)
    }

    // ---------------------------------------------------------- face presentation

    /**
     * Auto exists to guess a PRINTED legend. A drawn diamond has none, so
     * inheriting Auto would let the on-screen letters be decided by which pad
     * happened to be connected — or, with none connected, by the positional
     * fallback, which is not what a Switch controller looks like.
     */
    @Test fun `the on-screen face presentation never resolves to Auto`() {
        assertEquals(
            ControllerFaceLayout.Nintendo,
            AndroidControllerLayoutStore.touchLayoutFrom(null),
        )
        assertEquals(
            ControllerFaceLayout.Nintendo,
            AndroidControllerLayoutStore.touchLayoutFrom(ControllerFaceLayout.Auto.key),
        )
        assertEquals(
            ControllerFaceLayout.Xbox,
            AndroidControllerLayoutStore.touchLayoutFrom(ControllerFaceLayout.Xbox.key),
        )
        assertEquals(
            ControllerFaceLayout.Nintendo,
            AndroidControllerLayoutStore.touchLayoutFrom("nonsense-from-an-older-build"),
        )
    }

    /**
     * The default draws a Switch controller's letters AND sends what they say.
     * The two come from the same resolver, so they cannot disagree.
     */
    @Test fun `the default presentation sends what it draws`() {
        val backend = AndroidInputBackend()
        backend.setFaceLayout(AndroidControllerLayoutStore.DEFAULT_TOUCH_LAYOUT)
        backend.controller.setAuthority(InputAuthority.Touch)

        backend.controller.applyTouch(
            TouchContribution(positionalButtons = setOf(FaceButtonPosition.South.positional)),
        )
        assertEquals(setOf(ControllerButton.B), backend.state.value.buttons)

        backend.controller.applyTouch(
            TouchContribution(positionalButtons = setOf(FaceButtonPosition.East.positional)),
        )
        assertEquals(setOf(ControllerButton.A), backend.state.value.buttons)
    }

    // -------------------------------------------------------- end-to-end, no device

    /**
     * The whole host-side path, minus the platform event source: contacts in,
     * normalized controller state out. Nothing between them needs a device.
     */
    @Test fun `contacts become normalized controller state`() {
        val backend = AndroidInputBackend()
        backend.setFaceLayout(ControllerFaceLayout.Xbox)
        val gamepad = TouchGamepad(backend.controller)
        val layout = resolvedLayout()
        gamepad.engine.setLayout(layout)
        gamepad.activate()

        val stick = layout.control(TouchLayoutV1.STICK_LEFT)!!
        val trigger = layout.control(TouchLayoutV1.TRIGGER_RIGHT)!!
        val face = layout.control(TouchLayoutV1.FACE_SOUTH)!!

        gamepad.contacts.dispatch(
            listOf(
                TouchContact(31, TouchPhase.Down, stick.centerX, stick.centerY),
                TouchContact(4, TouchPhase.Down, trigger.centerX, trigger.centerY),
                TouchContact(77, TouchPhase.Down, face.centerX, face.centerY),
            ),
        )
        gamepad.contacts.dispatch(
            listOf(
                TouchContact(31, TouchPhase.Move, stick.centerX, stick.centerY - stick.trackingRadius),
                TouchContact(4, TouchPhase.Move, trigger.centerX, trigger.centerY),
                TouchContact(77, TouchPhase.Move, face.centerX, face.centerY),
            ),
        )

        val state = backend.state.value
        assertEquals(1, state.leftY)
        assertEquals(255, state.rightTrigger)
        assertEquals(setOf(ControllerButton.A, ControllerButton.R2), state.buttons)
    }

    @Test fun `an unmeasured layout accepts no contacts`() {
        val backend = AndroidInputBackend()
        val gamepad = TouchGamepad(backend.controller)
        gamepad.engine.setLayout(ResolvedTouchLayout.Empty)
        gamepad.activate()
        gamepad.contacts.dispatch(listOf(TouchContact(1, TouchPhase.Down, 10f, 10f)))
        assertEquals(ControllerState.Neutral, backend.state.value)
    }
}
