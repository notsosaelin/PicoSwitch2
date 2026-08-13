@file:OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)

package dev.picoswitch.companion.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import dev.picoswitch.companion.BuildConfig
import dev.picoswitch.companion.controller.BridgePhase
import dev.picoswitch.companion.controller.ControllerFaceLayout
import dev.picoswitch.companion.data.ColorTarget
import dev.picoswitch.companion.model.*

@Composable
fun HomeScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    ScreenColumn("Hardware at a glance", "Adapter, controller, and active virtual figure state") {
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val wide = maxWidth >= LayoutTokens.TwoPaneBreakpoint
            if (wide) {
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                        AdapterHero(ui, viewModel)
                        ControllerCard(ui)
                    }
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                        AmiiboStatusCard(ui, viewModel)
                        SafetyCard(ui)
                    }
                }
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                    AdapterHero(ui, viewModel); ControllerCard(ui); AmiiboStatusCard(ui, viewModel); SafetyCard(ui)
                }
            }
        }
    }
}

@Composable
private fun AdapterHero(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val snapshot = ui.snapshot
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Surface(shape = CircleShape, color = MaterialTheme.colorScheme.primaryContainer, modifier = Modifier.size(56.dp)) {
                Box(contentAlignment = Alignment.Center) { Icon(Icons.Default.Gamepad, null) }
            }
            Spacer(Modifier.width(LayoutTokens.Space4))
            Column(Modifier.weight(1f)) {
                Text(snapshot.personality.current.title, style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
                Text(snapshot.firmware.version.ifBlank { "Connect to read firmware" }, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            StatusPill(if (ui.connection.connected) "Online" else "Offline", ui.connection.connected)
        }
        HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space4))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            FilledTonalButton(onClick = viewModel::wake, enabled = ui.connection.connected && !ui.busy && ui.snapshot.capabilities.wake != CapabilityState.Unsupported) {
                Icon(Icons.Default.PowerSettingsNew, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Wake console")
            }
            OutlinedButton(onClick = { viewModel.navigate(AppSection.Modes) }) { Text("Change mode") }
        }
    }
}

@Composable
private fun ControllerCard(ui: CompanionUiState) {
    val controller = ui.snapshot.controller
    HardwareCard {
        SectionHeading(Icons.Default.SportsEsports, "Input controller")
        Spacer(Modifier.height(LayoutTokens.Space3))
        Text(controller.name, style = MaterialTheme.typography.titleLarge)
        if (controller.attached) {
            Text("VID %04X · PID %04X".format(controller.vid, controller.pid), style = MaterialTheme.typography.bodySmall)
            if (controller.batteryValid) {
                Spacer(Modifier.height(LayoutTokens.Space2))
                LinearProgressIndicator({ controller.batteryPercent / 100f }, Modifier.fillMaxWidth())
                Text("${controller.batteryPercent}%${if (controller.charging) " · charging" else ""}", style = MaterialTheme.typography.labelMedium)
            }
        } else Text("No Bluetooth input is currently reported by the adapter.", color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun AmiiboStatusCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val a = ui.snapshot.amiibo
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            SectionHeading(Icons.Default.Contactless, "Virtual Amiibo", Modifier.weight(1f))
            if (a.dirty) StatusPill("Needs sync", false) else if (a.loaded || a.v3Loaded) StatusPill(if (a.presented) "Presented" else "Loaded", true)
        }
        Spacer(Modifier.height(LayoutTokens.Space3))
        Text(if (a.loaded || a.v3Loaded) a.figureId.ifBlank { "Unknown figure" } else "No Amiibo loaded", style = MaterialTheme.typography.titleLarge)
        if (a.loaded || a.v3Loaded) Text("UID ${a.uid} · ${a.size} bytes · generation ${a.generation}", style = MaterialTheme.typography.bodySmall)
        Spacer(Modifier.height(LayoutTokens.Space4))
        Button(onClick = { viewModel.navigate(AppSection.Amiibo) }, modifier = Modifier.fillMaxWidth()) { Text("Open Amiibo library") }
    }
}

@Composable
private fun SafetyCard(ui: CompanionUiState) {
    if (ui.snapshot.managementEnabled != true && ui.connection.connected) return
    HardwareCard(container = MaterialTheme.colorScheme.secondaryContainer) {
        SectionHeading(Icons.Default.Security, "Connection note")
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text("In-band management is RAM-only and currently unauthenticated in firmware. Use it only in a trusted space; it returns to off after reboot.")
    }
}

@Composable
fun AmiiboScreen(ui: CompanionUiState, viewModel: CompanionViewModel, onImport: () -> Unit) {
    val selected = ui.library.firstOrNull { it.id == ui.selectedAmiiboId }
    val adapterLoaded = ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded
    ScreenFrame("Amiibo library", "Private local backups with verified adapter transfers") {
        FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Button(onClick = onImport) { Icon(Icons.Default.Add, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Import backup") }
            FilledTonalButton(onClick = viewModel::syncSelectedAmiibo, enabled = ui.connection.connected && (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded) && !ui.busy) {
                Icon(Icons.Default.Sync, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Sync adapter")
            }
        }
        ui.libraryWarnings.firstOrNull()?.let {
            Spacer(Modifier.height(LayoutTokens.Space2))
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.height(LayoutTokens.Space4))
        BoxWithConstraints(Modifier.fillMaxSize()) {
            if (ui.library.isEmpty()) {
                Column(Modifier.fillMaxSize()) {
                    if (adapterLoaded) {
                        AdapterOnlyAmiiboCard(ui, viewModel)
                        Spacer(Modifier.height(LayoutTokens.Space3))
                    }
                    EmptyState(
                        Icons.Default.Contactless,
                        if (adapterLoaded) "No local backup yet" else "Your library is empty",
                        if (adapterLoaded) "Download the adapter's active Amiibo before the console changes it again."
                        else "Import your own 540, 572, or 2048-byte Amiibo backup.",
                        Modifier.fillMaxWidth().weight(1f),
                    )
                }
            } else if (maxWidth >= 600.dp) {
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                    AmiiboGrid(ui, viewModel, Modifier.weight(1f).fillMaxHeight())
                    AmiiboDetail(selected, ui, viewModel, Modifier.width(LayoutTokens.DetailWidth).fillMaxHeight())
                }
            } else {
                val detailHeight = (maxHeight * 0.48f).coerceIn(120.dp, 280.dp)
                Column {
                    AmiiboDetail(selected, ui, viewModel, Modifier.fillMaxWidth().height(detailHeight))
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    AmiiboGrid(ui, viewModel, Modifier.weight(1f).fillMaxWidth())
                }
            }
        }
    }
}

@Composable
private fun AdapterOnlyAmiiboCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var clearOpen by rememberSaveable { mutableStateOf(false) }
    val status = ui.snapshot.amiibo
    val pro2 = ui.snapshot.personality.current == Personality.Pro2
    val enabled = ui.connection.connected && !ui.busy &&
        ui.snapshot.capabilities.amiibo != CapabilityState.Unsupported && pro2
    if (clearOpen) AlertDialog(
        onDismissRequest = { clearOpen = false }, title = { Text("Clear adapter Amiibo?") },
        text = { Text("This active Amiibo has no local backup in the app. Download it first if you may need it later.") },
        confirmButton = { TextButton(onClick = { viewModel.clearAdapterAmiibo(); clearOpen = false }) { Text("Clear adapter") } },
        dismissButton = { TextButton(onClick = { clearOpen = false }) { Text("Cancel") } },
    )
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            SectionHeading(Icons.Default.Contactless, "Adapter Amiibo", Modifier.weight(1f))
            StatusPill(if (status.presented) "Presented" else "Loaded", true)
        }
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text(status.figureId.ifBlank { "Unknown figure" }, style = MaterialTheme.typography.titleMedium)
        Text("UID ${status.uid.ifBlank { "unknown" }} · ${status.size} bytes · generation ${status.generation}", style = MaterialTheme.typography.bodySmall)
        if (!pro2) {
            Spacer(Modifier.height(LayoutTokens.Space2))
            Text("Switch to Pro Controller 2 mode to manage virtual Amiibo.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.height(LayoutTokens.Space3))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            FilledTonalButton(onClick = viewModel::syncSelectedAmiibo, enabled = enabled) { Text("Download to phone") }
            OutlinedButton(onClick = { viewModel.setPresented(!status.presented) }, enabled = enabled) {
                Text(if (status.presented) "Eject" else "Present")
            }
            TextButton(onClick = { clearOpen = true }, enabled = enabled && !status.dirty) { Text("Clear adapter") }
        }
        if (status.dirty) Text("Console-written data is unsynced. Download it before clearing.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun AmiiboGrid(ui: CompanionUiState, viewModel: CompanionViewModel, modifier: Modifier) {
    if (ui.library.isEmpty()) {
        EmptyState(Icons.Default.Contactless, "Your library is empty", "Import your own 540, 572, or 2048-byte Amiibo backup.", modifier)
        return
    }
    LazyVerticalGrid(
        columns = GridCells.Adaptive(168.dp), modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
        contentPadding = PaddingValues(bottom = LayoutTokens.Space5),
    ) {
        items(ui.library, key = { it.id }) { item ->
            val selected = item.id == ui.selectedAmiiboId
            Card(
                modifier = Modifier.fillMaxWidth().clickable { viewModel.selectAmiibo(item.id) },
                colors = CardDefaults.cardColors(containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant),
                border = if (selected) CardDefaults.outlinedCardBorder() else null,
            ) {
                Column(Modifier.padding(LayoutTokens.Space4)) {
                    Icon(Icons.Default.Contactless, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(36.dp))
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    Text(item.displayName, maxLines = 2, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.height(LayoutTokens.Space1))
                    Text(item.figureId.ifBlank { "Unknown figure" }, maxLines = 1, style = MaterialTheme.typography.bodySmall)
                    Text("${item.size} bytes", style = MaterialTheme.typography.labelSmall)
                }
            }
        }
    }
}

@Composable
private fun AmiiboDetail(item: AmiiboLibraryItem?, ui: CompanionUiState, viewModel: CompanionViewModel, modifier: Modifier) {
    var renameOpen by rememberSaveable(item?.id) { mutableStateOf(false) }
    var deleteOpen by rememberSaveable(item?.id) { mutableStateOf(false) }
    var clearOpen by rememberSaveable { mutableStateOf(false) }
    var name by rememberSaveable(item?.id) { mutableStateOf(item?.displayName.orEmpty()) }
    if (renameOpen && item != null) AlertDialog(
        onDismissRequest = { renameOpen = false },
        title = { Text("Rename local backup") },
        text = { OutlinedTextField(name, { name = it.take(120) }, label = { Text("Name") }, singleLine = true) },
        confirmButton = { TextButton(onClick = { viewModel.renameSelectedAmiibo(name); renameOpen = false }) { Text("Rename") } },
        dismissButton = { TextButton(onClick = { renameOpen = false }) { Text("Cancel") } },
    )
    if (deleteOpen && item != null) AlertDialog(
        onDismissRequest = { deleteOpen = false }, title = { Text("Delete local backup?") },
        text = { Text("This removes ${item.displayName} from this phone only. It cannot be undone and does not clear the adapter.") },
        confirmButton = { TextButton(onClick = { viewModel.deleteSelectedAmiibo(); deleteOpen = false }) { Text("Delete") } },
        dismissButton = { TextButton(onClick = { deleteOpen = false }) { Text("Cancel") } },
    )
    if (clearOpen) AlertDialog(
        onDismissRequest = { clearOpen = false }, title = { Text("Clear adapter Amiibo?") },
        text = { Text("This removes the stored virtual Amiibo from the adapter. Your private phone backup remains available.") },
        confirmButton = { TextButton(onClick = { viewModel.clearAdapterAmiibo(); clearOpen = false }) { Text("Clear adapter") } },
        dismissButton = { TextButton(onClick = { clearOpen = false }) { Text("Cancel") } },
    )
    Card(modifier) {
        if (item == null) {
            EmptyState(Icons.Default.TouchApp, "Select an Amiibo", "Choose a local backup to inspect or load.", Modifier.fillMaxSize())
        } else {
            Column(Modifier.padding(LayoutTokens.Space4).verticalScroll(rememberScrollState())) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(item.displayName, style = MaterialTheme.typography.titleLarge)
                        Text("Figure ${item.figureId}", style = MaterialTheme.typography.bodySmall)
                    }
                    IconButton(onClick = { renameOpen = true }) { Icon(Icons.Default.Edit, "Rename local copy") }
                    IconButton(onClick = { deleteOpen = true }) { Icon(Icons.Default.DeleteOutline, "Delete local copy") }
                }
                Spacer(Modifier.height(LayoutTokens.Space3))
                MetadataLine("UID", item.uid)
                MetadataLine("CRC32", item.crc32)
                MetadataLine("Format", if (item.size == 2048) "Figure v3 · 2 KB" else "NTAG215 · ${item.size} B")
                HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space3))
                Button(onClick = viewModel::loadSelectedAmiibo, enabled = ui.connection.connected && !ui.busy, modifier = Modifier.fillMaxWidth()) { Text("Load onto adapter") }
                Spacer(Modifier.height(LayoutTokens.Space2))
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                    FilledTonalButton(onClick = { viewModel.setPresented(!ui.snapshot.amiibo.presented) }, enabled = ui.connection.connected && (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded), modifier = Modifier.weight(1f)) {
                        Text(if (ui.snapshot.amiibo.presented) "Eject" else "Present")
                    }
                    OutlinedButton(onClick = viewModel::syncSelectedAmiibo, enabled = ui.connection.connected && (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded), modifier = Modifier.weight(1f)) { Text("Sync") }
                }
                if (ui.snapshot.amiibo.hasSave2) {
                    Spacer(Modifier.height(LayoutTokens.Space2))
                    OutlinedButton(onClick = { viewModel.selectCopy(!ui.snapshot.amiibo.usingSave2) }, Modifier.fillMaxWidth()) {
                        Text(if (ui.snapshot.amiibo.usingSave2) "Use clean copy" else "Use console-written copy")
                    }
                }
                if (ui.snapshot.amiibo.dirty) {
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    Surface(shape = MaterialTheme.shapes.small, color = MaterialTheme.colorScheme.errorContainer) {
                        Row(Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2), verticalAlignment = Alignment.CenterVertically) {
                            Icon(Icons.Default.Warning, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Console changes are not synced")
                        }
                    }
                }
                if ((ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded) && !ui.snapshot.amiibo.dirty) {
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    TextButton(onClick = { clearOpen = true }, Modifier.fillMaxWidth()) { Text("Clear adapter Amiibo") }
                }
            }
        }
    }
}

@Composable
fun ControllerScreen(ui: CompanionUiState, viewModel: CompanionViewModel, onPrepare: () -> Unit, onPairHost: () -> Unit) {
    ScreenColumn("Android controller", "Pass this handheld’s built-in controls through PicoSwitch2") {
        HardwareCard(container = MaterialTheme.colorScheme.secondaryContainer) {
            Text("Foreground only", style = MaterialTheme.typography.titleMedium)
            Text("Android’s public HID Device profile works without root or Shizuku, but input streams only while this app is visible. Bluetooth gamepads attached to Android may disconnect while bridge mode is active.")
        }
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val wide = maxWidth >= LayoutTokens.TwoPaneBreakpoint
            val source: @Composable () -> Unit = { InputSourceCard(ui, viewModel) }
            val layout: @Composable () -> Unit = { ControllerLayoutCard(ui, viewModel) }
            val bridge: @Composable () -> Unit = { BridgeCard(ui, viewModel, onPrepare, onPairHost) }
            if (wide) Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) { source(); layout() }
                Box(Modifier.weight(1f)) { bridge() }
            } else Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) { source(); layout(); bridge() }
        }
        InputDiagnostics(ui.controllerState)
    }
}

@Composable
private fun ControllerLayoutCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    HardwareCard {
        SectionHeading(Icons.Default.SwapHoriz, "Controller layout")
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text(
            "Android reports face-button positions, which may not match the letters printed on Nintendo-style handhelds.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        ControllerFaceLayout.entries.forEach { layout ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = LayoutTokens.TouchHeight)
                    .selectable(
                        selected = ui.requestedFaceLayout == layout,
                        enabled = ui.selectedSourceDescriptor != null,
                        role = Role.RadioButton,
                        onClick = { viewModel.setControllerFaceLayout(layout) },
                    ),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(selected = ui.requestedFaceLayout == layout, onClick = null, enabled = ui.selectedSourceDescriptor != null)
                Spacer(Modifier.width(LayoutTokens.Space2))
                Column(Modifier.weight(1f)) {
                    Text(layout.title)
                    Text(layout.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        if (ui.requestedFaceLayout == ControllerFaceLayout.Auto) {
            Text(
                "Auto resolved to ${ui.resolvedFaceLayout.layout.title}: ${ui.resolvedFaceLayout.reason}.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

@Composable
private fun InputSourceCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            SectionHeading(Icons.Default.Gamepad, "Built-in input", Modifier.weight(1f))
            IconButton(onClick = viewModel::refreshSources) { Icon(Icons.Default.Refresh, "Refresh inputs") }
        }
        if (ui.sourceDevices.isEmpty()) Text("No Android gamepad/joystick input device is visible to the app.")
        ui.sourceDevices.forEach { source ->
            val selected = source.descriptor == ui.selectedSourceDescriptor
            Surface(
                Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2).clickable { viewModel.selectSource(source.descriptor) },
                shape = MaterialTheme.shapes.medium,
                color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
            ) {
                Row(Modifier.padding(LayoutTokens.Space3), verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected, { viewModel.selectSource(source.descriptor) })
                    Column(Modifier.weight(1f)) {
                        Text(source.name, style = MaterialTheme.typography.titleSmall)
                        Text("VID %04X · PID %04X".format(source.vendorId, source.productId), style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
    }
}

@Composable
private fun BridgeCard(ui: CompanionUiState, viewModel: CompanionViewModel, onPrepare: () -> Unit, onPairHost: () -> Unit) {
    val hosts = remember(ui.bridge.phase, ui.connection.phase) { viewModel.pairedControllerHosts() }
    HardwareCard {
        SectionHeading(Icons.Default.BluetoothAudio, "Controller link")
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text(ui.bridge.phase.name, style = MaterialTheme.typography.titleLarge)
        ui.bridge.message?.let { Text(it, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        Spacer(Modifier.height(LayoutTokens.Space3))
        when (ui.bridge.phase) {
            BridgePhase.Idle, BridgePhase.Unsupported, BridgePhase.Failed ->
                Button(onClick = onPrepare, enabled = ui.selectedSourceDescriptor != null, modifier = Modifier.fillMaxWidth()) { Text("Prepare controller bridge") }
            BridgePhase.Playing ->
                Button(onClick = viewModel::stopControllerBridge, modifier = Modifier.fillMaxWidth(), colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)) { Text("Stop playing") }
            else -> LinearProgressIndicator(Modifier.fillMaxWidth())
        }
        if (ui.bridge.phase == BridgePhase.Ready) {
            hosts.forEach { host ->
                FilledTonalButton(onClick = { viewModel.connectControllerHost(host) }, Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2)) {
                    Text("Connect ${viewModel.controllerHostLabel(host)}")
                }
            }
            OutlinedButton(onClick = onPairHost, Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2)) { Text("Pair a PicoSwitch2 host") }
            Text("Open the adapter’s physical pairing window first. Android will show its own required chooser and bond prompt.", style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun InputDiagnostics(state: dev.picoswitch.companion.controller.ControllerState) {
    HardwareCard {
        SectionHeading(Icons.Default.MonitorHeart, "Live input")
        Spacer(Modifier.height(LayoutTokens.Space3))
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
            AxisMeter("LX", state.leftX, Modifier.weight(1f)); AxisMeter("LY", state.leftY, Modifier.weight(1f))
            AxisMeter("RX", state.rightX, Modifier.weight(1f)); AxisMeter("RY", state.rightY, Modifier.weight(1f))
        }
        Spacer(Modifier.height(LayoutTokens.Space3))
        Text("L2 ${state.leftTrigger}  ·  R2 ${state.rightTrigger}  ·  Hat ${dev.picoswitch.companion.controller.ControllerReportEncoder.hat(state)}", style = MaterialTheme.typography.bodyMedium)
        Text(if (state.buttons.isEmpty()) "No buttons held" else state.buttons.joinToString(" · ") { it.name }, style = MaterialTheme.typography.bodySmall)
        Text("Motion and rumble are not part of the v1 bridge protocol.", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun AxisMeter(label: String, value: Int, modifier: Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        LinearProgressIndicator({ value / 255f }, Modifier.fillMaxWidth())
        Text("$label $value", style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
fun ModesScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    ScreenColumn("Adapter & modes", "Console-facing personality and controller identity colors") {
        HardwareCard {
            SectionHeading(Icons.Default.Cable, "Output personality")
            Spacer(Modifier.height(LayoutTokens.Space2))
            Text("Switching uses the firmware’s existing USB disconnect/reconnect path. Controller input and audio may pause briefly.")
            FlowRow(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), modifier = Modifier.padding(top = LayoutTokens.Space3)) {
                val choices = ui.snapshot.personality.available.ifEmpty { listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight) }
                choices.forEach { mode ->
                    FilterChip(
                        selected = mode == ui.snapshot.personality.current,
                        onClick = { viewModel.switchPersonality(mode) }, enabled = ui.connection.connected && ui.snapshot.capabilities.personality != CapabilityState.Unsupported,
                        label = { Text(mode.title) }, leadingIcon = if (mode == ui.snapshot.personality.current) ({ Icon(Icons.Default.Check, null) }) else null,
                    )
                }
            }
        }
        ColorEditor("Body / lightbar", ColorTarget.Body, ui.snapshot.config.bodyColor, ui.connection.connected, viewModel)
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            if (maxWidth >= LayoutTokens.TwoPaneBreakpoint) Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                Box(Modifier.weight(1f)) { ColorEditor("Joy-Con 2 left accent", ColorTarget.LeftAccent, ui.snapshot.config.leftAccent, ui.connection.connected, viewModel) }
                Box(Modifier.weight(1f)) { ColorEditor("Joy-Con 2 right accent", ColorTarget.RightAccent, ui.snapshot.config.rightAccent, ui.connection.connected, viewModel) }
            } else Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                ColorEditor("Joy-Con 2 left accent", ColorTarget.LeftAccent, ui.snapshot.config.leftAccent, ui.connection.connected, viewModel)
                ColorEditor("Joy-Con 2 right accent", ColorTarget.RightAccent, ui.snapshot.config.rightAccent, ui.connection.connected, viewModel)
            }
        }
        Text("Firmware has no controller mapping command. Button remapping intentionally remains in the Switch’s persistent controller settings.", style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun ColorEditor(title: String, target: ColorTarget, initial: RgbColor, enabled: Boolean, viewModel: CompanionViewModel) {
    var red by rememberSaveable(initial) { mutableFloatStateOf(initial.red.toFloat()) }
    var green by rememberSaveable(initial) { mutableFloatStateOf(initial.green.toFloat()) }
    var blue by rememberSaveable(initial) { mutableFloatStateOf(initial.blue.toFloat()) }
    val current = RgbColor(red.toInt(), green.toInt(), blue.toInt())
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(36.dp).clip(CircleShape).background(Color(current.argb())))
            Spacer(Modifier.width(LayoutTokens.Space3)); Text(title, Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
            Text("#%02X%02X%02X".format(current.red, current.green, current.blue), style = MaterialTheme.typography.labelMedium)
        }
        ColorSlider("R", red, { red = it }, Color(0xFFFF6B6B)); ColorSlider("G", green, { green = it }, Color(0xFF5DDA84)); ColorSlider("B", blue, { blue = it }, Color(0xFF69A7FF))
        Button(onClick = { viewModel.saveColor(target, current) }, enabled = enabled, modifier = Modifier.fillMaxWidth()) { Text("Save color") }
    }
}

@Composable
private fun ColorSlider(label: String, value: Float, onValue: (Float) -> Unit, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(24.dp), fontWeight = FontWeight.Bold, color = color)
        Slider(value, onValue, valueRange = 0f..255f, modifier = Modifier.weight(1f).semantics { contentDescription = "$label color value" })
        Text(value.toInt().toString(), Modifier.width(38.dp), style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
fun MoreScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onExportDiagnostics: () -> Unit,
    theme: ThemeSelection,
) {
    ScreenColumn("Settings & information", "User-safe controls and protocol details") {
        ThemeSettingsCard(theme, viewModel)
        HardwareCard {
            SectionHeading(Icons.Default.Info, "About this connection")
            Spacer(Modifier.height(LayoutTokens.Space3))
            MetadataLine("App", BuildConfig.VERSION_NAME)
            MetadataLine("Firmware", ui.snapshot.firmware.version.ifBlank { "Not connected" })
            MetadataLine("Protocol", "BLE GATT · newline JSON v1")
            MetadataLine("Adapter", ui.connection.address ?: "Not connected")
        }
        HardwareCard {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Wireless management", style = MaterialTheme.typography.titleMedium)
                    Text("RAM-only firmware switch; defaults off after reboot", style = MaterialTheme.typography.bodySmall)
                }
                Switch(
                    checked = ui.snapshot.managementEnabled == true,
                    onCheckedChange = viewModel::setManagement,
                    enabled = ui.connection.connected && ui.snapshot.capabilities.managementGate == CapabilityState.Available,
                )
            }
        }
        HardwareCard {
            SectionHeading(Icons.Default.Link, "Phone bonds")
            Spacer(Modifier.height(LayoutTokens.Space2))
            when {
                ui.snapshot.capabilities.bonds == CapabilityState.Unsupported -> Text("This firmware does not expose LE management bond controls.")
                ui.snapshot.capabilities.bonds == CapabilityState.Available && ui.snapshot.bonds.isEmpty() -> Text("No LE management bonds reported. Classic controller bonds are intentionally managed by the adapter’s physical wipe gesture.")
            }
            ui.snapshot.bonds.forEach { bond ->
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    MetadataLine("#${bond.index}", bond.name ?: bond.address)
                    IconButton(onClick = { viewModel.removeBond(bond.index) }, enabled = !ui.busy) {
                        Icon(Icons.Default.LinkOff, "Remove management bond ${bond.index}")
                    }
                }
            }
            if (ui.snapshot.capabilities.bonds == CapabilityState.Unknown && ui.connection.connected) {
                Text("Bond state is unknown. Reconnect and retry; a large list may exceed the firmware's 511-byte reply limit.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            } else if (ui.snapshot.capabilities.bonds == CapabilityState.Available && ui.snapshot.bonds.isNotEmpty()) {
                Text("Firmware does not report a total count, so unusually large bond lists may be incomplete until pagination is added.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        HardwareCard {
            SectionHeading(Icons.Default.DeveloperMode, "Developer / diagnostics")
            Spacer(Modifier.height(LayoutTokens.Space2))
            MetadataLine("Bluetooth", "available ${ui.platform.bluetoothAvailable} · enabled ${ui.platform.bluetoothEnabled}")
            MetadataLine("Permissions", "scan ${ui.platform.scanPermission} · connect ${ui.platform.connectPermission}")
            MetadataLine("Companion manager", ui.platform.companionDeviceManager.toString())
            MetadataLine("Management GATT", ui.connection.phase.name)
            MetadataLine("HID profile", "${ui.bridge.phase.name} · registered ${ui.bridge.registered}")
            MetadataLine("Descriptor", "${dev.picoswitch.companion.controller.AndroidControllerDescriptor.bytes.size} bytes · report ID 1")
            MetadataLine("Saved host", if (viewModel.pairedControllerHosts().isEmpty()) "No" else "Yes")
            MetadataLine("Reports", "${ui.bridge.reportCount} · last ${if (ui.bridge.lastReportAtMillis == 0L) "never" else ui.bridge.lastReportAtMillis}")
            MetadataLine("Last command", ui.diagnosticSummary.lastCommand)
            MetadataLine("Last result", ui.diagnosticSummary.lastResult)
            MetadataLine("Last error", ui.diagnosticSummary.lastError)
            MetadataLine("Identity refresh", if (ui.identityRefreshPending) "Required" else "None pending")
            if (ui.identityRefreshPending) TextButton(onClick = viewModel::clearIdentityRefreshPending) { Text("Mark identity refresh complete") }
            Button(onClick = onExportDiagnostics, Modifier.fillMaxWidth()) {
                Icon(Icons.Default.Share, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Share privacy-safe diagnostics")
            }
            Text("Exports event types and app state only. Raw Amiibo bytes, JSON replies, keys, and Bluetooth addresses are excluded.", style = MaterialTheme.typography.bodySmall)
        }
        HardwareCard(container = MaterialTheme.colorScheme.errorContainer) {
            SectionHeading(Icons.Default.GppMaybe, "Security & validation")
            Spacer(Modifier.height(LayoutTokens.Space2))
            Text("Current firmware does not enforce authenticated management writes. Real adapter coexistence, wake while connected, OEM HID registration, and end-to-end controller input still require hardware validation.")
        }
    }
}

@Composable
private fun ThemeSettingsCard(selection: ThemeSelection, viewModel: CompanionViewModel) {
    HardwareCard {
        SectionHeading(Icons.Default.Palette, "Appearance")
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text(
            "Choose how the companion looks on this device. This is local app styling and does not change the adapter's controller identity colors.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(LayoutTokens.Space3))
        Text("Theme", style = MaterialTheme.typography.titleSmall)
        ThemeMode.entries.forEach { mode ->
            ThemeChoiceRow(
                selected = selection.mode == mode,
                title = mode.title,
                description = mode.description,
                onClick = { viewModel.setThemeMode(mode) },
            )
        }
        HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space3))
        Text("Accent palette", style = MaterialTheme.typography.titleSmall)
        Text(
            "Optional Joy-Con-inspired UI colors; these are visual references, not hardware identity claims.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        AccentPalette.entries.forEach { palette ->
            PaletteChoiceRow(
                selected = selection.palette == palette,
                palette = palette,
                onClick = { viewModel.setAccentPalette(palette) },
            )
        }
    }
}

@Composable
private fun ThemeChoiceRow(selected: Boolean, title: String, description: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = LayoutTokens.TouchHeight)
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(horizontal = LayoutTokens.Space1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Spacer(Modifier.width(LayoutTokens.Space2))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun PaletteChoiceRow(selected: Boolean, palette: AccentPalette, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = LayoutTokens.TouchHeight)
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(horizontal = LayoutTokens.Space1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Spacer(Modifier.width(LayoutTokens.Space2))
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space1)) {
            PaletteSwatch(palette.leftSwatch, "Left accent sample")
            PaletteSwatch(palette.rightSwatch, "Right accent sample")
        }
        Spacer(Modifier.width(LayoutTokens.Space3))
        Column(Modifier.weight(1f)) {
            Text(palette.title, style = MaterialTheme.typography.bodyLarge)
            Text(palette.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun PaletteSwatch(color: Color, description: String) {
    Box(
        Modifier
            .size(20.dp)
            .clip(CircleShape)
            .background(color)
            .semantics { contentDescription = description },
    )
}

@Composable
private fun ScreenColumn(title: String, subtitle: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = LayoutTokens.Space5), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
        ScreenTitle(title, subtitle); content(); Spacer(Modifier.height(LayoutTokens.Space5))
    }
}

@Composable
private fun ScreenFrame(title: String, subtitle: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize().padding(vertical = LayoutTokens.Space5)) { ScreenTitle(title, subtitle); Spacer(Modifier.height(LayoutTokens.Space4)); content() }
}

@Composable private fun ScreenTitle(title: String, subtitle: String) {
    Text(title, Modifier.semantics { heading() }, style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
    Text(subtitle, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun HardwareCard(modifier: Modifier = Modifier, container: Color = MaterialTheme.colorScheme.surfaceVariant, content: @Composable ColumnScope.() -> Unit) {
    Card(modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = container)) {
        Column(Modifier.fillMaxWidth().padding(LayoutTokens.Space4), content = content)
    }
}

@Composable private fun SectionHeading(icon: androidx.compose.ui.graphics.vector.ImageVector, title: String, modifier: Modifier = Modifier) {
    Row(modifier.semantics { heading() }, verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold) }
}

@Composable private fun StatusPill(label: String, positive: Boolean) {
    Surface(shape = CircleShape, color = if (positive) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary) {
        Text(label, Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space1), color = if (positive) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSecondary, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable private fun MetadataLine(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = LayoutTokens.Space1)) {
        Text(label, Modifier.widthIn(min = 86.dp), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable private fun EmptyState(icon: androidx.compose.ui.graphics.vector.ImageVector, title: String, body: String, modifier: Modifier) {
    Box(modifier, contentAlignment = Alignment.Center) {
        Column(Modifier.widthIn(max = 320.dp).padding(LayoutTokens.Space5), horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(icon, null, Modifier.size(48.dp), tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(LayoutTokens.Space3)); Text(title, style = MaterialTheme.typography.titleLarge)
            Text(body, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
