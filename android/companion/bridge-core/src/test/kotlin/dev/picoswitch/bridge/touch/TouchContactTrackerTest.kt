package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerState
import dev.picoswitch.bridge.core.InputAuthority
import dev.picoswitch.bridge.core.TouchContribution
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The platform-adapter contract, exercised the way platforms actually behave.
 *
 * Touch platforms report the whole current contact set on every event, in an
 * order that is theirs to choose and that reorders freely. These tests deliver
 * batches shuffled and with non-contiguous identifiers on purpose: an
 * implementation that keys ownership on delivery position passes a two-contact
 * test and silently swaps controls the moment a third arrives.
 */
class TouchContactTrackerTest {

    private lateinit var gamepad: TouchGamepad
    private lateinit var input: ControllerInputState
    private lateinit var resolved: ResolvedTouchLayout

    @Before fun setUp() {
        input = ControllerInputState()
        gamepad = TouchGamepad(input)
        resolved = TouchLayoutResolver.resolve(
            TouchLayoutV1.layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
        )
        gamepad.engine.setLayout(resolved)
        gamepad.activate()
    }

    @Test fun `ownership is unchanged when the platform reorders the batch`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        val right = resolved.control(TouchLayoutV1.STICK_RIGHT)!!
        val face = resolved.control(TouchLayoutV1.FACE_SOUTH)!!

        gamepad.contacts.dispatch(
            listOf(
                down(70_001, left), down(3, right), down(1_204, face),
            ),
        )
        val expected = mapOf(
            70_001L to TouchLayoutV1.STICK_LEFT,
            3L to TouchLayoutV1.STICK_RIGHT,
            1_204L to TouchLayoutV1.FACE_SOUTH,
        )
        expected.forEach { (id, control) -> assertEquals(control, gamepad.engine.ownerOf(id)) }

        // Now shuffle the order on every subsequent batch, as a platform may.
        repeat(6) { round ->
            val offset = (round + 1) * 10f
            val batch = listOf(
                move(3, right.centerX + offset, right.centerY),
                move(1_204, face.centerX, face.centerY),
                move(70_001, left.centerX - offset, left.centerY),
            ).let { if (round % 2 == 0) it.reversed() else it }
            gamepad.contacts.dispatch(batch)
            expected.forEach { (id, control) -> assertEquals(control, gamepad.engine.ownerOf(id)) }
        }

        val state = input.state.value
        assertTrue("left stick moved left", state.leftX < 128)
        assertTrue("right stick moved right", state.rightX > 128)
        assertEquals(setOf(ControllerButton.A), state.buttons)
    }

    /**
     * A contact that stops being reported without ever ending would otherwise
     * hold its control down forever, with nothing left that could release it.
     */
    @Test fun `a contact the platform stops reporting is cancelled`() {
        val face = resolved.control(TouchLayoutV1.FACE_SOUTH)!!
        val shoulder = resolved.control(TouchLayoutV1.SHOULDER_LEFT)!!
        gamepad.contacts.dispatch(listOf(down(11, face), down(12, shoulder)))
        assertEquals(setOf(ControllerButton.A, ControllerButton.L1), input.state.value.buttons)

        // Contact 11 vanishes from the batch entirely.
        gamepad.contacts.dispatch(listOf(move(12, shoulder.centerX, shoulder.centerY)))

        assertNull(gamepad.engine.ownerOf(11))
        assertEquals(setOf(ControllerButton.L1), input.state.value.buttons)
        assertEquals(1, gamepad.diagnostics().contactsCancelled.toInt())
    }

    @Test fun `lifting one contact leaves the others owned`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        val face = resolved.control(TouchLayoutV1.FACE_EAST)!!
        gamepad.contacts.dispatch(listOf(down(5, left), down(6, face)))
        gamepad.contacts.dispatch(
            listOf(up(6, face), move(5, left.centerX, left.centerY - left.trackingRadius)),
        )
        assertEquals(TouchLayoutV1.STICK_LEFT, gamepad.engine.ownerOf(5))
        assertNull(gamepad.engine.ownerOf(6))
        assertEquals(1, input.state.value.leftY)
        assertTrue(input.state.value.buttons.isEmpty())
    }

    @Test fun `release-all forgets every tracked contact`() {
        val left = resolved.control(TouchLayoutV1.STICK_LEFT)!!
        gamepad.contacts.dispatch(listOf(down(1, left), down(2, resolved.control(TouchLayoutV1.PLUS)!!)))
        assertEquals(2, gamepad.contacts.activeCount)

        gamepad.release(TouchReleaseReason.HostInactive)
        assertEquals(0, gamepad.contacts.activeCount)
        assertEquals(ControllerState.Neutral, input.state.value)

        // A later batch that still mentions the old contacts cannot resurrect them.
        gamepad.contacts.dispatch(listOf(move(1, left.centerX - 60f, left.centerY)))
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    // ------------------------------------------------------------------ authority

    @Test fun `activating takes authority and deactivating gives it back`() {
        assertEquals(InputAuthority.Touch, input.authority)
        gamepad.deactivate()
        assertEquals(InputAuthority.Physical, input.authority)
        assertTrue(!gamepad.active)
    }

    @Test fun `deactivating with a control held publishes neutral`() {
        val face = resolved.control(TouchLayoutV1.FACE_SOUTH)!!
        gamepad.contacts.dispatch(listOf(down(1, face)))
        assertNotEquals(ControllerState.Neutral, input.state.value)

        gamepad.deactivate()
        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(TouchContribution.Neutral, input.touchContribution)
        assertEquals(0, gamepad.diagnostics().ownedControls)
    }

    @Test fun `activate and deactivate are idempotent`() {
        gamepad.activate()
        gamepad.activate()
        assertEquals(InputAuthority.Touch, input.authority)
        gamepad.deactivate()
        gamepad.deactivate()
        assertEquals(InputAuthority.Physical, input.authority)
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    /** Reconnect must start from nothing, never replay what was held when it dropped. */
    @Test fun `a link drop clears held touch input and reconnect starts neutral`() {
        val trigger = resolved.control(TouchLayoutV1.TRIGGER_RIGHT)!!
        gamepad.contacts.dispatch(listOf(down(1, trigger)))
        assertEquals(255, input.state.value.rightTrigger)

        // What the session does on link down.
        gamepad.release(TouchReleaseReason.LinkEnded)
        input.neutralize()
        assertEquals(ControllerState.Neutral, input.state.value)

        // The link returns. Nothing may reappear without a new contact.
        gamepad.contacts.dispatch(listOf(move(1, trigger.centerX, trigger.centerY)))
        assertEquals(ControllerState.Neutral, input.state.value)

        gamepad.contacts.dispatch(listOf(down(2, trigger)))
        assertEquals(255, input.state.value.rightTrigger)
    }

    // -------------------------------------------------------------------- helpers

    private fun down(id: Long, control: ResolvedTouchControl) =
        TouchContact(id, TouchPhase.Down, control.centerX, control.centerY, id)

    private fun move(id: Long, x: Float, y: Float) = TouchContact(id, TouchPhase.Move, x, y, id)

    private fun up(id: Long, control: ResolvedTouchControl) =
        TouchContact(id, TouchPhase.Up, control.centerX, control.centerY, id)
}
