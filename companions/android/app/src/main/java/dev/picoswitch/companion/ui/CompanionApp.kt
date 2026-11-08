package dev.picoswitch.companion.ui

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.togetherWith
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.data.AdapterId
import dev.picoswitch.companion.ui.touch.TouchGamepadScreen

/**
 * The shell: the page and the navigation, at the edge the player chose, docked
 * or gone (docs/architecture/companion-shell-layout.md; see [NavShell]).
 */
@Composable
fun CompanionApp(
    viewModel: CompanionViewModel,
    onConnectAdapter: () -> Unit,
    onPairAdapter: () -> Unit,
    onRepairAdapter: (AdapterId?) -> Unit,
    onImportAmiibo: () -> Unit,
    onImportAmiiboFolder: () -> Unit,
    onExportAmiiboArchive: () -> Unit,
    onScanAmiibo: () -> Unit,
    onImportAmiiboKeys: () -> Unit,
    onPrepareController: () -> Unit,
    onOpenTouchGamepad: () -> Unit,
    onPickTouchBackground: () -> Unit,
    onExportDiagnostics: () -> Unit,
) {
    val ui by viewModel.ui.collectAsStateWithLifecycle()
    val theme by viewModel.theme.collectAsStateWithLifecycle()
    val snackbarHostState = remember { SnackbarHostState() }
    val destinationState = rememberSaveableStateHolder()
    LaunchedEffect(ui.message) {
        ui.message?.let { snackbarHostState.showSnackbar(it); viewModel.consumeMessage() }
    }

    FullScreenPresentation(touchGamepadActive = ui.touchGamepadActive)

    CompanionTheme(theme) {
        // A full-screen application MODE, above the shell rather than inside it.
        // The on-screen controller owns edge-to-edge presentation, hides the
        // chrome and handles its own back behaviour.
        if (ui.touchGamepadActive) {
            TouchGamepadScreen(ui, viewModel, onPickTouchBackground, onRetryLink = onPrepareController)
            return@CompanionTheme
        }

        val streaming = ui.bridge.phase == BridgeLinkPhase.Playing

        BoxWithConstraints(Modifier.fillMaxSize()) {
            val page: @Composable (Modifier) -> Unit = { modifier ->
                Box(modifier) {
                    PageHost(ui.section) { section ->
                        destinationState.SaveableStateProvider(section.name) {
                            when (section) {
                                AppSection.Play -> PlayScreen(
                                    ui, viewModel, onPrepareController, onOpenTouchGamepad, onConnectAdapter,
                                )
                                AppSection.Keyboard -> KeyboardMouseScreen(ui, viewModel)
                                // The one page that scrolls: the library is a
                                // collection of any length. The grid owns that
                                // scroll, so the frame adds none.
                                AppSection.Amiibo -> PageFrame(LayoutTokens.WidePageMaxWidth, scroll = false) {
                                    AmiiboScreen(
                                        ui, viewModel, onImportAmiibo, onImportKeys = onImportAmiiboKeys,
                                        onScan = onScanAmiibo,
                                    )
                                }
                                AppSection.Customization -> CustomizationScreen(ui, viewModel)
                                AppSection.Adapter -> AdapterScreen(
                                    ui, viewModel, onConnectAdapter, onPairAdapter, onRepairAdapter,
                                )
                                AppSection.Settings -> SettingsScreen(ui, viewModel, theme)
                            }
                        }
                    }
                    OverlayHost(
                        ui, viewModel, onExportDiagnostics,
                        onImportAmiibo, onImportAmiiboFolder,
                        onExportAmiiboArchive, onImportAmiiboKeys,
                    )
                    SnackbarHost(snackbarHostState, Modifier.align(Alignment.BottomCenter))
                }
            }
            ProvideShortWindow(maxHeight) {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    NavShell(
                        ui,
                        placement = NavPlacement.resolve(theme.navigation, landscape = maxWidth > maxHeight),
                        onSelect = viewModel::navigate,
                        onShownChange = viewModel::setNavigationShown,
                        page = page,
                    )
                    ui.operation?.let { OperationOverlay(it) }
                }
            }
        }
    }
}

/** The app's one motion curve: short, decelerating, never bouncy. */
internal fun <T> motionSpec(durationMillis: Int = 220) =
    tween<T>(durationMillis, easing = FastOutSlowInEasing)

/**
 * Page changes cross-fade with a slight rise, so moving between pages reads as
 * one surface settling rather than a cut. Short enough that L1 / R1 paging
 * through every page never queues up.
 */
@Composable
private fun PageHost(section: AppSection, content: @Composable (AppSection) -> Unit) {
    AnimatedContent(
        targetState = section,
        transitionSpec = {
            (fadeIn(motionSpec(220)) + slideInVertically(motionSpec(220)) { it / 40 }) togetherWith
                fadeOut(motionSpec(120))
        },
        label = "page",
    ) { target -> content(target) }
}

/**
 * Always full screen: the app hides the system bars on every page and there is
 * no toggle (docs/architecture/companion-shell-layout.md, "Top bar"). On a
 * landscape handheld the status and navigation bars are a tenth of the height,
 * and the pages are laid out to fit the window without scrolling.
 *
 * The bars are hidden with Android's transient behaviour, so a swipe from the
 * edge still shows them briefly; that is the platform's own rule for immersive
 * mode and the way out of the app is never taken away.
 *
 * Applied again on every resume: returning from another activity (a file
 * picker, the Bluetooth pairing dialog) can bring the bars back. The Touch
 * Gamepad hides and reshows the bars itself, so while it is open this stands
 * aside, and when it closes this runs again.
 */
@Composable
private fun FullScreenPresentation(touchGamepadActive: Boolean) {
    val view = LocalView.current
    val context = LocalContext.current
    LifecycleResumeEffect(touchGamepadActive) {
        if (!touchGamepadActive) {
            context.findActivity()?.window?.let { window ->
                WindowInsetsControllerCompat(window, view).apply {
                    systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                    hide(WindowInsetsCompat.Type.systemBars())
                }
            }
        }
        onPauseOrDispose {}
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

/**
 * Screens pushed over the current section.
 *
 * They slide in rather than cutting so the relationship to the page behind them
 * stays obvious; there are only two, so this is cheaper and more predictable
 * than adding a navigation graph.
 */
@Composable
private fun OverlayHost(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onExportDiagnostics: () -> Unit,
    onImportAmiiboFiles: () -> Unit,
    onImportAmiiboFolder: () -> Unit,
    onExportAmiiboArchive: () -> Unit,
    onImportAmiiboKeys: () -> Unit,
) {
    // Back closes an overlay (Diagnostics, Amiibo settings); it never leaves the app.
    androidx.activity.compose.BackHandler(enabled = ui.overlay != AppOverlay.None) { viewModel.closeOverlay() }
    AnimatedVisibility(
        visible = ui.overlay != AppOverlay.None,
        enter = slideInHorizontally(initialOffsetX = { it / 3 }) + fadeIn(),
        exit = slideOutHorizontally(targetOffsetX = { it / 3 }) + fadeOut(),
    ) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
            Box(Modifier.padding(horizontal = LayoutTokens.Space4)) {
                when (ui.overlay) {
                    AppOverlay.Diagnostics -> DiagnosticsScreen(ui, viewModel, onExportDiagnostics)
                    AppOverlay.AmiiboSettings -> AmiiboSettingsScreen(
                        ui, viewModel, onImportAmiiboFiles, onImportAmiiboFolder,
                        onExportAmiiboArchive, onImportAmiiboKeys,
                    )
                    AppOverlay.None -> Unit
                }
            }
        }
    }
}

@Composable
private fun OperationOverlay(progress: dev.picoswitch.companion.model.OperationProgress) {
    Surface(
        Modifier.fillMaxSize().semantics { liveRegion = LiveRegionMode.Assertive },
        color = MaterialTheme.colorScheme.scrim.copy(alpha = .45f),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Card(Modifier.widthIn(min = 260.dp, max = 380.dp).padding(LayoutTokens.Space4)) {
                Column(
                    Modifier.padding(LayoutTokens.Space5).fillMaxWidth(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
                ) {
                    if (progress.total > 0) {
                        CircularProgressIndicator(progress = { progress.fraction })
                    } else {
                        CircularProgressIndicator()
                    }
                    Text(progress.label, style = MaterialTheme.typography.titleMedium)
                    if (progress.total > 0) {
                        Text(
                            "${progress.completed} / ${progress.total} bytes",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                }
            }
        }
    }
}
