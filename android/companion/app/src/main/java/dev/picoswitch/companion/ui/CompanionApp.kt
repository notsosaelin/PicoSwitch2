package dev.picoswitch.companion.ui

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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.semantics
import androidx.lifecycle.compose.collectAsStateWithLifecycle

private data class NavItem(val section: AppSection, val icon: ImageVector)
private val navItems = listOf(
    NavItem(AppSection.Home, Icons.Default.Home),
    NavItem(AppSection.Amiibo, Icons.Default.Contactless),
    NavItem(AppSection.Controller, Icons.Default.SportsEsports),
    NavItem(AppSection.Modes, Icons.Default.SettingsInputComponent),
    NavItem(AppSection.More, Icons.Default.MoreHoriz),
)

@Composable
fun CompanionApp(
    viewModel: CompanionViewModel,
    onConnectAdapter: () -> Unit,
    onImportAmiibo: () -> Unit,
    onPrepareController: () -> Unit,
    onPairControllerHost: () -> Unit,
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
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val useRail = maxWidth >= LayoutTokens.NavigationBreakpoint
            Scaffold(
                modifier = Modifier.windowInsetsPadding(WindowInsets.safeDrawing),
                snackbarHost = { SnackbarHost(snackbarHostState) },
                bottomBar = {
                    if (!useRail) NavigationBar {
                        navItems.forEach { item ->
                            NavigationBarItem(
                                selected = ui.section == item.section,
                                onClick = { viewModel.navigate(item.section) },
                                icon = { Icon(item.icon, null) }, label = { Text(item.section.label) },
                            )
                        }
                    }
                },
            ) { padding ->
                Row(Modifier.fillMaxSize().padding(padding)) {
                    if (useRail) {
                        NavigationRail(
                            header = {
                                Icon(Icons.Default.Gamepad, "PicoSwitch Companion", Modifier.padding(vertical = 20.dp))
                            },
                        ) {
                            Spacer(Modifier.weight(1f))
                            navItems.forEach { item ->
                                NavigationRailItem(
                                    selected = ui.section == item.section,
                                    onClick = { viewModel.navigate(item.section) },
                                    icon = { Icon(item.icon, null) }, label = { Text(item.section.label) },
                                )
                            }
                            Spacer(Modifier.weight(1f))
                        }
                    }
                    Box(Modifier.weight(1f).fillMaxHeight(), contentAlignment = Alignment.TopCenter) {
                        Column(
                            Modifier.fillMaxSize().widthIn(max = LayoutTokens.ContentMaxWidth)
                                .padding(horizontal = LayoutTokens.Space4),
                        ) {
                            ConnectionStrip(ui, viewModel, onConnectAdapter)
                            Box(Modifier.weight(1f).fillMaxWidth()) {
                                destinationState.SaveableStateProvider(ui.section.name) {
                                    when (ui.section) {
                                        AppSection.Home -> HomeScreen(ui, viewModel)
                                        AppSection.Amiibo -> AmiiboScreen(ui, viewModel, onImportAmiibo)
                                        AppSection.Controller -> ControllerScreen(ui, viewModel, onPrepareController, onPairControllerHost)
                                        AppSection.Modes -> ModesScreen(ui, viewModel)
                                        AppSection.More -> MoreScreen(ui, viewModel, onExportDiagnostics, theme)
                                    }
                                }
                            }
                        }
                    }
                }
                ui.operation?.let { OperationOverlay(it) }
            }
        }
    }
}

@Composable
private fun ConnectionStrip(ui: CompanionUiState, viewModel: CompanionViewModel, onConnect: () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth().padding(top = LayoutTokens.Space3),
        color = if (ui.connection.connected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.medium,
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = LayoutTokens.Space4, vertical = LayoutTokens.Space2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(if (ui.connection.connected) Icons.Default.BluetoothConnected else Icons.AutoMirrored.Filled.BluetoothSearching, null)
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f)) {
                Text(if (ui.connection.connected) ui.connection.deviceName ?: "PicoSwitch2" else phaseLabel(ui), style = MaterialTheme.typography.titleSmall)
                ui.connection.message?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
            }
            if (ui.connection.connected) {
                IconButton(onClick = viewModel::refresh, enabled = !ui.busy) { Icon(Icons.Default.Refresh, "Refresh") }
                TextButton(onClick = viewModel::disconnect, enabled = !ui.busy) { Text("Disconnect") }
            } else {
                Button(onClick = onConnect, enabled = !ui.busy) { Text(if (ui.connection.phase.name == "Reconnecting") "Reconnect" else "Find adapter") }
            }
        }
    }
}

private fun phaseLabel(ui: CompanionUiState) = when (ui.connection.phase) {
    dev.picoswitch.companion.model.ConnectionPhase.Scanning -> "Finding PicoSwitch2…"
    dev.picoswitch.companion.model.ConnectionPhase.Connecting -> "Connecting…"
    dev.picoswitch.companion.model.ConnectionPhase.Reconnecting -> "Adapter disconnected"
    dev.picoswitch.companion.model.ConnectionPhase.Failed -> "Connection failed"
    else -> "Adapter offline"
}

@Composable
private fun OperationOverlay(progress: dev.picoswitch.companion.model.OperationProgress) {
    Surface(Modifier.fillMaxSize().semantics { liveRegion = LiveRegionMode.Assertive }, color = MaterialTheme.colorScheme.scrim.copy(alpha = .45f)) {
        Box(contentAlignment = Alignment.Center) {
            Card(Modifier.widthIn(min = 260.dp, max = 380.dp).padding(LayoutTokens.Space4)) {
                Column(Modifier.padding(LayoutTokens.Space5), horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator(progress = { if (progress.total > 0) progress.fraction else 0f })
                    Spacer(Modifier.height(LayoutTokens.Space4))
                    Text(progress.label, style = MaterialTheme.typography.titleMedium)
                    if (progress.total > 0) Text("${progress.completed} / ${progress.total} bytes", style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}
