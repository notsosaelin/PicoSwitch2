package dev.picoswitch.bridge.core

import dev.picoswitch.bridge.protocol.ControllerReportEncoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The platform-neutral input state machine.
 *
 * These tests were written against the Android router and now run with no
 * Android at all, which is the point: every rule below is bridge behavior, and a
 * Windows or Linux backend inherits it rather than reimplementing it.
 */
class ControllerInputStateTest {

    // ---------------------------------------------------------- virtual buttons

    /**
     * The Switch has Home, Capture and C/GameChat; most host devices have no key
     * for any of them, so those functions are only reachable through on-screen
     * controls. Pure state, needing no input device — which is exactly why the
     * touch path is kept independent of device selection.
     */
    @Test fun `virtual press publishes the button and release clears it`() {
        val input = ControllerInputState()

        input.setVirtualButton(ControllerButton.Home, true)
        assertTrue(ControllerButton.Home in input.state.value.buttons)

        input.setVirtualButton(ControllerButton.Home, false)
        assertTrue(input.state.value.buttons.isEmpty())
    }

    @Test fun `independent buttons coexist and release individually`() {
        val input = ControllerInputState()

        input.setVirtualButton(ControllerButton.Home, true)
        input.setVirtualButton(ControllerButton.Capture, true)
        assertEquals(
            setOf(ControllerButton.Home, ControllerButton.Capture),
            input.state.value.buttons,
        )

        input.setVirtualButton(ControllerButton.Home, false)
        assertEquals(setOf(ControllerButton.Capture), input.state.value.buttons)
    }

    @Test fun `touch buttons work with no input source selected`() {
        // A host with no built-in gamepad still has to reach Home and Capture, so
        // the touch path must not be gated on a selected physical device.
        val input = ControllerInputState()
        input.setSource(null)

        input.setVirtualButton(ControllerButton.Capture, true)
        assertTrue(ControllerButton.Capture in input.state.value.buttons)
    }

    @Test fun `neutralize releases a held touch button`() {
        // A dropped link neutralizes the state machine. A button still held in the
        // touch set would otherwise reappear as pressed on the next publish.
        val input = ControllerInputState()
        input.setVirtualButton(ControllerButton.Home, true)

        input.neutralize()
        assertTrue(input.state.value.buttons.isEmpty())

        input.setVirtualButton(ControllerButton.Capture, true)
        assertEquals(setOf(ControllerButton.Capture), input.state.value.buttons)
    }

    @Test fun `selecting a source releases a held touch button`() {
        val input = ControllerInputState()
        input.setVirtualButton(ControllerButton.Home, true)

        input.setSource(null)
        assertTrue(input.state.value.buttons.isEmpty())
    }

    @Test fun `C reaches the published state and encodes to the fifteenth button bit`() {
        val input = ControllerInputState()
        input.setVirtualButton(ControllerButton.C, true)
        assertTrue(ControllerButton.C in input.state.value.buttons)

        val report = ControllerReportEncoder.encode(input.state.value)
        // Payload byte 7 holds button bits 8..14; C is ordinal 14 -> bit 6 there.
        assertEquals(0x40, report[7].toInt() and 0xFF)
        assertEquals(0x00, report[6].toInt() and 0xFF)

        input.setVirtualButton(ControllerButton.C, false)
        assertFalse(ControllerButton.C in input.state.value.buttons)
    }

    @Test fun `C is independent of Home and Capture`() {
        val input = ControllerInputState()
        input.setVirtualButton(ControllerButton.Home, true)
        input.setVirtualButton(ControllerButton.C, true)
        assertEquals(
            setOf(ControllerButton.Home, ControllerButton.C),
            input.state.value.buttons,
        )
        input.setVirtualButton(ControllerButton.Home, false)
        assertEquals(setOf(ControllerButton.C), input.state.value.buttons)
    }

    // ---------------------------------------------------- layout is applied late

    /**
     * Buttons are held AS THE SOURCE REPORTED THEM and resolved through the
     * layout at publish time, so the backend never has to know the layout.
     *
     * A Nintendo-labelled handheld reports its printed letters, so `BUTTON_A` is
     * already the console's A. A positional source names its bottom button `A`,
     * and the console's bottom button is B.
     */
    @Test fun `layout is applied to reported buttons at publish time`() {
        val input = ControllerInputState()
        input.setRequestedLayout(ControllerFaceLayout.Nintendo)

        input.pressButton(ControllerButton.A, true)
        assertEquals(setOf(ControllerButton.A), input.state.value.buttons)

        input.setRequestedLayout(ControllerFaceLayout.Xbox)
        input.pressButton(ControllerButton.A, true)
        assertEquals(setOf(ControllerButton.B), input.state.value.buttons)
    }

    /**
     * A virtual button is already logical. Home has no positional twin, and
     * running it through the face mapper would be a category error waiting to
     * bite the day a virtual A/B button is added.
     */
    @Test fun `virtual buttons are not face swapped`() {
        val input = ControllerInputState()
        input.setRequestedLayout(ControllerFaceLayout.Nintendo)
        input.setVirtualButton(ControllerButton.Home, true)
        assertEquals(setOf(ControllerButton.Home), input.state.value.buttons)
    }

    @Test fun `changing layout clears held input rather than leaving it stuck`() {
        val input = ControllerInputState()
        input.pressButton(ControllerButton.A, true)
        input.setRequestedLayout(ControllerFaceLayout.Nintendo)
        assertTrue(input.state.value.buttons.isEmpty())
    }

    /**
     * A physical key and a touch press are independent origins for the same
     * button; releasing one must not cancel the other.
     */
    @Test fun `physical and virtual origins of the same button are independent`() {
        val input = ControllerInputState()
        input.pressButton(ControllerButton.Home, true)
        input.setVirtualButton(ControllerButton.Home, true)
        assertEquals(setOf(ControllerButton.Home), input.state.value.buttons)

        input.setVirtualButton(ControllerButton.Home, false)
        assertEquals(setOf(ControllerButton.Home), input.state.value.buttons)

        input.pressButton(ControllerButton.Home, false)
        assertTrue(input.state.value.buttons.isEmpty())
    }

    // ------------------------------------------------------------- D-pad merging

    @Test fun `keys and hat axes merge into one D-pad`() {
        val input = ControllerInputState()
        input.pressDpad(up = true)
        assertTrue(input.state.value.dpadUp)

        // A hat frame reporting centered must not cancel a still-held key.
        input.applyAnalog(analog(dpad = DpadState.fromAxes(0f, 0f)))
        assertTrue(input.state.value.dpadUp)

        input.pressDpad(up = false)
        assertFalse(input.state.value.dpadUp)

        input.applyAnalog(analog(dpad = DpadState.fromAxes(-1f, 0f)))
        assertTrue(input.state.value.dpadLeft)
    }

    /**
     * A source with no hat axes at all sends null, which must leave the hat
     * contribution untouched rather than repeatedly clearing a held key.
     */
    @Test fun `a source with no hat axes never clears the key D-pad`() {
        val input = ControllerInputState()
        input.pressDpad(right = true)
        repeat(3) { input.applyAnalog(analog(dpad = null)) }
        assertTrue(input.state.value.dpadRight)
    }

    @Test fun `opposite directions are retained and cancel only at encode time`() {
        val input = ControllerInputState()
        input.pressDpad(up = true, down = true)
        assertTrue(input.state.value.dpadUp)
        assertTrue(input.state.value.dpadDown)
        assertEquals(8, ControllerReportEncoder.hat(input.state.value))

        // Releasing one side must restore the still-held side, not invent an edge.
        input.pressDpad(down = false)
        assertEquals(0, ControllerReportEncoder.hat(input.state.value))
    }

    // ------------------------------------------------------------ analog framing

    /**
     * One physical event must produce one observable snapshot. Publishing sticks,
     * triggers and hat separately would emit three states per event and let a UI
     * or a sender see a half-applied frame.
     */
    @Test fun `one analog frame is one state change`() = runBlocking {
        val input = ControllerInputState()
        val seen = mutableListOf<ControllerState>()
        // Unconfined so every emission is observed synchronously; a conflating
        // collector would hide exactly the extra emissions this test looks for.
        val collector = launch(Dispatchers.Unconfined) { input.state.collect { seen += it } }

        input.applyAnalog(
            AnalogFrame(
                leftX = 10, leftY = 20, rightX = 30, rightY = 40,
                leftTrigger = 50, rightTrigger = 60,
                dpad = DpadState(up = true),
            ),
        )
        collector.cancel()

        // The initial neutral, then exactly one snapshot for the whole frame.
        assertEquals(2, seen.size)
        val after = seen.last()
        assertEquals(10, after.leftX)
        assertEquals(60, after.rightTrigger)
        assertTrue(after.dpadUp)
    }

    @Test fun `neutralize clears sticks triggers buttons and D-pad together`() {
        val input = ControllerInputState()
        input.applyAnalog(analog(leftX = 0, rightTrigger = 255, dpad = DpadState(left = true)))
        input.pressButton(ControllerButton.X, true)

        input.neutralize()
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    private fun analog(
        leftX: Int = 128, leftY: Int = 128, rightX: Int = 128, rightY: Int = 128,
        leftTrigger: Int = 0, rightTrigger: Int = 0, dpad: DpadState? = null,
    ) = AnalogFrame(leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, dpad)
}

/** The hat threshold, which every backend shares rather than reinventing. */
class DpadStateTest {
    @Test fun `axes past half scale become directions`() {
        assertEquals(DpadState(up = true), DpadState.fromAxes(0f, -1f))
        assertEquals(DpadState(down = true), DpadState.fromAxes(0f, 1f))
        assertEquals(DpadState(left = true), DpadState.fromAxes(-1f, 0f))
        assertEquals(DpadState(right = true), DpadState.fromAxes(1f, 0f))
        assertEquals(DpadState(up = true, right = true), DpadState.fromAxes(1f, -1f))
    }

    @Test fun `axes inside the threshold are centered`() {
        assertEquals(DpadState.None, DpadState.fromAxes(0f, 0f))
        assertEquals(DpadState.None, DpadState.fromAxes(0.5f, -0.5f))
    }
}
