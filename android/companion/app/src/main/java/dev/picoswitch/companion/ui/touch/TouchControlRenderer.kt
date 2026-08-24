package dev.picoswitch.companion.ui.touch

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathOperation
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Constraints
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerLayoutResolver
import dev.picoswitch.bridge.core.DpadState
import dev.picoswitch.bridge.touch.ControlSide
import dev.picoswitch.bridge.touch.ResolvedTouchControl
import dev.picoswitch.bridge.touch.ResolvedTouchLayout
import dev.picoswitch.bridge.touch.TouchCardinalSlot
import dev.picoswitch.bridge.touch.TouchControlAction
import dev.picoswitch.bridge.touch.TouchControlGlyph
import dev.picoswitch.bridge.touch.TouchDiagnosticsSnapshot
import dev.picoswitch.bridge.touch.TouchGuideKind
import dev.picoswitch.bridge.touch.TouchGuideLine
import dev.picoswitch.bridge.touch.TouchGameCubeGeometry
import dev.picoswitch.bridge.touch.TouchOutputControl
import dev.picoswitch.bridge.touch.TouchVisualRole
import dev.picoswitch.bridge.touch.TouchVector

/**
 * The colours the controller draws itself with.
 *
 * Resolved from the application theme once per frame-worth of state rather than
 * read inside the draw loop, so the renderer stays a pure function of geometry
 * plus these.
 */
data class TouchControlPalette(
    val idle: Color,
    val idleOutline: Color,
    val pressed: Color,
    val pressedOutline: Color,
    val label: Color,
    val pressedLabel: Color,
    val disabled: Color,
)

/**
 * What the controls should currently look like.
 *
 * Deliberately its OWN small value rather than the shared controller state: this
 * changes at contact rate, and hanging it off the application-wide UI state
 * would recompose the whole scaffold every time a thumb moves a pixel. The
 * semantic input has already reached the bridge by the time this is read; this
 * is only the picture.
 */
data class TouchVisualState(
    val leftStick: TouchVector = TouchVector.Zero,
    val rightStick: TouchVector = TouchVector.Zero,
    val dpad: DpadState = DpadState.None,
    val pressed: Set<String> = emptySet(),
    val enabled: Boolean = true,
)

/**
 * Draws the whole controller in one canvas.
 *
 * One pass rather than a composable per control, for a specific reason: with
 * many independently recomposing nodes, a stick move invalidates layout and
 * draw for the node tree around it, and the surface starts competing with the
 * report path it is supposed to be reporting on. Geometry is already resolved,
 * so drawing it is a loop.
 *
 * Nothing here decides anything semantic. Labels come from the shared face-layout
 * resolver, hit regions come from the resolved layout, and the pressed set comes
 * from the engine.
 */
fun DrawScope.drawTouchControls(
    layout: ResolvedTouchLayout,
    visual: TouchVisualState,
    palette: TouchControlPalette,
    faceLayout: ControllerFaceLayout,
    opacity: Float,
    textMeasurer: TextMeasurer,
    labelStyle: TextStyle,
) {
    layout.controls.forEach { control ->
        val held = control.id in visual.pressed
        val alpha = if (visual.enabled) opacity else opacity * DISABLED_ALPHA
        when (control.spec.visualRole) {
            TouchVisualRole.AnalogStick -> drawStick(
                control = control,
                displacement = if ((control.spec.action as TouchControlAction.Stick).side == ControlSide.Left) {
                    visual.leftStick
                } else {
                    visual.rightStick
                },
                palette = palette, alpha = alpha, held = held,
            )
            TouchVisualRole.UnifiedDpad -> drawDpad(control, visual.dpad, palette, alpha)
            TouchVisualRole.GameCubeLargeA,
            TouchVisualRole.GameCubeSmallB,
            TouchVisualRole.JoyConButton,
            TouchVisualRole.RoundButton -> drawRound(
                control, held, palette, alpha, textMeasurer, labelStyle,
                controlLabel(control, faceLayout),
            )
            TouchVisualRole.JoyConDirectionButton -> drawJoyConDirection(
                control, held, palette, alpha, textMeasurer, labelStyle,
            )
            TouchVisualRole.GameCubeBeanX,
            TouchVisualRole.GameCubeBeanY -> drawGameCubeBean(
                control, held, palette, alpha, textMeasurer, labelStyle,
            )
            TouchVisualRole.RectangularButton,
            TouchVisualRole.Utility -> drawPad(control, held, palette, alpha, textMeasurer, labelStyle)
            TouchVisualRole.Default -> when (val action = control.spec.action) {
                is TouchControlAction.Stick -> drawStick(
                    control,
                    if (action.side == ControlSide.Left) visual.leftStick else visual.rightStick,
                    palette,
                    alpha,
                    held,
                )
                TouchControlAction.Directions -> drawDpad(control, visual.dpad, palette, alpha)
                is TouchControlAction.Face -> drawRound(
                    control,
                    held,
                    palette,
                    alpha,
                    textMeasurer,
                    labelStyle,
                    ControllerLayoutResolver.faceLabel(action.position, faceLayout),
                )
                else -> if (control.spec.shape == dev.picoswitch.bridge.touch.TouchControlShape.Rectangle) {
                drawPad(control, held, palette, alpha, textMeasurer, labelStyle)
                } else {
                    drawRound(control, held, palette, alpha, textMeasurer, labelStyle, control.spec.label)
                }
            }
        }
    }
}

private fun controlLabel(control: ResolvedTouchControl, faceLayout: ControllerFaceLayout): String =
    when (val action = control.spec.action) {
        is TouchControlAction.Face -> ControllerLayoutResolver.faceLabel(action.position, faceLayout)
        else -> control.spec.label
    }

private fun DrawScope.drawRound(
    control: ResolvedTouchControl,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
    label: String,
) {
    val center = Offset(control.centerX, control.centerY)
    val radius = minOf(control.halfWidth, control.halfHeight)
    drawCircle(
        color = (if (held) palette.pressed else palette.idle).copy(alpha = alpha),
        radius = radius,
        center = center,
    )
    drawCircle(
        color = (if (held) palette.pressedOutline else palette.idleOutline).copy(alpha = alpha),
        radius = radius,
        center = center,
        style = Stroke(width = OUTLINE_WIDTH),
    )
    drawControlContent(
        glyph = control.spec.glyph,
        label = label,
        center = center,
        availableWidth = radius * 2f,
        availableHeight = radius * 2f,
        held = held,
        palette = palette,
        alpha = alpha,
        textMeasurer = textMeasurer,
        style = style,
    )
}

private fun DrawScope.drawPad(
    control: ResolvedTouchControl,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
) {
    val topLeft = Offset(control.centerX - control.halfWidth, control.centerY - control.halfHeight)
    val size = Size(control.halfWidth * 2f, control.halfHeight * 2f)
    val corner = androidx.compose.ui.geometry.CornerRadius(control.halfHeight * 0.45f)
    drawRoundRect(
        color = (if (held) palette.pressed else palette.idle).copy(alpha = alpha),
        topLeft = topLeft, size = size, cornerRadius = corner,
    )
    drawRoundRect(
        color = (if (held) palette.pressedOutline else palette.idleOutline).copy(alpha = alpha),
        topLeft = topLeft, size = size, cornerRadius = corner,
        style = Stroke(width = OUTLINE_WIDTH),
    )
    drawControlContent(
        glyph = control.spec.glyph,
        label = control.spec.label,
        center = Offset(control.centerX, control.centerY),
        availableWidth = size.width,
        availableHeight = size.height,
        held = held,
        palette = palette,
        alpha = alpha,
        textMeasurer = textMeasurer,
        style = style,
    )
}

/**
 * Code-native GameCube X/Y silhouette traced from Dolphin's actual `gcpad_x` / `gcpad_y`
 * alpha boundaries. The source bitmaps are references only and are not bundled in PicoSwitch2.
 */
private fun DrawScope.drawGameCubeBean(
    control: ResolvedTouchControl,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
) {
    val left = control.centerX - control.halfWidth
    val top = control.centerY - control.halfHeight
    val path = gameCubeFacePath(
        role = control.spec.visualRole,
        left = left,
        top = top,
        width = control.halfWidth * 2f,
        height = control.halfHeight * 2f,
        rotationDegrees = control.spec.visualRotationDegrees,
    )
    drawPath(path, (if (held) palette.pressed else palette.idle).copy(alpha = alpha))
    drawPath(
        path,
        (if (held) palette.pressedOutline else palette.idleOutline).copy(alpha = alpha),
        style = Stroke(width = OUTLINE_WIDTH),
    )
    drawControlContent(
        glyph = control.spec.glyph,
        label = control.spec.label,
        center = Offset(control.centerX, control.centerY),
        availableWidth = control.halfWidth * 2f,
        availableHeight = control.halfHeight * 2f,
        held = held,
        palette = palette,
        alpha = alpha,
        textMeasurer = textMeasurer,
        style = style,
    )
}

/** Normalized contours are intentionally visible to tests so the traced silhouettes cannot drift. */
internal fun gameCubeFaceContour(role: TouchVisualRole): List<Offset> =
    TouchGameCubeGeometry.contour(role).map { Offset(it.x, it.y) }

private fun gameCubeFacePath(
    role: TouchVisualRole,
    left: Float,
    top: Float,
    width: Float,
    height: Float,
    rotationDegrees: Float,
): Path {
    val oriented = orientedGameCubeFaceContour(role, width, height, rotationDegrees)
    val centerX = left + width / 2f
    val centerY = top + height / 2f
    fun point(index: Int): Offset {
        val source = oriented[(index + oriented.size) % oriented.size]
        return Offset(centerX + source.x, centerY + source.y)
    }

    return Path().apply {
        val first = point(0)
        moveTo(first.x, first.y)
        oriented.indices.forEach { index ->
            val previous = point(index - 1)
            val current = point(index)
            val next = point(index + 1)
            val following = point(index + 2)
            // Uniform Catmull-Rom converted to a cubic. Twenty equal-arc anchors retain
            // the source contour while keeping the Android path smooth at any scale.
            cubicTo(
                current.x + (next.x - previous.x) / 6f,
                current.y + (next.y - previous.y) / 6f,
                next.x - (following.x - current.x) / 6f,
                next.y - (following.y - current.y) / 6f,
                next.x,
                next.y,
            )
        }
        close()
    }
}

/** Rotates the preserved source silhouette without changing its anchors or template placement. */
internal fun orientedGameCubeFaceContour(
    role: TouchVisualRole,
    width: Float,
    height: Float,
    rotationDegrees: Float,
): List<Offset> {
    return TouchGameCubeGeometry.orientedContour(role, width, height, rotationDegrees)
        .map { Offset(it.x, it.y) }
}

/** Independent Joy-Con direction button with a recessed, centred triangle. */
private fun DrawScope.drawJoyConDirection(
    control: ResolvedTouchControl,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
) {
    drawRound(control, held, palette, alpha, textMeasurer, style, label = "")
    val available = minOf(control.halfWidth, control.halfHeight) * 2f
    val path = Path().apply {
        val vertices = joyConDirectionTriangle(
            control.spec.output,
            Offset(control.centerX, control.centerY),
            available * JOYCON_TRIANGLE_RADIUS_FRACTION,
        )
        moveTo(vertices[0].x, vertices[0].y)
        lineTo(vertices[1].x, vertices[1].y)
        lineTo(vertices[2].x, vertices[2].y)
        close()
    }
    val color = (if (held) palette.pressedLabel else palette.label).copy(alpha = alpha)
    drawPath(path, color.copy(alpha = color.alpha * CAPTURE_DISC_FILL_ALPHA))
    drawPath(
        path,
        color.copy(alpha = color.alpha * CAPTURE_DISC_RIM_ALPHA),
        style = Stroke(width = available * JOYCON_TRIANGLE_STROKE_FRACTION),
    )
}

/** Pure geometry kept visible to JVM tests; no arrow font or glyph is involved. */
internal fun joyConDirectionTriangle(
    output: TouchOutputControl,
    center: Offset,
    radius: Float,
): List<Offset> {
    val point = radius
    val wing = radius * 0.82f
    val base = radius * 0.56f
    return when (output) {
        TouchOutputControl.DirectionUp -> listOf(
            Offset(center.x, center.y - point),
            Offset(center.x - wing, center.y + base),
            Offset(center.x + wing, center.y + base),
        )
        TouchOutputControl.DirectionDown -> listOf(
            Offset(center.x, center.y + point),
            Offset(center.x - wing, center.y - base),
            Offset(center.x + wing, center.y - base),
        )
        TouchOutputControl.DirectionLeft -> listOf(
            Offset(center.x - point, center.y),
            Offset(center.x + base, center.y - wing),
            Offset(center.x + base, center.y + wing),
        )
        TouchOutputControl.DirectionRight -> listOf(
            Offset(center.x + point, center.y),
            Offset(center.x - base, center.y - wing),
            Offset(center.x - base, center.y + wing),
        )
        else -> error("$output is not a Joy-Con direction output")
    }
}

private fun DrawScope.drawControlContent(
    glyph: TouchControlGlyph?,
    label: String,
    center: Offset,
    availableWidth: Float,
    availableHeight: Float,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
) {
    when (glyph) {
        TouchControlGlyph.Capture -> drawCaptureGlyph(
            center, minOf(availableWidth, availableHeight), held, palette, alpha,
        )
        TouchControlGlyph.Home -> drawHomeGlyph(
            center, minOf(availableWidth, availableHeight), held, palette, alpha,
        )
        null -> if (label.isNotEmpty()) {
            drawLabel(
                label, center, availableWidth, availableHeight,
                held, palette, alpha, textMeasurer, style,
            )
        }
    }
}

/**
 * A broad, softly filled capture disc inspired by the recessed mark on a physical
 * controller button. The subtle fill keeps it from reading as a generic letter O,
 * while the slimmer rim stays legible against either idle or pressed button faces.
 * It remains Canvas-native rather than depending on an external image asset.
 */
private fun DrawScope.drawCaptureGlyph(
    center: Offset,
    available: Float,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
) {
    val color = (if (held) palette.pressedLabel else palette.label).copy(alpha = alpha)
    val radius = available * CAPTURE_DISC_RADIUS_FRACTION
    drawCircle(
        color = color.copy(alpha = color.alpha * CAPTURE_DISC_FILL_ALPHA),
        radius = radius,
        center = center,
    )
    drawCircle(
        color = color.copy(alpha = color.alpha * CAPTURE_DISC_RIM_ALPHA),
        radius = radius,
        center = center,
        style = Stroke(width = available * CAPTURE_DISC_STROKE_FRACTION),
    )
}

/** Generic house inside a circular outline, also entirely code-native. */
private fun DrawScope.drawHomeGlyph(
    center: Offset,
    available: Float,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
) {
    val color = (if (held) palette.pressedLabel else palette.label).copy(alpha = alpha)
    drawCircle(
        color = color,
        radius = available * HOME_CIRCLE_RADIUS_FRACTION,
        center = center,
        style = Stroke(width = available * HOME_CIRCLE_STROKE_FRACTION),
    )

    val unit = available * HOME_HOUSE_UNIT_FRACTION
    val house = Path().apply {
        moveTo(center.x, center.y - unit)
        lineTo(center.x - unit, center.y - unit * 0.12f)
        lineTo(center.x - unit * 0.72f, center.y - unit * 0.12f)
        lineTo(center.x - unit * 0.72f, center.y + unit * 0.78f)
        lineTo(center.x - unit * 0.25f, center.y + unit * 0.78f)
        lineTo(center.x - unit * 0.25f, center.y + unit * 0.20f)
        lineTo(center.x + unit * 0.25f, center.y + unit * 0.20f)
        lineTo(center.x + unit * 0.25f, center.y + unit * 0.78f)
        lineTo(center.x + unit * 0.72f, center.y + unit * 0.78f)
        lineTo(center.x + unit * 0.72f, center.y - unit * 0.12f)
        lineTo(center.x + unit, center.y - unit * 0.12f)
        close()
    }
    drawPath(house, color)
}

/**
 * The knob follows the axis value, not the raw contact.
 *
 * That is the honest picture: the deadzone and the circular clamp are what the
 * console is being told, so a knob drawn from the finger position would show
 * travel the controller is not sending.
 */
private fun DrawScope.drawStick(
    control: ResolvedTouchControl,
    displacement: TouchVector,
    palette: TouchControlPalette,
    alpha: Float,
    held: Boolean,
) {
    val center = Offset(control.centerX, control.centerY)
    val radius = control.trackingRadius
    drawCircle(
        color = palette.idle.copy(alpha = alpha * WELL_ALPHA),
        radius = radius, center = center,
    )
    drawCircle(
        color = palette.idleOutline.copy(alpha = alpha),
        radius = radius, center = center, style = Stroke(width = OUTLINE_WIDTH),
    )
    val knobRadius = radius * TOUCH_STICK_KNOB_FRACTION
    val travel = radius - knobRadius
    val knob = Offset(
        center.x + displacement.x * travel,
        center.y + displacement.y * travel,
    )
    drawCircle(
        color = (if (held) palette.pressed else palette.idle).copy(alpha = alpha),
        radius = knobRadius, center = knob,
    )
    drawCircle(
        color = (if (held) palette.pressedOutline else palette.idleOutline).copy(alpha = alpha),
        radius = knobRadius, center = knob, style = Stroke(width = OUTLINE_WIDTH),
    )
}

/**
 * A cross, with the active arms lit — both of them on a diagonal.
 *
 * The BODY is one cross, not four wedges radiating from a hub: a star is not the
 * shape a thumb is looking for. The ACTIVE FILL inside it is divided into four
 * wedges that meet at the exact centre, so a lit arm tapers to a point at the
 * hub instead of stopping square against the middle, and a diagonal is the
 * seamless union of two of them.
 *
 * ```text
 *  how the hub divides          Up held           Up + Right held
 *     ┌──┬──┐                  ┌─────┐              ┌─────┐
 *     │  │  │                  │#####│              │#####│
 *  ┌──┼──┼──┼──┐            ┌──┼──┼──┼──┐        ┌──┼──┼──┼──┐
 *  ├──┼──●──┼──┤            ├──┼──●──┼──┤        ├──┼──●####┤
 *  └──┼──┼──┼──┘            └──┼──┼──┼──┘        └──┼──┼──┼──┘
 *     │  │  │                  │     │              │     │
 *     └──┴──┘                  └─────┘              └─────┘
 *   four wedges meet         arm tapers to        one continuous
 *   at one point ●           a point at ●         region, no seam
 * ```
 *
 * Two rules make that safe, and both are easy to undo by accident:
 *
 * - The fill is INTERSECTED with the body path. The body's rounded arm ends are
 *   therefore still the outer boundary of a lit arm. Drawing the wedges without
 *   that intersection — or, as this once did, drawing a rounded rectangle per
 *   direction — squares off the rounded ends, adds a second silhouette on top of
 *   the first, and leaves notches where two overlays meet.
 * - The lit directions are unioned into ONE path and filled ONCE. Filling them
 *   separately double-composites translucent paint along the shared diagonal;
 *   filling them as separate sub-paths risks a hairline seam there.
 */
private fun DrawScope.drawDpad(
    control: ResolvedTouchControl,
    state: DpadState,
    palette: TouchControlPalette,
    alpha: Float,
) {
    val center = Offset(control.centerX, control.centerY)
    val radius = control.trackingRadius
    drawCircle(color = palette.idle.copy(alpha = alpha * WELL_ALPHA), radius = radius, center = center)
    drawCircle(
        color = palette.idleOutline.copy(alpha = alpha),
        radius = radius, center = center, style = Stroke(width = OUTLINE_WIDTH),
    )

    val arm = radius * ARM_FRACTION
    val half = radius * ARM_HALF_WIDTH
    val corner = androidx.compose.ui.geometry.CornerRadius(half * 0.55f)
    val idle = palette.idle.copy(alpha = alpha)
    val held = palette.pressed.copy(alpha = alpha)

    fun Path.bar(left: Float, top: Float, width: Float, height: Float) {
        addRoundRect(
            RoundRect(
                left = left,
                top = top,
                right = left + width,
                bottom = top + height,
                cornerRadius = corner,
            ),
        )
    }

    val idleCross = Path().apply {
        bar(center.x - half, center.y - arm, half * 2f, arm * 2f)
        bar(center.x - arm, center.y - half, arm * 2f, half * 2f)
    }
    drawPath(idleCross, idle)

    val lit = buildList {
        if (state.up) add(TouchCardinalSlot.North)
        if (state.down) add(TouchCardinalSlot.South)
        if (state.left) add(TouchCardinalSlot.West)
        if (state.right) add(TouchCardinalSlot.East)
    }
    if (lit.isEmpty()) return

    val region = lit
        .map { slot ->
            Path().apply {
                val wedge = dpadDirectionWedge(slot, center, arm, half, half * DPAD_WEDGE_OVERSHOOT)
                moveTo(wedge[0].x, wedge[0].y)
                wedge.drop(1).forEach { lineTo(it.x, it.y) }
                close()
            }
        }
        .reduce { merged, wedge -> Path().apply { op(merged, wedge, PathOperation.Union) } }
    drawPath(Path().apply { op(region, idleCross, PathOperation.Intersect) }, held)
}

/**
 * One direction's share of the D-pad, as a closed polygon.
 *
 * The arm, plus the quarter of the central square that belongs to this
 * direction: the square is cut by two diagonals through the exact centre, so all
 * four wedges terminate at one shared point and adjacent wedges share a whole
 * edge. That shared edge is what lets a diagonal press union into a single
 * continuous region with no seam through the middle.
 *
 * The polygon deliberately overshoots the arm END by [overshoot] and is exact on
 * the perpendicular axis. The caller intersects it with the body, so overshooting
 * outward guarantees the lit arm reaches the body's rounded tip with no sliver of
 * unlit colour left behind, while an exact perpendicular extent keeps the wedge
 * from spilling into the neighbouring arm's half of the cross.
 */
internal fun dpadDirectionWedge(
    slot: TouchCardinalSlot,
    center: Offset,
    armLength: Float,
    halfWidth: Float,
    overshoot: Float,
): List<Offset> {
    val outer = armLength + overshoot
    val x = center.x
    val y = center.y
    val h = halfWidth
    return when (slot) {
        TouchCardinalSlot.North -> listOf(
            Offset(x - h, y - outer), Offset(x + h, y - outer),
            Offset(x + h, y - h), center, Offset(x - h, y - h),
        )
        TouchCardinalSlot.South -> listOf(
            Offset(x - h, y + outer), Offset(x + h, y + outer),
            Offset(x + h, y + h), center, Offset(x - h, y + h),
        )
        TouchCardinalSlot.West -> listOf(
            Offset(x - outer, y - h), Offset(x - outer, y + h),
            Offset(x - h, y + h), center, Offset(x - h, y - h),
        )
        TouchCardinalSlot.East -> listOf(
            Offset(x + outer, y - h), Offset(x + outer, y + h),
            Offset(x + h, y + h), center, Offset(x + h, y - h),
        )
    }
}

/**
 * A legend, measured and centred.
 *
 * Bounded on both axes to the control it sits in. Short legends keep the shared
 * global size; longer legends shrink just enough to preserve visible padding.
 * A raised system font scale therefore cannot push text into the button outline.
 */
private fun DrawScope.drawLabel(
    label: String,
    center: Offset,
    availableWidth: Float,
    availableHeight: Float,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
) {
    val color = (if (held) palette.pressedLabel else palette.label).copy(alpha = alpha)

    fun measure(scale: Float) = textMeasurer.measure(
        text = label,
        style = style.copy(
            color = color,
            textAlign = TextAlign.Center,
            fontSize = style.fontSize * scale,
            // Material titleMedium carries its own line height. Scale it with the
            // glyphs so a fitted legend cannot retain an oversized line box.
            lineHeight = style.fontSize * scale,
        ),
        // Unconstrained, so an over-wide legend reports its true width instead of
        // silently wrapping or ellipsising -- which is what makes the shrink below
        // possible at all.
        constraints = Constraints(),
        maxLines = 1,
    )

    var measured = measure(1f)
    val scale = fitTouchLabelScale(
        measuredWidth = measured.size.width.toFloat(),
        measuredHeight = measured.size.height.toFloat(),
        availableWidth = availableWidth,
        availableHeight = availableHeight,
    )
    if (scale < 1f) {
        // One corrective pass is exact enough because text dimensions scale
        // linearly for a fixed typeface and one unwrapped line.
        measured = measure(scale)
    }
    drawText(
        textLayoutResult = measured,
        topLeft = Offset(
            center.x - measured.size.width / 2f,
            center.y - measured.size.height / 2f,
        ),
    )
}

/**
 * Returns a downscale factor that leaves deliberate breathing room around a legend.
 * The result never exceeds 1: the caller's global type size is the visual target,
 * while this function only handles unusually long text, small controls, and large
 * accessibility font scales.
 */
internal fun fitTouchLabelScale(
    measuredWidth: Float,
    measuredHeight: Float,
    availableWidth: Float,
    availableHeight: Float,
): Float {
    val widthRoom = (availableWidth * LABEL_WIDTH_FRACTION).coerceAtLeast(1f)
    val heightRoom = (availableHeight * LABEL_HEIGHT_FRACTION).coerceAtLeast(1f)
    return minOf(
        1f,
        widthRoom / measuredWidth.coerceAtLeast(1f),
        heightRoom / measuredHeight.coerceAtLeast(1f),
    )
}

/**
 * Turn the engine's ownership into the set of controls to draw as pressed.
 *
 * Ownership rather than the published contribution, because they answer
 * different questions: two contacts can hold the same logical button while only
 * one control is under each thumb, and the picture should show what was touched.
 */
fun pressedControlIds(
    layout: ResolvedTouchLayout,
    diagnostics: TouchDiagnosticsSnapshot,
    owned: (String) -> Boolean,
): Set<String> {
    if (diagnostics.ownedControls == 0) return emptySet()
    return layout.controls.mapNotNullTo(mutableSetOf()) { control ->
        control.id.takeIf(owned)
    }
}

/** Debug helper: the hit rectangle of a control, for the layout lab's overlay. */
fun ResolvedTouchControl.hitBounds(): Rect = Rect(
    left = centerX - hitHalfWidth,
    top = centerY - hitHalfHeight,
    right = centerX + hitHalfWidth,
    bottom = centerY + hitHalfHeight,
)

/** Android editor chrome; semantic validation remains in bridge-core. */
/**
 * The editing layer: grid, matched guides, every control's answerable bounds,
 * and what the next edit will actually move.
 *
 * Drawn on TOP of the ordinary controls rather than instead of them. Layout
 * editing is judged by looking at the controller, so the controller has to stay
 * on screen and keep looking like itself; the overlay only adds what direct
 * manipulation needs and cannot otherwise be seen — where the hit region really
 * is, and which controls a drag is about to take with it.
 *
 * ```text
 * grid  ->  guides  ->  every hit region  ->  the edit target  ->  handles
 * ```
 */
fun DrawScope.drawTouchEditorOverlay(
    layout: ResolvedTouchLayout,
    targets: Set<String>,
    primaryId: String?,
    grid: List<TouchGuideLine>,
    guides: List<TouchGuideLine>,
    palette: TouchControlPalette,
) {
    val region = layout.region
    grid.forEach { line -> drawGuide(line, region, palette.idleOutline.copy(alpha = GRID_ALPHA), GRID_WIDTH) }
    guides.forEach { line ->
        drawGuide(
            line,
            region,
            when (line.kind) {
                TouchGuideKind.RegionCenter -> palette.pressed.copy(alpha = GUIDE_ALPHA)
                TouchGuideKind.SafeEdge -> palette.disabled.copy(alpha = GUIDE_ALPHA)
                else -> palette.pressedOutline.copy(alpha = GUIDE_ALPHA)
            },
            GUIDE_WIDTH,
        )
    }

    layout.controls.forEach { control ->
        val bounds = control.hitBounds()
        val target = control.id in targets
        drawRect(
            color = (if (target) palette.pressedOutline else palette.idleOutline).copy(
                alpha = if (target) 0.95f else 0.4f,
            ),
            topLeft = bounds.topLeft,
            size = bounds.size,
            style = Stroke(width = if (target) SELECTED_WIDTH else UNSELECTED_WIDTH),
        )
        // Corner handles on the control the contextual bar is naming, so a
        // multi-control selection still says which one is the reference.
        if (control.id == primaryId) drawSelectionHandles(bounds, palette.pressed)
    }
}

private fun DrawScope.drawGuide(
    line: TouchGuideLine,
    region: dev.picoswitch.bridge.touch.TouchLayoutRegion,
    color: Color,
    width: Float,
) {
    if (line.vertical) {
        drawLine(color, Offset(line.position, region.top), Offset(line.position, region.bottom), width)
    } else {
        drawLine(color, Offset(region.left, line.position), Offset(region.right, line.position), width)
    }
}

private fun DrawScope.drawSelectionHandles(bounds: Rect, color: Color) {
    listOf(
        bounds.topLeft, bounds.topRight, bounds.bottomLeft, bounds.bottomRight,
    ).forEach { corner -> drawCircle(color, HANDLE_RADIUS, corner) }
}

private const val OUTLINE_WIDTH = 2f
private const val WELL_ALPHA = 0.55f
/** Legends stop short of the outline; text touching the rim reads as damage. */
private const val LABEL_WIDTH_FRACTION = 0.78f
/** Vertical padding is slightly more generous because a tight line box looks crowded. */
private const val LABEL_HEIGHT_FRACTION = 0.68f
private const val CAPTURE_DISC_RADIUS_FRACTION = 0.28f
private const val CAPTURE_DISC_FILL_ALPHA = 0.18f
private const val CAPTURE_DISC_RIM_ALPHA = 0.82f
private const val CAPTURE_DISC_STROKE_FRACTION = 0.045f
private const val HOME_CIRCLE_RADIUS_FRACTION = 0.30f
private const val HOME_CIRCLE_STROKE_FRACTION = 0.05f
private const val HOME_HOUSE_UNIT_FRACTION = 0.18f
internal const val TOUCH_STICK_KNOB_FRACTION = 0.46f
private const val JOYCON_TRIANGLE_RADIUS_FRACTION = 0.19f
private const val JOYCON_TRIANGLE_STROKE_FRACTION = 0.045f
private const val ARM_FRACTION = 0.90f
private const val ARM_HALF_WIDTH = 0.26f

/**
 * How far a lit wedge runs past the arm's flat end, as a fraction of the arm's
 * half width.
 *
 * Any positive value works: the fill is intersected with the body, so this only
 * has to be enough that float noise cannot leave an unlit hairline at the tip.
 */
private const val DPAD_WEDGE_OVERSHOOT = 0.25f
private const val DISABLED_ALPHA = 0.4f

/** Editor overlay weights: readable over the controls, never louder than them. */
private const val GRID_ALPHA = 0.16f
private const val GRID_WIDTH = 1f
private const val GUIDE_ALPHA = 0.85f
private const val GUIDE_WIDTH = 2f
private const val SELECTED_WIDTH = 4f
private const val UNSELECTED_WIDTH = 2f
private const val HANDLE_RADIUS = 7f
