package dev.picoswitch.companion.ui

import android.content.Intent
import android.provider.Settings
import androidx.compose.material3.TextButton
import androidx.compose.ui.platform.LocalContext
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.BluetoothSearching
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.companion.ui.touch.TouchGamepadScreen

private data class NavItem(val section: AppSection, val icon: ImageVector)

/**
 * The application's five destinations, in the order they are used.
 *
 * Adapter first because it is the physical thing being managed; Settings last
 * because it is the least frequent. Diagnostics is intentionally absent -- it
 * is opened from Settings, not visited daily.
 */
private val navItems = listOf(
    NavItem(AppSection.Adapter, Icons.Default.Cable),
    NavItem(AppSection.Keyboard, Icons.Default.Keyboard),
    NavItem(AppSection.Amiibo, Icons.Default.Contactless),
    NavItem(AppSection.Controller, Icons.Default.SportsEsports),
    NavItem(AppSection.Settings, Icons.Default.Settings),
)

@Composable
fun CompanionApp(
    viewModel: CompanionViewModel,
    onConnectAdapter: () -> Unit,
    onPairAdapter: () -> Unit,
    onRepairAdapter: () -> Unit,
    onImportAmiibo: () -> Unit,
    onImportAmiiboArchive: () -> Unit,
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

    CompanionTheme(theme) {
        // A full-screen application MODE, above the scaffold rather than inside
        // it. The on-screen controller owns edge-to-edge presentation, hides the
        // navigation chrome and handles its own back behaviour; rendering it in
        // the content column would hand it the scaffold's insets and width limit.
        if (ui.touchGamepadActive) {
            TouchGamepadScreen(ui, viewModel, onPickTouchBackground, onRetryLink = onPrepareController)
            return@CompanionTheme
        }
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val useRail = maxWidth >= LayoutTokens.NavigationBreakpoint
            val windowHeight = maxHeight
            val fontScale = LocalDensity.current.fontScale
            ProvideShortWindow(windowHeight) {
                Scaffold(
                    modifier = Modifier.windowInsetsPadding(WindowInsets.safeDrawing),
                    snackbarHost = { SnackbarHost(snackbarHostState) },
                    bottomBar = {
                        if (!useRail) {
                            // Material gives the selected item extra width, so the
                            // unselected labels are what run out of room first --
                            // and an ellipsised destination name ("Keybo…") is a
                            // navigation the user has to guess at. Below the width
                            // one label actually needs, fall back to Material's
                            // icon-only bar, where only the selected item is
                            // labelled and the rest stay legible icons with content
                            // descriptions. The threshold scales with the font
                            // scale because that, not the display, is usually what
                            // makes the text too wide.
                            val labelWidth = LayoutTokens.NavLabelWidth * fontScale
                            val showLabels = (maxWidth / navItems.size) >= labelWidth
                            NavigationBar {
                                navItems.forEach { item ->
                                    NavigationBarItem(
                                        selected = ui.section == item.section,
                                        onClick = { viewModel.navigate(item.section) },
                                        icon = { Icon(item.icon, item.section.label) },
                                        label = {
                                            Text(
                                                item.section.label,
                                                maxLines = 1,
                                                overflow = TextOverflow.Ellipsis,
                                            )
                                        },
                                        alwaysShowLabel = showLabels,
                                    )
                                }
                            }
                        }
                    },
                ) { padding ->
                    Row(Modifier.fillMaxSize().padding(padding)) {
                        if (useRail) {
                            NavigationRail(
                                header = {
                                    Icon(
                                        Icons.Default.Gamepad,
                                        "PicoSwitch2 Companion",
                                        Modifier.padding(vertical = LayoutTokens.Space4),
                                    )
                                },
                            ) {
                                Spacer(Modifier.weight(1f))
                                navItems.forEach { item ->
                                    NavigationRailItem(
                                        selected = ui.section == item.section,
                                        onClick = { viewModel.navigate(item.section) },
                                        icon = { Icon(item.icon, null) },
                                        label = { Text(item.section.label, maxLines = 1) },
                                    )
                                }
                                Spacer(Modifier.weight(1f))
                            }
                        }
                        Box(Modifier.weight(1f).fillMaxHeight(), contentAlignment = Alignment.TopCenter) {
                            Column(
                                Modifier.fillMaxSize()
                                    .widthIn(max = LayoutTokens.ContentMaxWidth)
                                    .padding(horizontal = LayoutTokens.Space4),
                            ) {
                                // One connection indicator for the whole
                                // application. Screens read it rather than
                                // deriving their own idea of "connected",
                                // which is what left stale green badges behind
                                // after a session ended.
                                ConnectionStrip(ui, viewModel, onConnectAdapter, onPairAdapter, onRepairAdapter)
                                Box(Modifier.weight(1f).fillMaxWidth()) {
                                    destinationState.SaveableStateProvider(ui.section.name) {
                                        when (ui.section) {
                                            AppSection.Adapter -> AdapterScreen(ui, viewModel)
                                            AppSection.Keyboard -> KeyboardMouseScreen(ui, viewModel)
                                            AppSection.Amiibo -> AmiiboScreen(
                                                ui, viewModel, onImportAmiibo, onImportKeys = onImportAmiiboKeys,
                                                onScan = onScanAmiibo,
                                            )
                                            AppSection.Controller -> ControllerScreen(
                                                ui, viewModel, onPrepareController, onOpenTouchGamepad,
                                            )
                                            AppSection.Settings -> SettingsScreen(
                                                ui, viewModel, onImportAmiiboKeys, theme,
                                            )
                                        }
                                    }
                                    OverlayHost(
                                        ui, viewModel, onExportDiagnostics,
                                        onImportAmiiboArchive, onExportAmiiboArchive,
                                        onImportAmiiboKeys,
                                    )
                                }
                            }
                        }
                    }
                    ui.operation?.let { OperationOverlay(it) }
                }
            }
        }
    }
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
    onImportAmiiboArchive: () -> Unit,
    onExportAmiiboArchive: () -> Unit,
    onImportAmiiboKeys: () -> Unit,
) {
    AnimatedVisibility(
        visible = ui.overlay != AppOverlay.None,
        enter = slideInHorizontally(initialOffsetX = { it / 3 }) + fadeIn(),
        exit = slideOutHorizontally(targetOffsetX = { it / 3 }) + fadeOut(),
    ) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
            when (ui.overlay) {
                AppOverlay.Diagnostics -> DiagnosticsScreen(ui, viewModel, onExportDiagnostics)
                AppOverlay.AmiiboSettings -> AmiiboSettingsScreen(
                    ui, viewModel, onImportAmiiboArchive, onExportAmiiboArchive, onImportAmiiboKeys,
                )
                AppOverlay.None -> Unit
            }
        }
    }
}

/**
 * The application-wide connection state.
 *
 * Present on every page rather than only on the home screen, because every page
 * has controls whose availability depends on it and because a disconnect that
 * happens while looking at another page must be visible where it happens.
 */
@Composable
private fun ConnectionStrip(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onConnect: () -> Unit,
    onPairAdapter: () -> Unit,
    onRepairAdapter: () -> Unit,
) {
    val connected = ui.connection.connected
    val busyPhase = ui.connection.phase == ConnectionPhase.Associating ||
        ui.connection.phase == ConnectionPhase.Bonding ||
        ui.connection.phase == ConnectionPhase.Scanning ||
        ui.connection.phase == ConnectionPhase.Connecting
    Surface(
        modifier = Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2),
        color = if (connected) MaterialTheme.colorScheme.primaryContainer
        else MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.medium,
    ) {
        Column {
            // Progress is a bar rather than a spinner in a corner so a slow
            // connect is legible without blocking the page behind it.
            if (busyPhase) LinearProgressIndicator(Modifier.fillMaxWidth())
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = LayoutTokens.TouchHeight)
                    .padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space1)
                    .semantics { liveRegion = LiveRegionMode.Polite },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    if (connected) Icons.Default.BluetoothConnected
                    else Icons.AutoMirrored.Filled.BluetoothSearching,
                    null,
                    Modifier.size(LayoutTokens.IconSize),
                )
                Spacer(Modifier.width(LayoutTokens.Space2))
                Column(Modifier.weight(1f)) {
                    Text(
                        if (connected) ui.connection.deviceName ?: "PicoSwitch2" else phaseLabel(ui),
                        style = MaterialTheme.typography.titleSmall,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    ui.connection.message?.let {
                        Text(
                            it,
                            style = MaterialTheme.typography.labelSmall,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                if (connected) {
                    if (ui.kbm.dirty) {
                        StatusChip("Unsaved", tone = ChipTone.Attention)
                        Spacer(Modifier.width(LayoutTokens.Space2))
                    }
                    IconButton(onClick = viewModel::refresh, enabled = !ui.busy) {
                        Icon(Icons.Default.Refresh, "Refresh adapter")
                    }
                    IconButton(onClick = viewModel::disconnect, enabled = !ui.busy) {
                        Icon(Icons.Default.LinkOff, "Disconnect")
                    }
                } else {
                    val actionEnabled = !ui.busy && !ui.relationshipStatus.attemptActive
                    if (ui.adapterRelationship != null && ui.connection.phase != ConnectionPhase.RepairRequired) {
                        IconButton(onClick = onPairAdapter, enabled = actionEnabled) {
                            Icon(Icons.Default.AddLink, "Pair another adapter")
                        }
                    }
                    if (ui.connection.phase == ConnectionPhase.RepairRequired) {
                        // Forgetting the Android bond is the one repair step
                        // this app cannot perform: BluetoothDevice.removeBond()
                        // is a @SystemApi gated on BLUETOOTH_PRIVILEGED. Offer
                        // the shortest supported route to it instead of leaving
                        // the user to find it.
                        val context = LocalContext.current
                        TextButton(
                            onClick = {
                                runCatching {
                                    context.startActivity(
                                        Intent(Settings.ACTION_BLUETOOTH_SETTINGS)
                                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                                    )
                                }
                            },
                            enabled = actionEnabled,
                        ) { Text("Bluetooth settings") }
                        Spacer(Modifier.width(LayoutTokens.Space2))
                        Button(onClick = onRepairAdapter, enabled = actionEnabled) { Text("Repair pairing") }
                    } else {
                        Button(onClick = onConnect, enabled = actionEnabled) {
                            Text(if (ui.adapterRelationship == null) "Pair Adapter" else "Reconnect")
                        }
                    }
                }
            }
        }
    }
}

private fun phaseLabel(ui: CompanionUiState) = when (ui.connection.phase) {
    ConnectionPhase.Associating -> "Pairing adapter…"
    ConnectionPhase.Bonding -> "Securing Android pairing…"
    ConnectionPhase.Scanning -> "Finding PicoSwitch2…"
    ConnectionPhase.Connecting -> "Connecting…"
    ConnectionPhase.Reconnecting -> "Adapter disconnected"
    ConnectionPhase.RepairRequired -> "Pairing repair needed"
    ConnectionPhase.Failed -> "Connection failed"
    else -> "Adapter offline"
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
