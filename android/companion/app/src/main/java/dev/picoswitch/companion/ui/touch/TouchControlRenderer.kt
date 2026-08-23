package dev.picoswitch.companion.ui.touch

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
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
import dev.picoswitch.bridge.touch.TouchControlAction
import dev.picoswitch.bridge.touch.TouchDiagnosticsSnapshot
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
 * nineteen independently recomposing nodes, a stick move invalidates layout and
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
        when (val action = control.spec.action) {
            is TouchControlAction.Stick -> drawStick(
                control = control,
                displacement = if (action.side == ControlSide.Left) visual.leftStick else visual.rightStick,
                palette = palette, alpha = alpha, held = held,
            )
            TouchControlAction.Directions -> drawDpad(control, visual.dpad, palette, alpha)
            is TouchControlAction.Face -> drawRound(
                control, held, palette, alpha, textMeasurer, labelStyle,
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
    if (label.isNotEmpty()) {
        drawLabel(label, center, radius * 2f, held, palette, alpha, textMeasurer, style)
    }
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
    if (control.spec.label.isNotEmpty()) {
        drawLabel(
            control.spec.label, Offset(control.centerX, control.centerY),
            size.width, held, palette, alpha, textMeasurer, style,
        )
    }
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
    val knobRadius = radius * KNOB_FRACTION
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
 * Drawn as a whole cross first and then the held halves on top, rather than as
 * four separate wedges. Wedges radiating from a hub read as a star at a glance,
 * and the shape a thumb is looking for is the one it already knows.
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

    fun bar(color: Color, left: Float, top: Float, width: Float, height: Float) = drawRoundRect(
        color = color,
        topLeft = Offset(left, top),
        size = Size(width, height),
        cornerRadius = corner,
    )

    bar(idle, center.x - half, center.y - arm, half * 2f, arm * 2f)
    bar(idle, center.x - arm, center.y - half, arm * 2f, half * 2f)
    if (state.up) bar(held, center.x - half, center.y - arm, half * 2f, arm)
    if (state.down) bar(held, center.x - half, center.y, half * 2f, arm)
    if (state.left) bar(held, center.x - arm, center.y - half, arm, half * 2f)
    if (state.right) bar(held, center.x, center.y - half, arm, half * 2f)
}

/**
 * A legend, measured and centred.
 *
 * Bounded to the control it sits in: a raised system font scale must not push a
 * one-character label past the button drawn around it. The accessible thing here
 * is the hit region, which the layout already guarantees, not an enormous letter.
 */
private fun DrawScope.drawLabel(
    label: String,
    center: Offset,
    available: Float,
    held: Boolean,
    palette: TouchControlPalette,
    alpha: Float,
    textMeasurer: TextMeasurer,
    style: TextStyle,
) {
    val color = (if (held) palette.pressedLabel else palette.label).copy(alpha = alpha)
    // Room inside the control, not the control's full width: a legend that runs
    // to the outline reads as damage.
    val room = (available * LABEL_WIDTH_FRACTION).coerceAtLeast(1f)

    fun measure(scale: Float) = textMeasurer.measure(
        text = label,
        style = style.copy(
            color = color,
            textAlign = TextAlign.Center,
            fontSize = style.fontSize * scale,
        ),
        // Unconstrained, so an over-wide legend reports its true width instead of
        // silently wrapping or ellipsising -- which is what makes the shrink below
        // possible at all.
        constraints = Constraints(),
        maxLines = 1,
    )

    var measured = measure(1f)
    if (measured.size.width > room) {
        // One corrective pass. Shrinking is bounded so a raised system font scale
        // cannot push a legend past the button drawn around it, and cannot make it
        // microscopic either -- the accessible thing here is the hit region, which
        // the layout already guarantees.
        val scale = (room / measured.size.width).coerceAtLeast(MIN_LABEL_SCALE)
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

private const val OUTLINE_WIDTH = 2f
private const val WELL_ALPHA = 0.55f
/** Legends stop short of the outline; text touching the rim reads as damage. */
private const val LABEL_WIDTH_FRACTION = 0.78f
/** Below this a legend is decoration rather than a label, so it stops shrinking. */
private const val MIN_LABEL_SCALE = 0.62f
private const val KNOB_FRACTION = 0.46f
private const val ARM_FRACTION = 0.90f
private const val ARM_HALF_WIDTH = 0.26f
private const val DISABLED_ALPHA = 0.4f
