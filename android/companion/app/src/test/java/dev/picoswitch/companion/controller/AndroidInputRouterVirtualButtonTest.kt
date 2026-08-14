package dev.picoswitch.companion.controller

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * On-screen console buttons.
 *
 * The Switch has Home and Capture; most Android handhelds do not, so those
 * functions are only reachable through the touch controls while the handheld is
 * the active controller. These are pure state tests: they need no InputDevice,
 * which is exactly why the touch path is kept independent of device selection.
 */
class AndroidInputRouterVirtualButtonTest {

    @Test
    fun `virtual press publishes the button and release clears it`() {
        val router = AndroidInputRouter()

        router.setVirtualButton(ControllerButton.Home, true)
        assertTrue(ControllerButton.Home in router.state.value.buttons)

        router.setVirtualButton(ControllerButton.Home, false)
        assertTrue(router.state.value.buttons.isEmpty())
    }

    @Test
    fun `independent buttons coexist and release individually`() {
        val router = AndroidInputRouter()

        router.setVirtualButton(ControllerButton.Home, true)
        router.setVirtualButton(ControllerButton.Capture, true)
        assertEquals(
            setOf(ControllerButton.Home, ControllerButton.Capture),
            router.state.value.buttons,
        )

        router.setVirtualButton(ControllerButton.Home, false)
        assertEquals(setOf(ControllerButton.Capture), router.state.value.buttons)
    }

    @Test
    fun `touch buttons work with no input device selected`() {
        // A handheld with no built-in gamepad still has to reach Home and Capture,
        // so the touch path must not be gated on a selected physical device.
        val router = AndroidInputRouter()
        router.select(null)

        router.setVirtualButton(ControllerButton.Capture, true)
        assertTrue(ControllerButton.Capture in router.state.value.buttons)
    }

    @Test
    fun `neutralize releases a held touch button`() {
        // A dropped link neutralizes the router. A button still held in the touch
        // set would otherwise reappear as pressed on the next publish.
        val router = AndroidInputRouter()
        router.setVirtualButton(ControllerButton.Home, true)

        router.neutralize()
        assertTrue(router.state.value.buttons.isEmpty())

        router.setVirtualButton(ControllerButton.Capture, true)
        assertEquals(setOf(ControllerButton.Capture), router.state.value.buttons)
    }

    @Test
    fun `selecting a device releases a held touch button`() {
        // select() republishes neutral, so the touch set has to reset with it or the
        // two would disagree about what is held.
        val router = AndroidInputRouter()
        router.setVirtualButton(ControllerButton.Home, true)

        router.select(null)
        assertTrue(router.state.value.buttons.isEmpty())
    }
}
