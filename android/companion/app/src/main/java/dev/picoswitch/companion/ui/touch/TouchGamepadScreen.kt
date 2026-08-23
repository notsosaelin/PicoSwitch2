package dev.picoswitch.companion.ui.touch

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.LinkOff
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.bridge.touch.ResolvedTouchLayout
import dev.picoswitch.bridge.touch.TouchLayoutRegion
import dev.picoswitch.bridge.touch.TouchLayoutResolver
import dev.picoswitch.bridge.touch.TouchLayoutV1
import dev.picoswitch.bridge.touch.TouchReleaseReason
import dev.picoswitch.companion.bridge.AndroidTouchFeedback
import dev.picoswitch.companion.data.TouchGamepadSettings
import dev.picoswitch.companion.ui.CompanionUiState
import dev.picoswitch.companion.ui.CompanionViewModel
import dev.picoswitch.bridge.touch.TouchFeedbackBackend
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * The on-screen controller, full screen.
 *
 * A dedicated application MODE rather than a destination inside the ordinary
 * scaffold: it owns edge-to-edge presentation, hides the navigation chrome, and
 * has its own back behaviour. Rendering it inside the content column would give
 * it the scaffold's insets and width limit, which is the opposite of what a
 * gameplay surface needs.
 *
 * ```text
 * measured area  -  system gesture / cutout insets
 *        |
 *        v
 * TouchLayoutResolver          portable
 *        |
 *        v            Compose pointer events
 * TouchControlEngine  <---- TouchContactTracker  <---- touchGamepadContacts
 *        |
 *        v
 * ControllerInputState -> BridgeSession -> the adapter
 * ```
 *
 * The background may draw edge to edge; the CONTROLS are resolved inside the
 * interaction-safe rectangle so nothing important lives under a back-gesture
 * strip or a display cutout.
 */
@Composable
fun TouchGamepadScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onPickBackgroundImage: () -> Unit,
    /** Start or restart the controller link without leaving the mode. */
    onRetryLink: () -> Unit,
) {
    val gamepad = viewModel.touchGamepad
    val view = LocalView.current
    val context = LocalContext.current
    val density = LocalDensity.current
    val layoutDirection = LocalLayoutDirection.current
    val settings = ui.touchSettings

    var menuOpen by rememberSaveable { mutableStateOf(false) }
    var area by remember { mutableStateOf(IntSize.Zero) }

    // Visual state is LOCAL and mutated from the pointer handler. Hanging it off
    // the application state would recompose the whole app for every pixel a thumb
    // moves; the semantic input has already reached the bridge by then, and this
    // is only the picture.
    val visual = remember { mutableStateOf(TouchVisualState()) }

    /**
     * Recompute the picture from what the engine is holding.
     *
     * Called from the pointer path after a whole batch and from each release
     * boundary — never on a timer. A per-frame poll would keep the frame clock
     * running for as long as the controller is on screen, and this surface is
     * idle whenever no thumb is moving.
     */
    fun refreshVisual() {
        val snapshot = gamepad.diagnostics()
        val layout = gamepad.engine.resolvedLayout
        val next = visual.value.copy(
            leftStick = snapshot.leftStick,
            rightStick = snapshot.rightStick,
            dpad = snapshot.dpad,
            pressed = pressedControlIds(layout, snapshot) { gamepad.engine.contactOn(it) != null },
        )
        if (next != visual.value) visual.value = next
    }

    val insets = WindowInsets.safeContent
    val left = insets.getLeft(density, layoutDirection)
    val top = insets.getTop(density)
    val right = insets.getRight(density, layoutDirection)
    val bottom = insets.getBottom(density)

    val resolved = remember(area, left, top, right, bottom, density.density) {
        if (area.width == 0 || area.height == 0) {
            ResolvedTouchLayout.Empty
        } else {
            TouchLayoutResolver.resolve(
                TouchLayoutV1.layout,
                TouchLayoutRegion(
                    left = left.toFloat(),
                    top = top.toFloat(),
                    right = (area.width - right).toFloat(),
                    bottom = (area.height - bottom).toFloat(),
                    unitScale = density.density,
                ),
            )
        }
    }

    // Geometry changed: every retained contact position was measured against the
    // previous rectangle and means nothing now.
    LaunchedEffect(resolved) {
        gamepad.engine.setLayout(resolved)
        visual.value = TouchVisualState(enabled = ui.bridge.phase == BridgeLinkPhase.Playing)
    }

    LaunchedEffect(settings.hapticsEnabled) {
        gamepad.setFeedbackBackend(
            if (settings.hapticsEnabled) AndroidTouchFeedback(view) else TouchFeedbackBackend.None,
        )
    }

    // Opening the menu is an interruption like any other: whatever was held has
    // no contact left that will release it.
    LaunchedEffect(menuOpen) {
        if (menuOpen) gamepad.release(TouchReleaseReason.HostInactive)
        refreshVisual()
    }

    LaunchedEffect(ui.bridge.phase) {
        visual.value = visual.value.copy(enabled = ui.bridge.phase == BridgeLinkPhase.Playing)
    }

    ImmersivePresentation(landscapePreferred = true)
    ReleaseOnHostInactive(
        onRelease = {
            viewModel.releaseTouchInput(it)
            refreshVisual()
        },
    )

    BackHandler(enabled = true) {
        if (menuOpen) menuOpen = false else viewModel.exitTouchGamepad()
    }

    val background = rememberTouchBackground(settings.backgroundImage)
    val textMeasurer = rememberTextMeasurer()
    val palette = touchControlPalette()
    val labelStyle = MaterialTheme.typography.titleMedium.copy(
        fontSize = 15.sp,
        fontWeight = FontWeight.SemiBold,
    )

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .onSizeChanged { area = it },
    ) {
        background?.let { image ->
            Image(
                bitmap = image,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
            // Readability over a bright photo. Decoration only; it can never move
            // a hit region.
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = settings.backgroundDim)),
            )
        }

        if (!resolved.fits) {
            UnusableWindowNotice(resolved.problem, onExit = viewModel::exitTouchGamepad)
        } else {
            Canvas(
                modifier = Modifier
                    .fillMaxSize()
                    .semantics { contentDescription = CONTROLLER_DESCRIPTION }
                    .then(
                        if (menuOpen) {
                            Modifier
                        } else {
                            Modifier.touchGamepadContacts(
                                key = resolved,
                                tracker = gamepad.contacts,
                                afterBatch = ::refreshVisual,
                            )
                        },
                    ),
            ) {
                // Read INSIDE the draw lambda: the write from the pointer handler
                // then invalidates drawing only, never composition.
                drawTouchControls(
                    layout = resolved,
                    visual = visual.value,
                    palette = palette,
                    faceLayout = ui.touchFaceLayout,
                    opacity = settings.controlOpacity,
                    textMeasurer = textMeasurer,
                    labelStyle = labelStyle,
                )
            }
        }

        // In the layout's quiet centre, not over the controls. The middle band is
        // kept free by the layout itself, so a status strip there cannot shadow
        // anything the user is trying to press.
        val bannerTop = with(density) {
            (resolved.region.top + resolved.region.height * BANNER_BAND).toDp()
        }
        LinkStatusBanner(
            phase = ui.bridge.phase,
            message = ui.bridge.message,
            onRetry = onRetryLink,
            modifier = Modifier.align(Alignment.TopCenter).padding(top = bannerTop),
        )

        if (menuOpen) {
            TouchGamepadMenu(
                settings = settings,
                faceLayout = ui.touchFaceLayout,
                onSettings = viewModel::setTouchSettings,
                onFaceLayout = viewModel::setTouchFaceLayout,
                onPickBackground = onPickBackgroundImage,
                onClearBackground = { viewModel.setTouchBackground(null) },
                onExit = {
                    menuOpen = false
                    viewModel.exitTouchGamepad()
                },
                onClose = { menuOpen = false },
            )
        }
    }

    // The engine reports a menu tap through the portable path, so the control is
    // audited for overlap alongside every gameplay control instead of floating
    // above them where a stray thumb could shadow it.
    DisposableEffect(gamepad) {
        gamepad.onMenuRequested { menuOpen = true }
        onDispose {
            gamepad.onMenuRequested { }
            gamepad.release(TouchReleaseReason.Disposed)
        }
    }
}

/**
 * Edge-to-edge, bars hidden, landscape preferred while the controller is open.
 *
 * Every change is undone on the way out. The bars are hidden with the transient
 * behaviour so a swipe always brings them back — a gameplay surface the user
 * cannot leave by any ordinary means is not immersive, it is a trap.
 *
 * The orientation is a REQUEST. Large-screen and windowed configurations may
 * ignore it, which is correct and is why nothing below depends on it: the layout
 * resolves against whatever rectangle actually arrives.
 */
@Composable
private fun ImmersivePresentation(landscapePreferred: Boolean) {
    val view = LocalView.current
    val context = LocalContext.current
    DisposableEffect(landscapePreferred) {
        val activity = context.findActivity()
        val window = activity?.window
        val previousOrientation = activity?.requestedOrientation
        val controller = window?.let { WindowInsetsControllerCompat(it, view) }

        if (window != null) WindowCompat.setDecorFitsSystemWindows(window, false)
        controller?.apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(androidx.core.view.WindowInsetsCompat.Type.systemBars())
        }
        if (landscapePreferred && activity != null) {
            activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        }

        onDispose {
            controller?.show(androidx.core.view.WindowInsetsCompat.Type.systemBars())
            if (window != null) WindowCompat.setDecorFitsSystemWindows(window, true)
            if (activity != null && previousOrientation != null) {
                activity.requestedOrientation = previousOrientation
            }
        }
    }
}

/**
 * Release on every host-inactivity boundary the surface can observe.
 *
 * ON_PAUSE covers the screen turning off, the app being backgrounded, a system
 * picker taking focus and the lock screen appearing. The Activity neutralizes
 * the session on the same event, so the console is told as well as the engine
 * being cleared — but this is the one that runs even if the surface is composed
 * somewhere the Activity's own hook does not reach.
 */
@Composable
private fun ReleaseOnHostInactive(onRelease: (TouchReleaseReason) -> Unit) {
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_PAUSE, Lifecycle.Event.ON_STOP ->
                    onRelease(TouchReleaseReason.HostInactive)
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
}

/**
 * The truth about the link, without replacing the controller with an error page.
 *
 * An automatic reconnect is plausible and usually quick, so the layout stays put
 * and the state is stated in a strip. Silence would be worse than either: a
 * controller that looks fine and does nothing is the hardest failure to report.
 */
@Composable
private fun LinkStatusBanner(
    phase: BridgeLinkPhase,
    message: String?,
    onRetry: () -> Unit,
    modifier: Modifier = Modifier,
) {
    if (phase == BridgeLinkPhase.Playing) return
    val text = when (phase) {
        BridgeLinkPhase.Idle -> "Not connected to the adapter"
        BridgeLinkPhase.Preparing, BridgeLinkPhase.Registering -> "Preparing the controller link…"
        BridgeLinkPhase.Connecting -> "Connecting to the adapter…"
        BridgeLinkPhase.Ready -> "Ready — reconnecting the controller link…"
        BridgeLinkPhase.Unsupported -> "This device cannot act as a controller"
        BridgeLinkPhase.Failed -> message ?: "The controller link failed"
        BridgeLinkPhase.Playing -> return
    }
    val failed = phase == BridgeLinkPhase.Failed || phase == BridgeLinkPhase.Unsupported
    // A spinner is a claim that something is happening. Idle is not something
    // happening -- nothing is in flight and nothing will be until the user acts —
    // so it gets an icon like any other resting state.
    val working = phase == BridgeLinkPhase.Preparing || phase == BridgeLinkPhase.Registering ||
        phase == BridgeLinkPhase.Connecting || phase == BridgeLinkPhase.Ready
    Surface(
        modifier = modifier.padding(8.dp),
        // A container colour rather than a translucent surface: over a black
        // background or a dark photograph, "surface at 86%" is indistinguishable
        // from nothing at all, and an unreadable status is worse than none.
        color = if (failed) MaterialTheme.colorScheme.errorContainer
        else MaterialTheme.colorScheme.secondaryContainer,
        contentColor = if (failed) MaterialTheme.colorScheme.onErrorContainer
        else MaterialTheme.colorScheme.onSecondaryContainer,
        shape = MaterialTheme.shapes.small,
        tonalElevation = 3.dp,
    ) {
        Row(
            Modifier
                .padding(horizontal = 14.dp, vertical = 8.dp)
                .semantics { liveRegion = LiveRegionMode.Polite },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            when {
                working -> CircularProgressIndicator(
                    Modifier.size(14.dp),
                    strokeWidth = 2.dp,
                    color = LocalContentColor.current,
                )
                failed -> Icon(Icons.Default.Warning, null, Modifier.size(16.dp))
                else -> Icon(Icons.Default.LinkOff, null, Modifier.size(16.dp))
            }
            Spacer(Modifier.width(8.dp))
            Text(text, style = MaterialTheme.typography.labelLarge)
            // A link that is not going to fix itself must be actionable from
            // here. Leaving the only recovery outside the mode would mean the
            // answer to "it says it cannot connect" is "leave and come back".
            if (!working) {
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = onRetry, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text("Retry")
                }
            }
        }
    }
}

/**
 * Shown instead of the controller when the window genuinely cannot hold it.
 *
 * Drawing an overlapping controller here would be worse than refusing: the user
 * would press one control and the console would receive another.
 */
@Composable
private fun UnusableWindowNotice(problem: String?, onExit: () -> Unit) {
    Box(Modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                problem ?: "This window is too small for the on-screen controller",
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Text(
                "Turn the device sideways or make the window larger.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = onExit, modifier = Modifier.heightIn(min = 48.dp)) {
                Text("Leave the on-screen controller")
            }
        }
    }
}

/**
 * The controller's own settings, and the way out.
 *
 * Scoped here rather than added to the application's Settings page: these are
 * decisions about one surface, and none of them mean anything anywhere else.
 */
@Composable
private fun TouchGamepadMenu(
    settings: TouchGamepadSettings,
    faceLayout: ControllerFaceLayout,
    onSettings: (TouchGamepadSettings) -> Unit,
    onFaceLayout: (ControllerFaceLayout) -> Unit,
    onPickBackground: () -> Unit,
    onClearBackground: () -> Unit,
    onExit: () -> Unit,
    onClose: () -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.6f)),
        contentAlignment = Alignment.Center,
    ) {
        Card(
            Modifier
                .widthIn(max = 460.dp)
                .fillMaxWidth(0.9f)
                .windowInsetsPadding(WindowInsets.safeContent),
        ) {
            Column(
                Modifier.padding(20.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Touch Gamepad",
                        Modifier.weight(1f),
                        style = MaterialTheme.typography.titleLarge,
                    )
                    IconButton(onClick = onClose, modifier = Modifier.size(48.dp)) {
                        Icon(Icons.Default.Close, "Close the Touch Gamepad menu")
                    }
                }

                Text("Face buttons", style = MaterialTheme.typography.titleSmall)
                Text(
                    "Chooses the letters drawn on the diamond and what each position sends.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                // Auto is not offered: it exists to guess a PRINTED legend, and a
                // drawn control has none.
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(ControllerFaceLayout.Nintendo, ControllerFaceLayout.Xbox).forEach { option ->
                        FilterChip(
                            selected = faceLayout == option,
                            onClick = { onFaceLayout(option) },
                            label = { Text(option.title) },
                            modifier = Modifier.heightIn(min = 48.dp),
                        )
                    }
                }

                SettingSlider(
                    label = "Control opacity",
                    value = settings.controlOpacity,
                    range = TouchGamepadSettings.MIN_OPACITY..TouchGamepadSettings.MAX_OPACITY,
                    onValue = { onSettings(settings.copy(controlOpacity = it)) },
                )
                SettingSlider(
                    label = "Stick deadzone",
                    value = settings.stickDeadzone,
                    range = 0f..TouchGamepadSettings.MAX_DEADZONE,
                    onValue = { onSettings(settings.copy(stickDeadzone = it)) },
                )
                SettingSlider(
                    label = "Background dim",
                    value = settings.backgroundDim,
                    range = 0f..TouchGamepadSettings.MAX_DIM,
                    onValue = { onSettings(settings.copy(backgroundDim = it)) },
                )

                Row(
                    Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("Touch feedback", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "A short local buzz when a control is pressed. Console rumble is separate.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(
                        checked = settings.hapticsEnabled,
                        onCheckedChange = { onSettings(settings.copy(hapticsEnabled = it)) },
                    )
                }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(
                        onClick = onPickBackground,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text("Background image") }
                    if (settings.backgroundImage != null) {
                        OutlinedButton(
                            onClick = onClearBackground,
                            modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                        ) { Text("Remove") }
                    }
                }

                Button(
                    onClick = onExit,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                ) { Text("Exit Touch Gamepad") }
            }
        }
    }
}

@Composable
private fun SettingSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValue: (Float) -> Unit,
) {
    Column {
        Text(
            "$label  ${(value * 100).toInt()}%",
            style = MaterialTheme.typography.bodyMedium,
        )
        Slider(
            value = value,
            onValueChange = onValue,
            valueRange = range,
            modifier = Modifier.semantics { contentDescription = label },
        )
    }
}

/** Decoded off the contact path, and only when the stored copy actually changes. */
@Composable
private fun rememberTouchBackground(path: String?): ImageBitmap? {
    var image by remember(path) { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(path) {
        image = if (path == null) null else withContext(Dispatchers.IO) { TouchBackgroundStore.load(path) }
    }
    return image
}

@Composable
private fun touchControlPalette(): TouchControlPalette {
    val scheme = MaterialTheme.colorScheme
    return remember(scheme) {
        TouchControlPalette(
            idle = scheme.surfaceVariant,
            idleOutline = scheme.outline,
            pressed = scheme.primary,
            pressedOutline = scheme.primary,
            label = scheme.onSurface,
            pressedLabel = scheme.onPrimary,
            disabled = scheme.outline,
        )
    }
}

private fun Context.findActivity(): Activity? {
    var current = this
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

/**
 * Where the status strip sits, as a fraction down the interaction area.
 *
 * Inside the quiet middle band the layout deliberately keeps free — below the
 * top row of shoulders and centre controls, above `-` and `+`.
 */
private const val BANNER_BAND = 0.22f

private const val CONTROLLER_DESCRIPTION =
    "On-screen controller. Sticks, D-pad, face buttons, shoulders and triggers are " +
        "positional touch controls; use the menu control at the top to change settings or exit."
