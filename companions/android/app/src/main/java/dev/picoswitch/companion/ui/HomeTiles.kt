package dev.picoswitch.companion.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Whether focus should be drawn: only while the user drives the app with keys
 * or a gamepad. Compose switches the input mode on the first key or touch, so a
 * touch user never sees a focus ring and a pad user always does.
 */
@Composable
internal fun keyNavigation(): Boolean = LocalInputModeManager.current.inputMode == InputMode.Keyboard

/**
 * A console-home tile: the focused tile lifts (scale + accent outline +
 * shadow) the way a game launcher shows its cursor, and a press sinks it a
 * little. The motion is a spring so a fast D-pad sweep stays fluid instead of
 * queueing animations.
 */
@Composable
internal fun HomeTile(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    container: Color = MaterialTheme.colorScheme.surfaceContainerHigh,
    content: Color = MaterialTheme.colorScheme.onSurface,
    shape: Shape = RoundedCornerShape(LayoutTokens.CardRadius),
    padding: PaddingValues = PaddingValues(LayoutTokens.Space4),
    description: String? = null,
    body: @Composable BoxScope.() -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val pressed by interaction.collectIsPressedAsState()
    val lifted = focused && keyNavigation()
    // The lift is a fixed few dp, not a percentage: 3.5 % of a full-height
    // tile pushed it past the top of the screen (the Amiibo tile, 2026-11-08).
    var longestSide by remember { mutableIntStateOf(0) }
    val maxGrowPx = with(LocalDensity.current) { TILE_MAX_GROW.toPx() }
    val liftScale = if (longestSide > 0) minOf(1.035f, 1f + 2f * maxGrowPx / longestSide) else 1f
    val scale by animateFloatAsState(
        when {
            pressed -> 0.985f
            lifted -> liftScale
            else -> 1f
        },
        spring(dampingRatio = 0.7f, stiffness = Spring.StiffnessMediumLow),
        label = "tile-scale",
    )
    val outline by animateColorAsState(
        if (lifted) MaterialTheme.colorScheme.primary else Color.Transparent,
        tween(120),
        label = "tile-outline",
    )
    Surface(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier
            .onSizeChanged { longestSide = maxOf(it.width, it.height) }
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
            }
            .then(if (description != null) Modifier.semanticsDescription(description) else Modifier),
        shape = shape,
        color = container,
        contentColor = content,
        border = BorderStroke(3.dp, outline),
        shadowElevation = if (lifted) 10.dp else 1.dp,
        interactionSource = interaction,
    ) {
        Box(Modifier.padding(padding).alpha(if (enabled) 1f else LayoutTokens.DisabledAlpha + 0.2f), content = body)
    }
}

/** How far a lifted tile may grow past each edge: inside the page's own padding. */
private val TILE_MAX_GROW = 3.dp

private fun Modifier.semanticsDescription(text: String): Modifier =
    semantics { contentDescription = text }

/** A small pill that pulses while something is live: "LIVE", "PLAYING". */
@Composable
internal fun LiveBadge(label: String, color: Color = MaterialTheme.colorScheme.error) {
    val pulse = rememberInfiniteTransition(label = "live")
    val dot by pulse.animateFloat(
        initialValue = 1f,
        targetValue = 0.35f,
        animationSpec = infiniteRepeatable(tween(900), RepeatMode.Reverse),
        label = "live-dot",
    )
    Surface(shape = CircleShape, color = color.copy(alpha = 0.16f), contentColor = color) {
        Row(Modifier.padding(horizontal = 10.dp, vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(8.dp).alpha(dot).background(color, CircleShape))
            Spacer(Modifier.width(6.dp))
            Text(label, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.2.sp, maxLines = 1, softWrap = false)
        }
    }
}

/** How loud an [ActionButton] is. One shape and size for all three. */
internal enum class ActionTone { Primary, Danger, Tonal }

/**
 * The app's one action button: every action on Play (Start / Stop, Home,
 * Capture, C, the quick actions) is this, at one height, one shape and one type
 * size, so a row of them reads as a set. Either a click ([onClick]) or a hold
 * ([onHold], press and release reported separately, for console buttons whose
 * meaning depends on how long they are held).
 */
@Composable
internal fun ActionButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    label: String,
    modifier: Modifier = Modifier,
    tone: ActionTone = ActionTone.Tonal,
    enabled: Boolean = true,
    busy: Boolean = false,
    onClick: (() -> Unit)? = null,
    onHold: ((Boolean) -> Unit)? = null,
) {
    val colors = MaterialTheme.colorScheme
    val (container, content) = when (tone) {
        ActionTone.Primary -> colors.primary to colors.onPrimary
        ActionTone.Danger -> colors.errorContainer to colors.onErrorContainer
        ActionTone.Tonal -> colors.secondaryContainer to colors.onSecondaryContainer
    }
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val lifted = focused && keyNavigation()
    val scale by animateFloatAsState(
        if (lifted) 1.04f else 1f,
        spring(dampingRatio = 0.7f, stiffness = Spring.StiffnessMediumLow),
        label = "action-scale",
    )
    val outline by animateColorAsState(
        if (lifted) colors.onSurface else Color.Transparent, tween(120), label = "action-outline",
    )
    val shape = RoundedCornerShape(LayoutTokens.ActionRadius)
    val sized = modifier
        .height(LayoutTokens.ActionHeight)
        .graphicsLayer {
            scaleX = scale
            scaleY = scale
        }
        .semantics { contentDescription = label }
    val body: @Composable () -> Unit = {
        Row(
            Modifier.fillMaxSize().padding(horizontal = LayoutTokens.Space3)
                .alpha(if (enabled) 1f else LayoutTokens.DisabledAlpha + 0.1f),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
        ) {
            if (busy) {
                androidx.compose.material3.CircularProgressIndicator(
                    Modifier.size(20.dp), strokeWidth = 2.dp, color = content,
                )
            } else {
                androidx.compose.material3.Icon(icon, null, Modifier.size(22.dp))
            }
            Spacer(Modifier.width(LayoutTokens.Space2))
            Text(
                label,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
            )
        }
    }
    if (onHold != null) {
        Surface(
            sized.pointerInput(label, enabled) {
                if (!enabled) return@pointerInput
                detectTapGestures(
                    onPress = {
                        onHold(true)
                        // Releases on cancel too, so a drag off the button can
                        // never latch it down.
                        tryAwaitRelease()
                        onHold(false)
                    },
                )
            },
            shape = shape,
            color = container,
            contentColor = content,
            border = BorderStroke(2.dp, outline),
            content = body,
        )
    } else {
        Surface(
            onClick = { onClick?.invoke() },
            enabled = enabled && !busy,
            modifier = sized,
            shape = shape,
            color = container,
            contentColor = content,
            border = BorderStroke(2.dp, outline),
            interactionSource = interaction,
            content = body,
        )
    }
}
