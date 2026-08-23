package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition

/**
 * The default on-screen controller.
 *
 * ## Reading the numbers
 *
 * Every coordinate below is written against a reference window of
 * [TouchLayoutResolver.REFERENCE_WIDTH_UNITS] x
 * [TouchLayoutResolver.REFERENCE_HEIGHT_UNITS] logical units and then stored
 * normalized, so the file reads as a drawing rather than as a list of ratios.
 * The resolver scales sizes and stretches anchors; see its documentation for why
 * those two are not the same operation.
 *
 * ## Why the controls are where they are
 *
 * ```text
 * ZL  L   [menu]   ( )( )( )        R  ZR
 *
 *   (D-pad)                          (N)
 *                                 (W)   (E)
 *              (-)      (+)          (S)
 *
 *      (left stick)  (L3) (R3) (right stick)
 * ```
 *
 * - The two primary left-thumb controls (D-pad, left stick) and the two primary
 *   right-thumb controls (face diamond, right stick) sit at the outer edges,
 *   where a thumb holding the device already rests.
 * - Shoulders and triggers occupy the top corners, inside the safe rectangle so
 *   they never land in a system gesture strip.
 * - Everything low-frequency — `-`, `+`, Home, Capture, C, the stick clicks —
 *   lives toward the centre, deliberately away from the territory a thumb sweeps
 *   during play. They are reachable without being in the way.
 * - The middle stays quiet. That is not wasted space: it separates the hands,
 *   keeps a custom background visible, and leaves somewhere for future per-game
 *   controls to go without moving anything that muscle memory has learned.
 *
 * The stick clicks are their own controls rather than a press gesture on the
 * stick, because a long press on the knob would either delay ordinary movement
 * or fire while aiming. Separate targets also make "move and click at the same
 * time" an ordinary two-contact chord instead of a special case.
 *
 * ## Changing this layout
 *
 * `TouchLayoutV1Test` resolves it at every representative window shape and
 * fails on overlapping hit regions, undersized targets or a control outside the
 * safe rectangle. Move something and the audit will tell you what it broke.
 */
object TouchLayoutV1 {

    /**
     * Persisted schema version.
     *
     * Present from the first release even though nothing migrates yet. A stored
     * layout with no way to say which shape it is leaves a future reader only one
     * safe option, which is to discard the user's configuration.
     */
    const val SCHEMA_VERSION = 1

    const val ID = "picoswitch.touch.v1"

    // Control ids. Referenced by the renderer and by tests, so they are constants
    // rather than strings repeated at each site.
    const val TRIGGER_LEFT = "trigger-left"
    const val TRIGGER_RIGHT = "trigger-right"
    const val SHOULDER_LEFT = "shoulder-left"
    const val SHOULDER_RIGHT = "shoulder-right"
    const val MENU = "menu"
    const val CAPTURE = "capture"
    const val HOME = "home"
    const val CHAT = "chat"
    const val DPAD = "dpad"
    const val FACE_NORTH = "face-north"
    const val FACE_EAST = "face-east"
    const val FACE_SOUTH = "face-south"
    const val FACE_WEST = "face-west"
    const val MINUS = "minus"
    const val PLUS = "plus"
    const val STICK_LEFT = "stick-left"
    const val STICK_RIGHT = "stick-right"
    const val STICK_CLICK_LEFT = "stick-click-left"
    const val STICK_CLICK_RIGHT = "stick-click-right"

    val layout: TouchLayout = TouchLayout(
        id = ID,
        schemaVersion = SCHEMA_VERSION,
        controls = listOf(
            // ------------------------------------------------------- top edge
            shoulder(TRIGGER_LEFT, TouchControlAction.Trigger(ControlSide.Left), 62f, 42f, TouchControlKind.Trigger),
            shoulder(SHOULDER_LEFT, TouchControlAction.Logical(ControllerButton.L1), 166f, 42f),
            spec(
                id = MENU,
                kind = TouchControlKind.Button,
                action = TouchControlAction.SystemMenu,
                x = 250f, y = 42f, width = 58f, height = 56f,
                shape = TouchControlShape.Rectangle, margin = 2f, label = "MENU",
            ),
            // Short legends, because these are the smallest text-bearing controls
            // in the layout and a word that has to be shrunk to fit is less
            // readable than an abbreviation that does not.
            small(CAPTURE, TouchControlAction.Logical(ControllerButton.Capture), 330f, 44f, "CAP"),
            small(HOME, TouchControlAction.Logical(ControllerButton.Home), 400f, 44f, "HOME"),
            small(CHAT, TouchControlAction.Logical(ControllerButton.C), 470f, 44f, "C"),
            shoulder(SHOULDER_RIGHT, TouchControlAction.Logical(ControllerButton.R1), 634f, 42f),
            shoulder(TRIGGER_RIGHT, TouchControlAction.Trigger(ControlSide.Right), 738f, 42f, TouchControlKind.Trigger),

            // ------------------------------------------------- primary clusters
            spec(
                id = DPAD,
                kind = TouchControlKind.Dpad,
                action = TouchControlAction.Directions,
                x = 100f, y = 164f, width = 126f, height = 126f,
            ),
            face(FACE_NORTH, FaceButtonPosition.North, 700f, 114f),
            face(FACE_WEST, FaceButtonPosition.West, 634f, 180f),
            face(FACE_EAST, FaceButtonPosition.East, 766f, 180f),
            face(FACE_SOUTH, FaceButtonPosition.South, 700f, 246f),

            // ------------------------------------------------------- centre band
            centre(MINUS, TouchControlAction.Logical(ControllerButton.Select), 330f, 244f, "-"),
            centre(PLUS, TouchControlAction.Logical(ControllerButton.Start), 470f, 244f, "+"),

            // ------------------------------------------------------------ sticks
            spec(
                id = STICK_LEFT,
                kind = TouchControlKind.Stick,
                action = TouchControlAction.Stick(ControlSide.Left),
                x = 216f, y = 312f, width = 146f, height = 146f,
            ),
            spec(
                id = STICK_RIGHT,
                kind = TouchControlKind.Stick,
                action = TouchControlAction.Stick(ControlSide.Right),
                x = 584f, y = 312f, width = 146f, height = 146f,
            ),
            stickClick(STICK_CLICK_LEFT, ControllerButton.LeftStick, 350f, 332f, "L3"),
            stickClick(STICK_CLICK_RIGHT, ControllerButton.RightStick, 450f, 332f, "R3"),
        ),
    )

    // ------------------------------------------------------------------ builders

    private fun spec(
        id: String,
        kind: TouchControlKind,
        action: TouchControlAction,
        x: Float,
        y: Float,
        width: Float,
        height: Float,
        shape: TouchControlShape = TouchControlShape.Circle,
        margin: Float = 0f,
        label: String = "",
    ) = TouchControlSpec(
        id = id,
        kind = kind,
        action = action,
        anchorX = x / TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
        anchorY = y / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
        widthUnits = width,
        heightUnits = height,
        shape = shape,
        hitMarginUnits = margin,
        label = label,
    )

    /**
     * A shoulder or trigger pad.
     *
     * Wider than tall because the reachable band along the top edge is a strip,
     * and because a wide target survives a thumb that arrives at an angle.
     */
    private fun shoulder(
        id: String,
        action: TouchControlAction,
        x: Float,
        y: Float,
        kind: TouchControlKind = TouchControlKind.Button,
    ) = spec(
        id = id, kind = kind, action = action, x = x, y = y,
        width = 92f, height = 56f, shape = TouchControlShape.Rectangle, margin = 2f,
        label = when (action) {
            is TouchControlAction.Trigger ->
                if (action.side == ControlSide.Left) "ZL" else "ZR"
            is TouchControlAction.Logical ->
                if (action.button == ControllerButton.L1) "L" else "R"
            else -> ""
        },
    )

    private fun face(id: String, position: FaceButtonPosition, x: Float, y: Float) = spec(
        id = id,
        kind = TouchControlKind.FaceButton,
        action = TouchControlAction.Face(position),
        x = x, y = y, width = 62f, height = 62f,
        // No hit margin, deliberately. The four are 66 units apart on centre, so
        // any expansion here starts eating the neighbour and z-order would begin
        // deciding which button a roll between them lands on.
        margin = 0f,
    )

    private fun small(id: String, action: TouchControlAction, x: Float, y: Float, label: String) =
        spec(id = id, kind = TouchControlKind.Button, action = action, x = x, y = y, width = 54f, height = 54f, margin = 3f, label = label)

    private fun centre(id: String, action: TouchControlAction, x: Float, y: Float, label: String) =
        spec(id = id, kind = TouchControlKind.Button, action = action, x = x, y = y, width = 58f, height = 58f, margin = 3f, label = label)

    private fun stickClick(id: String, button: ControllerButton, x: Float, y: Float, label: String) =
        spec(
            id = id, kind = TouchControlKind.Button,
            action = TouchControlAction.Logical(button),
            x = x, y = y, width = 56f, height = 56f, margin = 3f, label = label,
        )
}
