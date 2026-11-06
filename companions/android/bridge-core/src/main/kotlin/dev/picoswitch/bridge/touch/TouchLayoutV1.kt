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
 * ZL  L       [capture][home][C]     R  ZR
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
    const val TEMPLATE_REVISION = 2
    internal const val LEFT_PRIMARY_X_UNITS = 100f
    internal const val LEFT_PRIMARY_Y_UNITS = 164f
    internal const val LEFT_SECONDARY_X_UNITS = 216f
    internal const val LEFT_SECONDARY_Y_UNITS = 312f
    internal const val RIGHT_SECONDARY_X_UNITS = 584f
    internal const val RIGHT_SECONDARY_Y_UNITS = 312f
    internal const val FACE_CLUSTER_X_UNITS = 700f
    internal const val FACE_CLUSTER_Y_UNITS = 184f
    private const val FACE_GROUP = "face-cluster"
    private val FACE_DIAMOND = TouchGroupGeometry(FACE_CLUSTER_X_UNITS, FACE_CLUSTER_Y_UNITS)
        .squareDiamond(radiusUnits = 60f)

    // Control ids. Referenced by the renderer and by tests, so they are constants
    // rather than strings repeated at each site.
    const val TRIGGER_LEFT = "trigger-left"
    const val TRIGGER_RIGHT = "trigger-right"
    const val SHOULDER_LEFT = "shoulder-left"
    const val SHOULDER_RIGHT = "shoulder-right"
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

    /** Optional; present in the template, hidden until the user adds them. */
    const val GRIP_LEFT = "grip-left"
    const val GRIP_RIGHT = "grip-right"

    val layout: TouchLayout = TouchLayout(
        id = ID,
        schemaVersion = SCHEMA_VERSION,
        controls = listOf(
            // ------------------------------------------------------- top edge
            shoulder(TRIGGER_LEFT, TouchOutputControl.ZL, TouchControlAction.Trigger(ControlSide.Left), 62f, 42f, TouchControlKind.Trigger),
            shoulder(SHOULDER_LEFT, TouchOutputControl.L, TouchControlAction.Logical(ControllerButton.L1), 166f, 42f),
            utility(
                CAPTURE, TouchOutputControl.Capture, TouchControlAction.Logical(ControllerButton.Capture),
                330f, TouchControlGlyph.Capture,
            ),
            utility(
                HOME, TouchOutputControl.Home, TouchControlAction.Logical(ControllerButton.Home),
                400f, TouchControlGlyph.Home,
            ),
            utility(CHAT, TouchOutputControl.C, TouchControlAction.Logical(ControllerButton.C), 470f, label = "C"),
            shoulder(SHOULDER_RIGHT, TouchOutputControl.R, TouchControlAction.Logical(ControllerButton.R1), 634f, 42f),
            shoulder(TRIGGER_RIGHT, TouchOutputControl.ZR, TouchControlAction.Trigger(ControlSide.Right), 738f, 42f, TouchControlKind.Trigger),

            // ------------------------------------------------- primary clusters
            spec(
                id = DPAD,
                output = TouchOutputControl.Dpad,
                kind = TouchControlKind.Dpad,
                action = TouchControlAction.Directions,
                x = LEFT_SECONDARY_X_UNITS, y = LEFT_SECONDARY_Y_UNITS,
                width = 146f, height = 146f,
                visualRole = TouchVisualRole.UnifiedDpad,
            ),
            face(FACE_NORTH, TouchOutputControl.FaceNorth, FaceButtonPosition.North, FACE_DIAMOND.getValue(TouchCardinalSlot.North)),
            face(FACE_WEST, TouchOutputControl.FaceWest, FaceButtonPosition.West, FACE_DIAMOND.getValue(TouchCardinalSlot.West)),
            face(FACE_EAST, TouchOutputControl.FaceEast, FaceButtonPosition.East, FACE_DIAMOND.getValue(TouchCardinalSlot.East)),
            face(FACE_SOUTH, TouchOutputControl.FaceSouth, FaceButtonPosition.South, FACE_DIAMOND.getValue(TouchCardinalSlot.South)),

            // ------------------------------------------------------- centre band
            centre(MINUS, TouchOutputControl.Minus, TouchControlAction.Logical(ControllerButton.Select), 330f, 244f, "-"),
            centre(PLUS, TouchOutputControl.Plus, TouchControlAction.Logical(ControllerButton.Start), 470f, 244f, "+"),

            // ------------------------------------------------------------ sticks
            spec(
                id = STICK_LEFT,
                output = TouchOutputControl.PrimaryStick,
                kind = TouchControlKind.Stick,
                action = TouchControlAction.Stick(ControlSide.Left),
                x = LEFT_PRIMARY_X_UNITS, y = LEFT_PRIMARY_Y_UNITS,
                width = 146f, height = 146f,
                visualRole = TouchVisualRole.AnalogStick,
            ),
            spec(
                id = STICK_RIGHT,
                output = TouchOutputControl.SecondaryStick,
                kind = TouchControlKind.Stick,
                action = TouchControlAction.Stick(ControlSide.Right),
                x = RIGHT_SECONDARY_X_UNITS, y = RIGHT_SECONDARY_Y_UNITS,
                width = 146f, height = 146f,
                visualRole = TouchVisualRole.AnalogStick,
            ),
            stickClick(STICK_CLICK_LEFT, TouchOutputControl.PrimaryStickClick, ControllerButton.LeftStick, 350f, 332f, "L3"),
            stickClick(STICK_CLICK_RIGHT, TouchOutputControl.SecondaryStickClick, ControllerButton.RightStick, 450f, 332f, "R3"),
        ),
        profileId = TouchProfileId.Pro2,
        templateId = ID,
        templateRevision = TEMPLATE_REVISION,
    )

    /**
     * Controls this personality HAS but the shipped layout does not place.
     *
     * Catalog entries, appended to [template] and never to [layout]: the default
     * document instantiates only what the shipped controller shows, and Add
     * Control offers everything in the catalog. That is the whole mechanism —
     * there is no hidden ghost instance sitting in the layout waiting to be
     * revealed. See [TouchTemplateControl.inDefaultLayout].
     *
     * GL/GR are real Pro Controller 2 grip buttons. Their authored position is
     * the outer BOTTOM corners — below the sticks and outboard of the D-pad and
     * the face diamond — which is both the closest a flat screen gets to where
     * the hands already are and the only region wide enough that adding BOTH
     * still passes the overlap audit at authored size. Every other outer
     * position collides: level with the sticks runs into the left stick, and
     * level with the centre band runs into the face diamond.
     */
    private val OPTIONAL_TEMPLATE_CONTROLS: List<TouchTemplateControl> = listOf(
        grip(GRIP_LEFT, TouchOutputControl.GL, 48f, 352f, "GL"),
        grip(GRIP_RIGHT, TouchOutputControl.GR, 752f, 352f, "GR"),
    )

    /** The immutable profile-backed form of the already validated V1 layout. */
    val template: TouchLayoutTemplate = TouchLayoutTemplate(
        id = ID,
        profileId = TouchProfileId.Pro2,
        schemaVersion = SCHEMA_VERSION,
        templateRevision = TEMPLATE_REVISION,
        controls = layout.controls.map { control ->
            TouchTemplateControl(
                id = control.id,
                output = control.output,
                interaction = control.kind,
                geometry = TouchControlGeometry(
                    anchorX = control.anchorX,
                    anchorY = control.anchorY,
                    widthUnits = control.widthUnits,
                    heightUnits = control.heightUnits,
                    shape = control.shape,
                    hitMarginUnits = control.hitMarginUnits,
                    priority = control.priority,
                    groupOffsetXUnits = control.groupOffsetXUnits,
                    groupOffsetYUnits = control.groupOffsetYUnits,
                ),
                visual = TouchVisualSpec(
                    control.visualRole,
                    control.label,
                    control.glyph,
                    control.visualRotationDegrees,
                ),
                editGroupId = control.editGroupId,
                category = categoryOf(control),
            )
        } + OPTIONAL_TEMPLATE_CONTROLS,
    )

    private fun categoryOf(control: TouchControlSpec): TouchControlCategory = when (control.output) {
        TouchOutputControl.FaceNorth, TouchOutputControl.FaceEast,
        TouchOutputControl.FaceSouth, TouchOutputControl.FaceWest,
        -> TouchControlCategory.Face
        TouchOutputControl.Dpad -> TouchControlCategory.Directions
        TouchOutputControl.PrimaryStick, TouchOutputControl.SecondaryStick,
        TouchOutputControl.PrimaryStickClick, TouchOutputControl.SecondaryStickClick,
        -> TouchControlCategory.Sticks
        TouchOutputControl.L, TouchOutputControl.R,
        TouchOutputControl.ZL, TouchOutputControl.ZR,
        -> TouchControlCategory.Shoulders
        else -> TouchControlCategory.System
    }

    private fun grip(
        id: String,
        output: TouchOutputControl,
        x: Float,
        y: Float,
        label: String,
    ) = TouchTemplateControl(
        id = id,
        output = output,
        interaction = TouchControlKind.Button,
        geometry = TouchControlGeometry(
            anchorX = x / TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
            anchorY = y / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
            widthUnits = 64f,
            heightUnits = 64f,
            shape = TouchControlShape.Rectangle,
            hitMarginUnits = 2f,
        ),
        visual = TouchVisualSpec(TouchVisualRole.RectangularButton, label),
        inDefaultLayout = false,
        category = TouchControlCategory.Grip,
    )

    // ------------------------------------------------------------------ builders

    private fun spec(
        id: String,
        output: TouchOutputControl,
        kind: TouchControlKind,
        action: TouchControlAction,
        x: Float,
        y: Float,
        width: Float,
        height: Float,
        shape: TouchControlShape = TouchControlShape.Circle,
        margin: Float = 0f,
        label: String = "",
        glyph: TouchControlGlyph? = null,
        visualRole: TouchVisualRole = TouchVisualRole.Default,
        visualRotationDegrees: Float = 0f,
        editGroupId: String? = null,
        groupOffsetXUnits: Float = 0f,
        groupOffsetYUnits: Float = 0f,
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
        glyph = glyph,
        output = output,
        visualRole = visualRole,
        visualRotationDegrees = visualRotationDegrees,
        editGroupId = editGroupId,
        groupOffsetXUnits = groupOffsetXUnits,
        groupOffsetYUnits = groupOffsetYUnits,
    )

    /**
     * A shoulder or trigger pad.
     *
     * Wider than tall because the reachable band along the top edge is a strip,
     * and because a wide target survives a thumb that arrives at an angle.
     */
    private fun shoulder(
        id: String,
        output: TouchOutputControl,
        action: TouchControlAction,
        x: Float,
        y: Float,
        kind: TouchControlKind = TouchControlKind.Button,
    ) = spec(
        id = id, output = output, kind = kind, action = action, x = x, y = y,
        width = 92f, height = 56f, shape = TouchControlShape.Rectangle, margin = 2f,
        visualRole = TouchVisualRole.RectangularButton,
        label = when (action) {
            is TouchControlAction.Trigger ->
                if (action.side == ControlSide.Left) "ZL" else "ZR"
            is TouchControlAction.Logical ->
                if (action.button == ControllerButton.L1) "L" else "R"
            else -> ""
        },
    )

    private fun face(
        id: String,
        output: TouchOutputControl,
        position: FaceButtonPosition,
        placement: TouchGroupPlacement,
    ) = spec(
        id = id,
        output = output,
        kind = TouchControlKind.FaceButton,
        action = TouchControlAction.Face(position),
        x = placement.anchorX * TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
        y = placement.anchorY * TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
        width = 60f,
        height = 60f,
        // No hit margin, deliberately. Adjacent centres are separated by a
        // consistent square-diamond edge, so
        // any expansion here starts eating the neighbour and z-order would begin
        // deciding which button a roll between them lands on.
        margin = 0f,
        visualRole = TouchVisualRole.RoundButton,
        editGroupId = FACE_GROUP,
        groupOffsetXUnits = placement.offsetXUnits,
        groupOffsetYUnits = placement.offsetYUnits,
    )

    /** Compact utility controls share one rounded-square silhouette. */
    private fun utility(
        id: String,
        output: TouchOutputControl,
        action: TouchControlAction,
        x: Float,
        glyph: TouchControlGlyph? = null,
        label: String = "",
    ) = spec(
        id = id,
        output = output,
        kind = TouchControlKind.Button,
        action = action,
        x = x,
        y = 44f,
        width = 54f,
        height = 54f,
        shape = TouchControlShape.Rectangle,
        margin = 3f,
        label = label,
        glyph = glyph,
        visualRole = TouchVisualRole.Utility,
    )

    private fun centre(id: String, output: TouchOutputControl, action: TouchControlAction, x: Float, y: Float, label: String) =
        spec(id = id, output = output, kind = TouchControlKind.Button, action = action, x = x, y = y, width = 58f, height = 58f, margin = 3f, label = label, visualRole = TouchVisualRole.RoundButton)

    private fun stickClick(id: String, output: TouchOutputControl, button: ControllerButton, x: Float, y: Float, label: String) =
        spec(
            id = id, output = output, kind = TouchControlKind.Button,
            action = TouchControlAction.Logical(button),
            x = x, y = y, width = 56f, height = 56f, margin = 3f, label = label,
            visualRole = TouchVisualRole.RoundButton,
        )
}
