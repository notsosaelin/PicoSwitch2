package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition

/** Console-facing controller identities for which a touch layout is shipped. */
enum class TouchProfileId(val key: String) {
    Pro2("pro2"),
    GameCube("gc"),
    JoyConLeft("jcl"),
    JoyConRight("jcr");

    companion object {
        fun fromKey(value: String?): TouchProfileId? = entries.firstOrNull { it.key == value }
    }
}

/**
 * What a control visibly represents on the selected console-facing controller.
 *
 * This is deliberately distinct from [TouchControlAction].  A sideways Joy-Con
 * direction button, for example, is produced through a generic face-button
 * bridge usage, while a GameCube Z control is produced through the generic R
 * shoulder usage.  The profile owns that fixed binding; user state never does.
 */
enum class TouchOutputControl {
    Unspecified,
    FaceSouth,
    FaceEast,
    FaceWest,
    FaceNorth,
    A,
    B,
    X,
    Y,
    DirectionUp,
    DirectionLeft,
    DirectionRight,
    DirectionDown,
    Dpad,
    PrimaryStick,
    SecondaryStick,
    PrimaryStickClick,
    SecondaryStickClick,
    L,
    R,
    ZL,
    ZR,
    Z,
    SL,
    SR,
    Minus,
    Plus,
    Home,
    Capture,
    C,
}

/** Geometry authored in the reference layout's logical coordinate system. */
data class TouchControlGeometry(
    val anchorX: Float,
    val anchorY: Float,
    val widthUnits: Float,
    val heightUnits: Float,
    val shape: TouchControlShape = TouchControlShape.Circle,
    val hitMarginUnits: Float = 0f,
    val priority: Int = 0,
    /** Logical-unit offset from a shared group anchor; immune to aspect-ratio distortion. */
    val groupOffsetXUnits: Float = 0f,
    val groupOffsetYUnits: Float = 0f,
)

/** Portable visual intent.  A host renderer supplies the actual paths. */
data class TouchVisualSpec(
    val role: TouchVisualRole,
    val label: String = "",
    val glyph: TouchControlGlyph? = null,
    /** Clockwise rotation in screen coordinates; immutable template art direction. */
    val rotationDegrees: Float = 0f,
)

/** One immutable, shipped control before a profile binding is applied. */
data class TouchTemplateControl(
    val id: String,
    val output: TouchOutputControl,
    val interaction: TouchControlKind,
    val geometry: TouchControlGeometry,
    val visual: TouchVisualSpec,
    val editGroupId: String? = null,
)

/** Immutable shipped default.  User changes are sparse overrides, never mutations of this value. */
data class TouchLayoutTemplate(
    val id: String,
    val profileId: TouchProfileId,
    val schemaVersion: Int,
    val templateRevision: Int,
    val controls: List<TouchTemplateControl>,
)

data class TouchControllerProfile(
    val id: TouchProfileId,
    val displayName: String,
    val defaultTemplate: TouchLayoutTemplate,
    val outputs: Set<TouchOutputControl>,
    val bindings: Map<TouchOutputControl, TouchControlAction>,
) {
    init {
        require(defaultTemplate.profileId == id)
        require(outputs == bindings.keys)
    }
}

/** The complete, exhaustive shipped touch-profile catalog. */
object TouchProfileCatalog {
    val profiles: Map<TouchProfileId, TouchControllerProfile> = listOf(
        pro2(),
        gameCube(),
        joyConLeft(),
        joyConRight(),
    ).associateBy { it.id }

    fun require(profileId: TouchProfileId): TouchControllerProfile =
        requireNotNull(profiles[profileId]) { "No touch profile for $profileId" }

    private fun pro2(): TouchControllerProfile {
        val bindings = linkedMapOf(
            TouchOutputControl.FaceSouth to TouchControlAction.Face(FaceButtonPosition.South),
            TouchOutputControl.FaceEast to TouchControlAction.Face(FaceButtonPosition.East),
            TouchOutputControl.FaceWest to TouchControlAction.Face(FaceButtonPosition.West),
            TouchOutputControl.FaceNorth to TouchControlAction.Face(FaceButtonPosition.North),
            TouchOutputControl.Dpad to TouchControlAction.Directions,
            TouchOutputControl.PrimaryStick to TouchControlAction.Stick(ControlSide.Left),
            TouchOutputControl.SecondaryStick to TouchControlAction.Stick(ControlSide.Right),
            TouchOutputControl.PrimaryStickClick to TouchControlAction.Logical(ControllerButton.LeftStick),
            TouchOutputControl.SecondaryStickClick to TouchControlAction.Logical(ControllerButton.RightStick),
            TouchOutputControl.L to TouchControlAction.Logical(ControllerButton.L1),
            TouchOutputControl.R to TouchControlAction.Logical(ControllerButton.R1),
            TouchOutputControl.ZL to TouchControlAction.Trigger(ControlSide.Left),
            TouchOutputControl.ZR to TouchControlAction.Trigger(ControlSide.Right),
            TouchOutputControl.Minus to TouchControlAction.Logical(ControllerButton.Select),
            TouchOutputControl.Plus to TouchControlAction.Logical(ControllerButton.Start),
            TouchOutputControl.Home to TouchControlAction.Logical(ControllerButton.Home),
            TouchOutputControl.Capture to TouchControlAction.Logical(ControllerButton.Capture),
            TouchOutputControl.C to TouchControlAction.Logical(ControllerButton.C),
        )
        return TouchControllerProfile(
            id = TouchProfileId.Pro2,
            displayName = "Pro Controller 2",
            defaultTemplate = TouchLayoutV1.template,
            outputs = bindings.keys,
            bindings = bindings,
        )
    }

    private fun gameCube(): TouchControllerProfile {
        val bindings = linkedMapOf(
            TouchOutputControl.A to TouchControlAction.Logical(ControllerButton.A),
            TouchOutputControl.B to TouchControlAction.Logical(ControllerButton.B),
            TouchOutputControl.X to TouchControlAction.Logical(ControllerButton.X),
            TouchOutputControl.Y to TouchControlAction.Logical(ControllerButton.Y),
            TouchOutputControl.Dpad to TouchControlAction.Directions,
            TouchOutputControl.PrimaryStick to TouchControlAction.Stick(ControlSide.Left),
            TouchOutputControl.SecondaryStick to TouchControlAction.Stick(ControlSide.Right),
            // The firmware's GameCube policy maps generic L/R shoulders to ZL/Z.
            TouchOutputControl.ZL to TouchControlAction.Logical(ControllerButton.L1),
            TouchOutputControl.Z to TouchControlAction.Logical(ControllerButton.R1),
            // A touch trigger is a full analog pull plus the digital trigger bit;
            // the GameCube seam turns that endpoint into native travel + detent.
            TouchOutputControl.L to TouchControlAction.Trigger(ControlSide.Left),
            TouchOutputControl.R to TouchControlAction.Trigger(ControlSide.Right),
            TouchOutputControl.Plus to TouchControlAction.Logical(ControllerButton.Start),
            TouchOutputControl.Home to TouchControlAction.Logical(ControllerButton.Home),
            TouchOutputControl.Capture to TouchControlAction.Logical(ControllerButton.Capture),
            TouchOutputControl.C to TouchControlAction.Logical(ControllerButton.C),
        )
        return TouchControllerProfile(
            id = TouchProfileId.GameCube,
            displayName = "NSO GameCube",
            defaultTemplate = TouchPersonalityTemplates.gameCube,
            outputs = bindings.keys,
            bindings = bindings,
        )
    }

    private fun joyConLeft(): TouchControllerProfile {
        val bindings = linkedMapOf(
            TouchOutputControl.PrimaryStick to TouchControlAction.Stick(ControlSide.Left),
            TouchOutputControl.PrimaryStickClick to TouchControlAction.Logical(ControllerButton.LeftStick),
            TouchOutputControl.DirectionUp to TouchControlAction.Logical(ControllerButton.X),
            TouchOutputControl.DirectionLeft to TouchControlAction.Logical(ControllerButton.A),
            TouchOutputControl.DirectionRight to TouchControlAction.Logical(ControllerButton.Y),
            TouchOutputControl.DirectionDown to TouchControlAction.Logical(ControllerButton.B),
            TouchOutputControl.SL to TouchControlAction.Logical(ControllerButton.L1),
            TouchOutputControl.SR to TouchControlAction.Logical(ControllerButton.R1),
            TouchOutputControl.L to TouchControlAction.Trigger(ControlSide.Left),
            TouchOutputControl.ZL to TouchControlAction.Trigger(ControlSide.Right),
            TouchOutputControl.Minus to TouchControlAction.Logical(ControllerButton.Select),
            TouchOutputControl.Capture to TouchControlAction.Logical(ControllerButton.Capture),
        )
        return TouchControllerProfile(
            id = TouchProfileId.JoyConLeft,
            displayName = "Joy-Con 2 (L), sideways",
            defaultTemplate = TouchPersonalityTemplates.joyConLeft,
            outputs = bindings.keys,
            bindings = bindings,
        )
    }

    private fun joyConRight(): TouchControllerProfile {
        val bindings = linkedMapOf(
            TouchOutputControl.PrimaryStick to TouchControlAction.Stick(ControlSide.Left),
            TouchOutputControl.PrimaryStickClick to TouchControlAction.Logical(ControllerButton.LeftStick),
            TouchOutputControl.A to TouchControlAction.Logical(ControllerButton.A),
            TouchOutputControl.B to TouchControlAction.Logical(ControllerButton.X),
            TouchOutputControl.X to TouchControlAction.Logical(ControllerButton.B),
            TouchOutputControl.Y to TouchControlAction.Logical(ControllerButton.Y),
            TouchOutputControl.SL to TouchControlAction.Logical(ControllerButton.L1),
            TouchOutputControl.SR to TouchControlAction.Logical(ControllerButton.R1),
            TouchOutputControl.R to TouchControlAction.Trigger(ControlSide.Left),
            TouchOutputControl.ZR to TouchControlAction.Trigger(ControlSide.Right),
            TouchOutputControl.Plus to TouchControlAction.Logical(ControllerButton.Start),
            TouchOutputControl.Home to TouchControlAction.Logical(ControllerButton.Home),
            TouchOutputControl.C to TouchControlAction.Logical(ControllerButton.C),
        )
        return TouchControllerProfile(
            id = TouchProfileId.JoyConRight,
            displayName = "Joy-Con 2 (R), sideways",
            defaultTemplate = TouchPersonalityTemplates.joyConRight,
            outputs = bindings.keys,
            bindings = bindings,
        )
    }
}

/** Personality-specific shipped geometry.  The Pro2 baseline remains in [TouchLayoutV1]. */
object TouchPersonalityTemplates {
    private const val SCHEMA_VERSION = 1
    private const val REVISION = 2
    private const val FACE_GROUP = "face-cluster"
    private const val DIRECTION_GROUP = "direction-cluster"
    private const val UTILITY_GROUP = "utility-cluster"
    private const val SECONDARY_GROUP = "secondary-cluster"
    private const val GAMECUBE_FACE_SCALE = 0.80f
    private const val GAMECUBE_FACE_VISUAL_CENTER_OFFSET_Y = -2.70f
    private const val GAMECUBE_FACE_NUDGE_X = -2.72f
    private const val GAMECUBE_FACE_NUDGE_Y = 2.25f
    private const val GAMECUBE_UTILITY_SPACING = 70f
    private const val GAMECUBE_SECONDARY_OFFSET_X = 170f
    private const val JOYCON_PRIMARY_CENTER_Y = 255f
    private const val JOYCON_BUTTON_CLUSTER_CENTER_X = 636f
    // Preserve the GameCube internal geometry while placing its complete visual
    // group opposite the upper-left main stick. The anchor compensates for the
    // asymmetric cluster's visual centre rather than treating A as its centre.
    private val GAMECUBE_FACE_GROUP = TouchGroupGeometry(
        TouchLayoutV1.FACE_CLUSTER_X_UNITS,
        TouchLayoutV1.LEFT_PRIMARY_Y_UNITS - GAMECUBE_FACE_VISUAL_CENTER_OFFSET_Y,
    )
    private val GAMECUBE_TOP_UTILITIES = TouchGroupGeometry(400f, 44f)
    private val GAMECUBE_SECONDARY_CONTROLS = TouchGroupGeometry(
        400f,
        TouchLayoutV1.LEFT_SECONDARY_Y_UNITS,
    )
    private val JOYCON_LEFT_DIRECTIONS = TouchGroupGeometry(
        JOYCON_BUTTON_CLUSTER_CENTER_X,
        JOYCON_PRIMARY_CENTER_Y,
    )
        .squareDiamond(radiusUnits = 60f)
    private val JOYCON_RIGHT_FACE = TouchGroupGeometry(
        JOYCON_BUTTON_CLUSTER_CENTER_X,
        JOYCON_PRIMARY_CENTER_Y,
    )
        .squareDiamond(radiusUnits = 60f)

    val gameCube = TouchLayoutTemplate(
        id = "picoswitch.touch.gc.v1",
        profileId = TouchProfileId.GameCube,
        schemaVersion = SCHEMA_VERSION,
        templateRevision = REVISION,
        controls = listOf(
            pad("zl", TouchOutputControl.ZL, 60f, 42f, "ZL"),
            pad("trigger-l", TouchOutputControl.L, 160f, 42f, "L", TouchControlKind.Trigger),
            groupUtility(
                "capture", TouchOutputControl.Capture,
                GAMECUBE_TOP_UTILITIES.at(-GAMECUBE_UTILITY_SPACING, 0f),
                TouchControlGlyph.Capture,
            ),
            groupUtility(
                "home", TouchOutputControl.Home,
                GAMECUBE_TOP_UTILITIES.at(0f, 0f),
                TouchControlGlyph.Home,
            ),
            groupUtility(
                "chat", TouchOutputControl.C,
                GAMECUBE_TOP_UTILITIES.at(GAMECUBE_UTILITY_SPACING, 0f),
                label = "C",
            ),
            pad("z", TouchOutputControl.Z, 640f, 42f, "Z"),
            pad("trigger-r", TouchOutputControl.R, 740f, 42f, "R", TouchControlKind.Trigger),
            // GameCube controls substitute directly into the proven Pro2 major-
            // control composition; only their personality-specific art differs.
            vector(
                "main-stick", TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                TouchLayoutV1.LEFT_PRIMARY_X_UNITS, TouchLayoutV1.LEFT_PRIMARY_Y_UNITS, 140f,
            ),
            groupVector(
                "dpad", TouchOutputControl.Dpad, TouchControlKind.Dpad,
                GAMECUBE_SECONDARY_CONTROLS.at(-GAMECUBE_SECONDARY_OFFSET_X, 0f), 140f,
            ),
            groupVector(
                "c-stick", TouchOutputControl.SecondaryStick, TouchControlKind.Stick,
                GAMECUBE_SECONDARY_CONTROLS.at(GAMECUBE_SECONDARY_OFFSET_X, 0f), 140f,
            ),
            round(
                "plus", TouchOutputControl.Plus,
                TouchLayoutResolver.REFERENCE_WIDTH_UNITS / 2f, 240f, 52f, "+",
            ),
            // Physical GameCube relationship: large A at the centre, B south-west,
            // vertical X to the east, and horizontal Y north-west.
            gcFace(
                "a", TouchOutputControl.A, gameCubeFaceAt(0f, 0f),
                88f, 88f, "A", TouchVisualRole.GameCubeLargeA,
            ),
            gcFace(
                "b", TouchOutputControl.B, gameCubeFaceAt(-68.5f, 57.5f),
                56f, 56f, "B", TouchVisualRole.GameCubeSmallB, margin = 1f,
            ),
            gcFace(
                "x", TouchOutputControl.X, gameCubeFaceAt(77f, -5f),
                52f, 84f, "X", TouchVisualRole.GameCubeBeanX, margin = 7f,
                rotationDegrees = 10.7f,
            ),
            gcFace(
                "y", TouchOutputControl.Y, gameCubeFaceAt(-37.5f, -67.25f),
                84f, 54f, "Y", TouchVisualRole.GameCubeBeanY, margin = 7f,
                rotationDegrees = -11.0f,
            ),
        ),
    )

    val joyConLeft = TouchLayoutTemplate(
        id = "picoswitch.touch.jcl.v1",
        profileId = TouchProfileId.JoyConLeft,
        schemaVersion = SCHEMA_VERSION,
        templateRevision = REVISION,
        controls = listOf(
            pad("sl", TouchOutputControl.SL, 60f, 42f, "SL"),
            pad("l", TouchOutputControl.L, 160f, 42f, "L", TouchControlKind.Trigger),
            utility("minus", TouchOutputControl.Minus, 365f, label = "-"),
            utility("capture", TouchOutputControl.Capture, 435f, TouchControlGlyph.Capture),
            pad("zl", TouchOutputControl.ZL, 640f, 42f, "ZL", TouchControlKind.Trigger),
            pad("sr", TouchOutputControl.SR, 740f, 42f, "SR"),
            vector(
                "primary-stick", TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                150f, JOYCON_PRIMARY_CENTER_Y, 150f,
            ),
            round("stick-click", TouchOutputControl.PrimaryStickClick, 285f, 320f, 56f, "L3"),
            joyButton(
                "direction-up", TouchOutputControl.DirectionUp,
                JOYCON_LEFT_DIRECTIONS.getValue(TouchCardinalSlot.North),
                group = DIRECTION_GROUP, role = TouchVisualRole.JoyConDirectionButton,
            ),
            joyButton(
                "direction-left", TouchOutputControl.DirectionLeft,
                JOYCON_LEFT_DIRECTIONS.getValue(TouchCardinalSlot.West),
                group = DIRECTION_GROUP, role = TouchVisualRole.JoyConDirectionButton,
            ),
            joyButton(
                "direction-right", TouchOutputControl.DirectionRight,
                JOYCON_LEFT_DIRECTIONS.getValue(TouchCardinalSlot.East),
                group = DIRECTION_GROUP, role = TouchVisualRole.JoyConDirectionButton,
            ),
            joyButton(
                "direction-down", TouchOutputControl.DirectionDown,
                JOYCON_LEFT_DIRECTIONS.getValue(TouchCardinalSlot.South),
                group = DIRECTION_GROUP, role = TouchVisualRole.JoyConDirectionButton,
            ),
        ),
    )

    val joyConRight = TouchLayoutTemplate(
        id = "picoswitch.touch.jcr.v1",
        profileId = TouchProfileId.JoyConRight,
        schemaVersion = SCHEMA_VERSION,
        templateRevision = REVISION,
        controls = listOf(
            pad("sl", TouchOutputControl.SL, 60f, 42f, "SL"),
            pad("r", TouchOutputControl.R, 160f, 42f, "R", TouchControlKind.Trigger),
            utility("plus", TouchOutputControl.Plus, 330f, label = "+"),
            utility("home", TouchOutputControl.Home, 400f, TouchControlGlyph.Home),
            utility("chat", TouchOutputControl.C, 470f, label = "C"),
            pad("zr", TouchOutputControl.ZR, 640f, 42f, "ZR", TouchControlKind.Trigger),
            pad("sr", TouchOutputControl.SR, 740f, 42f, "SR"),
            vector(
                "primary-stick", TouchOutputControl.PrimaryStick, TouchControlKind.Stick,
                150f, JOYCON_PRIMARY_CENTER_Y, 150f,
            ),
            round("stick-click", TouchOutputControl.PrimaryStickClick, 285f, 320f, 56f, "R3"),
            joyButton(
                "x", TouchOutputControl.X, JOYCON_RIGHT_FACE.getValue(TouchCardinalSlot.North),
                label = "X", group = FACE_GROUP,
            ),
            joyButton(
                "y", TouchOutputControl.Y, JOYCON_RIGHT_FACE.getValue(TouchCardinalSlot.West),
                label = "Y", group = FACE_GROUP,
            ),
            joyButton(
                "a", TouchOutputControl.A, JOYCON_RIGHT_FACE.getValue(TouchCardinalSlot.East),
                label = "A", group = FACE_GROUP,
            ),
            joyButton(
                "b", TouchOutputControl.B, JOYCON_RIGHT_FACE.getValue(TouchCardinalSlot.South),
                label = "B", group = FACE_GROUP,
            ),
        ),
    )

    private fun geometry(
        x: Float,
        y: Float,
        width: Float,
        height: Float,
        shape: TouchControlShape = TouchControlShape.Circle,
        margin: Float = 0f,
        groupOffsetXUnits: Float = 0f,
        groupOffsetYUnits: Float = 0f,
    ) = TouchControlGeometry(
        anchorX = x / TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
        anchorY = y / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
        widthUnits = width,
        heightUnits = height,
        shape = shape,
        hitMarginUnits = margin,
        groupOffsetXUnits = groupOffsetXUnits,
        groupOffsetYUnits = groupOffsetYUnits,
    )

    private fun pad(
        id: String,
        output: TouchOutputControl,
        x: Float,
        y: Float,
        label: String,
        kind: TouchControlKind = TouchControlKind.Button,
    ) = TouchTemplateControl(
        id,
        output,
        kind,
        geometry(x, y, 88f, 56f, TouchControlShape.Rectangle, 2f),
        TouchVisualSpec(TouchVisualRole.RectangularButton, label),
    )

    private fun utility(
        id: String,
        output: TouchOutputControl,
        x: Float,
        glyph: TouchControlGlyph? = null,
        label: String = "",
    ) = TouchTemplateControl(
        id,
        output,
        TouchControlKind.Button,
        geometry(x, 44f, 50f, 50f, TouchControlShape.Rectangle, 3f),
        TouchVisualSpec(TouchVisualRole.Utility, label, glyph),
    )

    private fun groupUtility(
        id: String,
        output: TouchOutputControl,
        placement: TouchGroupPlacement,
        glyph: TouchControlGlyph? = null,
        label: String = "",
    ) = TouchTemplateControl(
        id,
        output,
        TouchControlKind.Button,
        geometry(
            placement.anchorX * TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
            placement.anchorY * TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
            50f,
            50f,
            TouchControlShape.Rectangle,
            3f,
            placement.offsetXUnits,
            placement.offsetYUnits,
        ),
        TouchVisualSpec(TouchVisualRole.Utility, label, glyph),
        editGroupId = UTILITY_GROUP,
    )

    private fun round(
        id: String,
        output: TouchOutputControl,
        x: Float,
        y: Float,
        size: Float,
        label: String,
    ) = TouchTemplateControl(
        id,
        output,
        TouchControlKind.Button,
        geometry(x, y, size, size, margin = 2f),
        TouchVisualSpec(TouchVisualRole.RoundButton, label),
    )

    private fun vector(
        id: String,
        output: TouchOutputControl,
        kind: TouchControlKind,
        x: Float,
        y: Float,
        size: Float,
        margin: Float = 0f,
        visualRole: TouchVisualRole? = null,
    ) = TouchTemplateControl(
        id,
        output,
        kind,
        geometry(x, y, size, size, margin = margin),
        TouchVisualSpec(
            visualRole ?: if (kind == TouchControlKind.Dpad) TouchVisualRole.UnifiedDpad
            else TouchVisualRole.AnalogStick,
        ),
    )

    private fun groupVector(
        id: String,
        output: TouchOutputControl,
        kind: TouchControlKind,
        placement: TouchGroupPlacement,
        size: Float,
    ) = TouchTemplateControl(
        id,
        output,
        kind,
        geometry(
            placement.anchorX * TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
            placement.anchorY * TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
            size,
            size,
            groupOffsetXUnits = placement.offsetXUnits,
            groupOffsetYUnits = placement.offsetYUnits,
        ),
        TouchVisualSpec(
            if (kind == TouchControlKind.Dpad) TouchVisualRole.UnifiedDpad
            else TouchVisualRole.AnalogStick,
        ),
        editGroupId = SECONDARY_GROUP,
    )

    private fun gameCubeFaceAt(x: Float, y: Float) = GAMECUBE_FACE_GROUP.at(
        x + GAMECUBE_FACE_NUDGE_X,
        y + GAMECUBE_FACE_NUDGE_Y,
    )

    private fun gcFace(
        id: String,
        output: TouchOutputControl,
        placement: TouchGroupPlacement,
        width: Float,
        height: Float,
        label: String,
        role: TouchVisualRole,
        margin: Float = 0f,
        rotationDegrees: Float = 0f,
    ): TouchTemplateControl {
        val scaledWidth = width * GAMECUBE_FACE_SCALE
        val scaledHeight = height * GAMECUBE_FACE_SCALE
        val minimumAuthoredTarget = TouchLayoutAudit.MIN_TARGET_UNITS / TouchLayoutResolver.MIN_SCALE
        val accessibleMargin = (
            (minimumAuthoredTarget - minOf(scaledWidth, scaledHeight)) / 2f
            ).coerceAtLeast(0f)
        return TouchTemplateControl(
            id,
            output,
            TouchControlKind.Button,
            geometry(
                placement.anchorX * TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
                placement.anchorY * TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
                scaledWidth,
                scaledHeight,
                shape = if (
                    role == TouchVisualRole.GameCubeBeanX || role == TouchVisualRole.GameCubeBeanY
                ) TouchControlShape.GameCubeContour else TouchControlShape.Circle,
                margin = maxOf(margin * GAMECUBE_FACE_SCALE, accessibleMargin),
                groupOffsetXUnits = placement.offsetXUnits * GAMECUBE_FACE_SCALE,
                groupOffsetYUnits = placement.offsetYUnits * GAMECUBE_FACE_SCALE,
            ),
            TouchVisualSpec(role, label, rotationDegrees = rotationDegrees),
            editGroupId = FACE_GROUP,
        )
    }

    private fun joyButton(
        id: String,
        output: TouchOutputControl,
        placement: TouchGroupPlacement,
        label: String = "",
        group: String,
        role: TouchVisualRole = TouchVisualRole.JoyConButton,
    ) = TouchTemplateControl(
        id,
        output,
        TouchControlKind.Button,
        geometry(
            placement.anchorX * TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
            placement.anchorY * TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
            58f,
            58f,
            groupOffsetXUnits = placement.offsetXUnits,
            groupOffsetYUnits = placement.offsetYUnits,
        ),
        TouchVisualSpec(role, label),
        editGroupId = group,
    )
}
