package dev.picoswitch.companion.ui.touch

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performTouchInput
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerState
import dev.picoswitch.bridge.core.FaceButtonPosition
import dev.picoswitch.bridge.touch.ControlSide
import dev.picoswitch.bridge.touch.ResolvedTouchControl
import dev.picoswitch.bridge.touch.ResolvedTouchLayout
import dev.picoswitch.bridge.touch.TouchControlAction
import dev.picoswitch.bridge.touch.TouchControlKind
import dev.picoswitch.bridge.touch.TouchControlShape
import dev.picoswitch.bridge.touch.TouchControlSpec
import dev.picoswitch.bridge.touch.TouchGamepad
import dev.picoswitch.bridge.touch.TouchLayout
import dev.picoswitch.bridge.touch.TouchLayoutRegion
import dev.picoswitch.bridge.touch.TouchLayoutV1
import dev.picoswitch.bridge.touch.TouchReleaseReason
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import kotlin.math.min

/**
 * The Compose pointer adapter, driven by real pointer events.
 *
 * The pure tests prove the engine is correct given contacts. This proves the
 * ANDROID half actually delivers them: that identifiers survive, that several
 * simultaneous contacts reach the router independently, and that nothing in the
 * gesture system takes one away. Those are exactly the failures that cannot be
 * reproduced without the framework in the loop.
 *
 * ## Why the geometry here is synthetic
 *
 * The production layout is deliberately refused in a window too small to play in,
 * and a phone in portrait is such a window — so a test built on it would prove
 * nothing on half the devices it ran on and would silently depend on the runner's
 * orientation. The controls below are placed against whatever area this surface
 * actually gets, which keeps the test about the ADAPTER. The production layout's
 * own geometry is covered exhaustively, at seven window shapes and four
 * densities, by `TouchLayoutResolverTest` and `TouchLayoutAuditTest`.
 */
class TouchPointerAdapterTest {

    @get:Rule val rule = createComposeRule()

    private lateinit var input: ControllerInputState
    private lateinit var gamepad: TouchGamepad

    @Volatile private var layout: ResolvedTouchLayout = ResolvedTouchLayout.Empty

    private fun setUpSurface() {
        input = ControllerInputState()
        gamepad = TouchGamepad(input)
        rule.setContent {
            Box(
                Modifier
                    .fillMaxSize()
                    .testTag(TAG)
                    .onSizeChanged { size ->
                        layout = testLayout(size.width.toFloat(), size.height.toFloat())
                        gamepad.engine.setLayout(layout)
                    }
                    .touchGamepadContacts(key = Unit, tracker = gamepad.contacts),
            )
        }
        rule.waitForIdle()
        gamepad.activate()
        assertTrue("the surface was never measured", layout.controls.isNotEmpty())
    }

    private fun control(id: String): ResolvedTouchControl = requireNotNull(layout.control(id)) {
        "no control '$id' in the test layout"
    }

    private fun center(id: String) = control(id).let { Offset(it.centerX, it.centerY) }

    /**
     * Two contacts, delivered together, must own two different controls. This is
     * the case a forest of independent gesture detectors gets wrong: the second
     * one fails to start while the first is dragging.
     */
    @Test fun twoSimultaneousContactsDriveTwoControls() {
        setUpSurface()
        val left = control(TouchLayoutV1.STICK_LEFT)
        val right = control(TouchLayoutV1.STICK_RIGHT)

        rule.onNodeWithTag(TAG).performTouchInput {
            down(LEFT_ID, Offset(left.centerX, left.centerY))
            down(RIGHT_ID, Offset(right.centerX, right.centerY))
            moveTo(LEFT_ID, Offset(left.centerX - left.trackingRadius, left.centerY))
            moveTo(RIGHT_ID, Offset(right.centerX, right.centerY + right.trackingRadius))
        }
        rule.waitForIdle()

        val leftContact = gamepad.engine.contactOn(TouchLayoutV1.STICK_LEFT)
        val rightContact = gamepad.engine.contactOn(TouchLayoutV1.STICK_RIGHT)
        assertNotNull("the left stick was never claimed", leftContact)
        assertNotNull("the right stick was never claimed", rightContact)
        assertNotEquals("one contact cannot own both sticks", leftContact, rightContact)

        val state = input.state.value
        assertEquals(1, state.leftX)
        assertEquals(255, state.rightY)
    }

    /**
     * Identity, not position. The framework reorders its pointer collection
     * freely, and lifting the earliest contact is exactly when it does.
     *
     * Asserted from the CONTROL's side, because the identifier the framework
     * assigns is its own business — the property that matters is that the same
     * contact still owns the same control after the collection has been
     * reshuffled underneath it. An index-keyed implementation fails here even
     * though it looks correct with two fingers.
     */
    @Test fun ownershipSurvivesAReorderingLift() {
        setUpSurface()
        val left = control(TouchLayoutV1.STICK_LEFT)

        rule.onNodeWithTag(TAG).performTouchInput {
            down(FIRST_ID, center(TouchLayoutV1.FACE_SOUTH))
            down(LEFT_ID, Offset(left.centerX, left.centerY))
            down(LAST_ID, center(TouchLayoutV1.SHOULDER_RIGHT))
        }
        rule.waitForIdle()
        val stickContact = gamepad.engine.contactOn(TouchLayoutV1.STICK_LEFT)
        val shoulderContact = gamepad.engine.contactOn(TouchLayoutV1.SHOULDER_RIGHT)
        assertNotNull("the stick was never claimed", stickContact)
        assertNotNull("the shoulder was never claimed", shoulderContact)
        assertNotNull(gamepad.engine.contactOn(TouchLayoutV1.FACE_SOUTH))

        // Lift the FIRST contact, then keep driving the stick with its own.
        rule.onNodeWithTag(TAG).performTouchInput { up(FIRST_ID) }
        rule.onNodeWithTag(TAG).performTouchInput {
            moveTo(LEFT_ID, Offset(left.centerX + left.trackingRadius, left.centerY))
        }
        rule.waitForIdle()

        assertEquals(
            "the stick changed hands when another contact lifted",
            stickContact, gamepad.engine.contactOn(TouchLayoutV1.STICK_LEFT),
        )
        assertEquals(
            "the shoulder changed hands when another contact lifted",
            shoulderContact, gamepad.engine.contactOn(TouchLayoutV1.SHOULDER_RIGHT),
        )
        assertNull(gamepad.engine.contactOn(TouchLayoutV1.FACE_SOUTH))

        val state = input.state.value
        assertEquals(255, state.leftX)
        assertEquals(setOf(ControllerButton.R1), state.buttons)
    }

    /** Movement, a face button and a trigger at once — an ordinary chord. */
    @Test fun aMovementAndActionChordReachesTheControllerState() {
        setUpSurface()
        val left = control(TouchLayoutV1.STICK_LEFT)

        rule.onNodeWithTag(TAG).performTouchInput {
            down(LEFT_ID, Offset(left.centerX, left.centerY))
            down(FIRST_ID, center(TouchLayoutV1.FACE_SOUTH))
            down(LAST_ID, center(TouchLayoutV1.TRIGGER_RIGHT))
            moveTo(LEFT_ID, Offset(left.centerX, left.centerY - left.trackingRadius))
        }
        rule.waitForIdle()

        val state = input.state.value
        assertEquals(1, state.leftY)
        assertEquals(255, state.rightTrigger)
        assertTrue(ControllerButton.R2 in state.buttons)
        // Face South is positional A; the default layout in this test is the
        // resolver's, so it publishes A.
        assertTrue(ControllerButton.A in state.buttons)
        assertEquals(3, gamepad.diagnostics().ownedControls)
    }

    /**
     * A contact leaving the stick's circle keeps the stick; it must not become a
     * press on whatever it passed over.
     */
    @Test fun aStickKeepsItsContactAcrossTheWholeArea() {
        setUpSurface()
        val left = control(TouchLayoutV1.STICK_LEFT)
        val face = control(TouchLayoutV1.FACE_SOUTH)

        rule.onNodeWithTag(TAG).performTouchInput {
            down(LEFT_ID, Offset(left.centerX, left.centerY))
        }
        rule.waitForIdle()
        val stickContact = gamepad.engine.contactOn(TouchLayoutV1.STICK_LEFT)
        assertNotNull("the stick was never claimed", stickContact)

        // Straight out to the far edge, well beyond the stick's own circle.
        rule.onNodeWithTag(TAG).performTouchInput {
            moveTo(LEFT_ID, Offset(layout.region.right - 1f, left.centerY))
        }
        rule.waitForIdle()
        assertEquals("the stick clamps at full deflection", 255, input.state.value.leftX)
        assertEquals("the stick lost its contact", stickContact, gamepad.engine.contactOn(TouchLayoutV1.STICK_LEFT))

        // And on across another control, which must not take the contact.
        rule.onNodeWithTag(TAG).performTouchInput {
            moveTo(LEFT_ID, Offset(face.centerX, face.centerY))
        }
        rule.waitForIdle()

        assertEquals(
            "the stick lost its contact to a control the thumb passed over",
            stickContact, gamepad.engine.contactOn(TouchLayoutV1.STICK_LEFT),
        )
        assertNull(gamepad.engine.contactOn(TouchLayoutV1.FACE_SOUTH))
        assertTrue("no button may be pressed", input.state.value.buttons.isEmpty())
    }

    /** Lifting releases, and nothing is left held. */
    @Test fun liftingEveryContactReturnsToNeutral() {
        setUpSurface()
        rule.onNodeWithTag(TAG).performTouchInput {
            down(FIRST_ID, center(TouchLayoutV1.FACE_SOUTH))
            down(LAST_ID, center(TouchLayoutV1.TRIGGER_RIGHT))
        }
        rule.waitForIdle()
        assertNotEquals(ControllerState.Neutral, input.state.value)

        rule.onNodeWithTag(TAG).performTouchInput {
            up(FIRST_ID)
            up(LAST_ID)
        }
        rule.waitForIdle()

        assertEquals(ControllerState.Neutral, input.state.value)
        assertEquals(0, gamepad.diagnostics().ownedControls)
        assertEquals(0, gamepad.contacts.activeCount)
    }

    /**
     * The release-all boundary, from a real held gesture. Whatever was down has
     * no contact left that will ever release it, so the boundary has to.
     */
    @Test fun releasingWhileHeldClearsEverything() {
        setUpSurface()
        val left = control(TouchLayoutV1.STICK_LEFT)
        rule.onNodeWithTag(TAG).performTouchInput {
            down(LEFT_ID, Offset(left.centerX, left.centerY))
            moveTo(LEFT_ID, Offset(left.centerX - left.trackingRadius, left.centerY))
            down(FIRST_ID, center(TouchLayoutV1.FACE_SOUTH))
        }
        rule.waitForIdle()
        assertNotEquals(ControllerState.Neutral, input.state.value)

        gamepad.release(TouchReleaseReason.HostInactive)
        rule.waitForIdle()
        assertEquals(ControllerState.Neutral, input.state.value)

        // The contacts are still physically down. They must not resume.
        rule.onNodeWithTag(TAG).performTouchInput {
            moveTo(LEFT_ID, Offset(left.centerX + left.trackingRadius, left.centerY))
        }
        rule.waitForIdle()
        assertEquals(ControllerState.Neutral, input.state.value)
    }

    // -------------------------------------------------------------------- layout

    /**
     * Five well-separated controls placed against the measured area.
     *
     * Built directly rather than through `TouchLayoutResolver` so this test never
     * depends on the runner's window being large enough for the production
     * layout — see the class doc.
     */
    private fun testLayout(width: Float, height: Float): ResolvedTouchLayout {
        val unit = min(width, height)
        val stickHalf = unit * 0.16f
        val buttonHalf = unit * 0.09f

        fun circle(id: String, action: TouchControlAction, x: Float, y: Float, half: Float) =
            ResolvedTouchControl(
                spec = TouchControlSpec(
                    id = id,
                    kind = if (action is TouchControlAction.Stick) TouchControlKind.Stick
                    else TouchControlKind.Button,
                    action = action,
                    anchorX = x / width, anchorY = y / height,
                    widthUnits = half * 2f, heightUnits = half * 2f,
                    shape = TouchControlShape.Circle,
                ),
                centerX = x, centerY = y,
                halfWidth = half, halfHeight = half,
                hitHalfWidth = half, hitHalfHeight = half,
            )

        val controls = listOf(
            circle(
                TouchLayoutV1.STICK_LEFT, TouchControlAction.Stick(ControlSide.Left),
                width * 0.18f, height * 0.70f, stickHalf,
            ),
            circle(
                TouchLayoutV1.STICK_RIGHT, TouchControlAction.Stick(ControlSide.Right),
                width * 0.82f, height * 0.70f, stickHalf,
            ),
            circle(
                TouchLayoutV1.FACE_SOUTH,
                TouchControlAction.Face(FaceButtonPosition.South),
                width * 0.82f, height * 0.28f, buttonHalf,
            ),
            circle(
                TouchLayoutV1.SHOULDER_RIGHT,
                TouchControlAction.Logical(ControllerButton.R1),
                width * 0.5f, height * 0.10f, buttonHalf,
            ),
            circle(
                TouchLayoutV1.TRIGGER_RIGHT,
                TouchControlAction.Trigger(ControlSide.Right),
                width * 0.18f, height * 0.10f, buttonHalf,
            ),
        )
        return ResolvedTouchLayout(
            layout = TouchLayout("adapter-test", TouchLayoutV1.SCHEMA_VERSION, controls.map { it.spec }),
            region = TouchLayoutRegion(0f, 0f, width, height, 1f),
            controls = controls,
            scale = 1f,
            fits = true,
        )
    }

    private companion object {
        const val TAG = "touch-gamepad-area"

        // Deliberately non-contiguous and out of order.
        const val FIRST_ID = 7
        const val LEFT_ID = 42
        const val RIGHT_ID = 3
        const val LAST_ID = 901
    }
}
