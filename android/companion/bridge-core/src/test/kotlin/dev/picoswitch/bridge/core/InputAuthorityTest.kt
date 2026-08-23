package dev.picoswitch.bridge.core

import dev.picoswitch.bridge.protocol.ControllerReportEncoder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which host control set is the controller, and what happens at the boundary.
 *
 * The rule under test is that gameplay input has exactly one origin at a time.
 * Merging is not a milder version of this: a physical stick left and a touch
 * stick right have no combined meaning, and whichever event arrived last would
 * win by accident, which is the kind of defect that only shows up in a game.
 */
class InputAuthorityTest {

    private fun analog(
        leftX: Int = 128, leftY: Int = 128, rightX: Int = 128, rightY: Int = 128,
        leftTrigger: Int = 0, rightTrigger: Int = 0, dpad: DpadState? = null,
    ) = AnalogFrame(leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, dpad)

    @Test fun `physical is authoritative until something says otherwise`() {
        val input = ControllerInputState()
        assertEquals(InputAuthority.Physical, input.authority)
    }

    // ------------------------------------------------------- touch authoritative

    @Test fun `touch input reaches the published state once touch is authoritative`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(
            TouchContribution(
                leftX = 0, leftY = 255, rightTrigger = 255,
                dpad = DpadState(left = true),
                positionalButtons = setOf(ControllerButton.A),
                logicalButtons = setOf(ControllerButton.R2),
            ),
        )
        val state = input.state.value
        assertEquals(0, state.leftX)
        assertEquals(255, state.leftY)
        assertEquals(255, state.rightTrigger)
        assertTrue(state.dpadLeft)
        assertEquals(setOf(ControllerButton.A, ControllerButton.R2), state.buttons)
    }

    /**
     * The failure this prevents: a physical stick the user is not holding on
     * purpose (or a resting handheld's drift) quietly cancelling the touch stick.
     */
    @Test fun `physical events are discarded while touch is authoritative`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(TouchContribution(leftX = 255, positionalButtons = setOf(ControllerButton.B)))

        input.applyAnalog(analog(leftX = 0, rightTrigger = 200))
        input.pressButton(ControllerButton.Y, true)
        input.pressDpad(down = true)

        val state = input.state.value
        assertEquals("the touch stick must win", 255, state.leftX)
        assertEquals(0, state.rightTrigger)
        assertEquals(setOf(ControllerButton.B), state.buttons)
        assertFalse(state.dpadDown)
    }

    @Test fun `touch events are discarded while physical is authoritative`() {
        val input = ControllerInputState()
        input.applyAnalog(analog(leftX = 20))
        input.applyTouch(TouchContribution(leftX = 255, logicalButtons = setOf(ControllerButton.L1)))

        assertEquals(20, input.state.value.leftX)
        assertTrue(input.state.value.buttons.isEmpty())
        assertEquals(TouchContribution.Neutral, input.touchContribution)
    }

    // ------------------------------------------------------------- the boundary

    @Test fun `taking authority clears whatever the previous origin was holding`() {
        val input = ControllerInputState()
        input.pressButton(ControllerButton.A, true)
        input.applyAnalog(analog(leftX = 0, leftTrigger = 255))
        assertEquals(0, input.state.value.leftX)

        input.setAuthority(InputAuthority.Touch)
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    @Test fun `giving authority back clears whatever touch was holding`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(
            TouchContribution(leftX = 255, logicalButtons = setOf(ControllerButton.R2), rightTrigger = 255),
        )
        assertEquals(255, input.state.value.rightTrigger)

        input.setAuthority(InputAuthority.Physical)
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(TouchContribution.Neutral, input.touchContribution)

        // And the physical path works again straight away.
        input.pressButton(ControllerButton.A, true)
        assertEquals(setOf(ControllerButton.A), input.state.value.buttons)
    }

    @Test fun `setting the same authority twice is not a hidden neutralize`() {
        val input = ControllerInputState()
        input.pressButton(ControllerButton.A, true)
        input.setAuthority(InputAuthority.Physical)
        assertEquals(setOf(ControllerButton.A), input.state.value.buttons)
    }

    @Test fun `neutralize clears both origins`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(TouchContribution(leftX = 0, positionalButtons = setOf(ControllerButton.A)))

        input.neutralize()
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(TouchContribution.Neutral, input.touchContribution)
        assertEquals("authority itself is not a held input", InputAuthority.Touch, input.authority)
    }

    // ------------------------------------------- software / meta buttons persist

    /**
     * Home, Capture and C have no physical key on this hardware, so they are host
     * actions rather than a second controller. They must keep working under
     * either authority, and releasing a gameplay origin must not release them.
     */
    @Test fun `software buttons contribute under either authority`() {
        val input = ControllerInputState()
        input.setVirtualButton(ControllerButton.Home, true)
        assertEquals(setOf(ControllerButton.Home), input.state.value.buttons)

        input.setAuthority(InputAuthority.Touch)
        input.setVirtualButton(ControllerButton.Capture, true)
        input.applyTouch(TouchContribution(positionalButtons = setOf(ControllerButton.A)))
        assertEquals(
            setOf(ControllerButton.Capture, ControllerButton.A),
            input.state.value.buttons,
        )

        // Releasing the touch origin leaves the independently held one alone.
        input.applyTouch(TouchContribution.Neutral)
        assertEquals(setOf(ControllerButton.Capture), input.state.value.buttons)
    }

    // ------------------------------------------------------------ face layout

    /**
     * One resolver, one truth. A touch face press enters by the same positional
     * route a physical key does, so the layout preference applies identically and
     * the on-screen legend cannot drift away from the bit that is sent.
     */
    @Test fun `the face layout applies to touch presses exactly as to physical ones`() {
        val input = ControllerInputState()
        input.setRequestedLayout(ControllerFaceLayout.Nintendo)
        input.setAuthority(InputAuthority.Touch)

        input.applyTouch(TouchContribution(positionalButtons = setOf(FaceButtonPosition.South.positional)))
        assertEquals(setOf(ControllerButton.B), input.state.value.buttons)

        input.applyTouch(TouchContribution(positionalButtons = setOf(FaceButtonPosition.East.positional)))
        assertEquals(setOf(ControllerButton.A), input.state.value.buttons)
    }

    @Test fun `a logical touch button is never face swapped`() {
        val input = ControllerInputState()
        input.setRequestedLayout(ControllerFaceLayout.Nintendo)
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(TouchContribution(logicalButtons = setOf(ControllerButton.C, ControllerButton.L1)))
        assertEquals(setOf(ControllerButton.C, ControllerButton.L1), input.state.value.buttons)
    }

    @Test fun `changing the layout while touch is authoritative clears held input`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(TouchContribution(positionalButtons = setOf(ControllerButton.A)))
        input.setRequestedLayout(ControllerFaceLayout.Nintendo)
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    // ------------------------------------------------------------------ the wire

    @Test fun `a touch trigger encodes as both the button bit and the analog byte`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(
            TouchContribution(rightTrigger = 255, logicalButtons = setOf(ControllerButton.R2)),
        )
        val report = ControllerReportEncoder.encode(input.state.value)
        assertEquals("analog right trigger", 255, report[5].toInt() and 0xFF)
        // R2 is ordinal 7 -> bit 7 of the low button byte.
        assertEquals(0x80, report[6].toInt() and 0xFF)
    }

    @Test fun `a touch diagonal encodes to the matching hat code`() {
        val input = ControllerInputState()
        input.setAuthority(InputAuthority.Touch)
        input.applyTouch(TouchContribution(dpad = DpadState(up = true, right = true)))
        assertEquals(1, ControllerReportEncoder.hat(input.state.value))
    }
}
