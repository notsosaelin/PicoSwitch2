package dev.picoswitch.companion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.protocol.BridgeContract
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.model.PeerInfo
import dev.picoswitch.companion.model.PeerRole
import dev.picoswitch.companion.model.PeerTransport

/**
 * Using this handheld as the controller, and choosing which controller the
 * adapter forwards to the console.
 *
 * Conceptually one topic -- who is driving -- so the adapter's active-source
 * choice and this phone's own bridge live together rather than in two places.
 */
@Composable
fun ControllerScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onPrepare: () -> Unit,
    onOpenTouchGamepad: () -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)
        val gap = if (LocalShortWindow.current) LayoutTokens.Space3 else LayoutTokens.Space4

        val active: @Composable () -> Unit = { ActiveInputCard(ui, viewModel) }
        val bridge: @Composable () -> Unit = { BridgeCard(ui, viewModel, onPrepare, onOpenTouchGamepad) }
        val source: @Composable () -> Unit = { InputSourceCard(ui, viewModel) }
        val saved: @Composable () -> Unit = { SavedPairingsCard(ui, viewModel) }
        val layout: @Composable () -> Unit = { ControllerLayoutCard(ui, viewModel) }

        Column(Modifier.fillMaxSize()) {
            ScreenHeader(AppSection.Controller.title, subtitle = AppSection.Controller.subtitle)
            Spacer(Modifier.height(LayoutTokens.Space3))
            if (twoColumn) {
                Row(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(gap),
                ) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                        active(); source(); saved()
                    }
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                        bridge(); layout()
                    }
                }
            } else {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(gap),
                ) {
                    active(); bridge(); source(); saved(); layout()
                    Spacer(Modifier.height(LayoutTokens.Space5))
                }
            }
        }
    }
}

@Composable
private fun ActiveInputCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val input = ui.snapshot.input
    SectionCard(title = "Console input", icon = Icons.Default.SwapHoriz) {
        when {
            !ui.connection.connected -> InlineNotice(
                "Connect to the adapter to choose which controller drives the console.",
            )
            ui.snapshot.capabilities.activeInput == CapabilityState.Unsupported -> InlineNotice(
                "This firmware does not support controller switching.",
            )
            input.sources.isEmpty() -> InlineNotice(
                "No controller is currently available to the adapter.",
            )
            else -> {
                input.sources.forEach { source ->
                    val selected = source.id == input.activeId || source.id == input.pendingId
                    SettingsRow(
                        title = source.name,
                        supporting = when {
                            // Active wins over pending: once the adapter has applied
                            // the switch both ids name this source, and reporting
                            // "switching" then would leave a streaming controller
                            // permanently labelled as waiting.
                            source.id == input.activeId && input.awaitingFresh ->
                                "Switching · waiting for fresh input"
                            source.id == input.activeId -> "Controlling the console"
                            source.id == input.pendingId -> "Switching · waiting for fresh input"
                            else -> "Available"
                        },
                        enabled = !ui.busy,
                        onClick = { viewModel.setActiveInput(source.id) },
                        role = Role.RadioButton,
                        trailing = { RadioButton(selected = selected, onClick = null, enabled = !ui.busy) },
                    )
                }
                if (input.truncated) {
                    InlineNotice(
                        "More controllers are connected than this firmware can report.",
                        tone = ChipTone.Error,
                    )
                }
                OutlinedButton(
                    onClick = { viewModel.setActiveInput(0) },
                    enabled = !ui.busy && (input.activeId != 0L || input.pendingId != 0L),
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Pause console input") }
            }
        }
    }
}

@Composable
private fun BridgeCard(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onPrepare: () -> Unit,
    onOpenTouchGamepad: () -> Unit,
) {
    val phase = ui.bridge.phase
    SectionCard(
        title = "This handheld",
        icon = Icons.Default.BluetoothAudio,
        trailing = {
            StatusChip(
                phase.productLabel(),
                tone = when (phase) {
                    BridgeLinkPhase.Playing -> ChipTone.Positive
                    BridgeLinkPhase.Failed, BridgeLinkPhase.Unsupported -> ChipTone.Error
                    else -> ChipTone.Neutral
                },
            )
        },
    ) {
        if (phase == BridgeLinkPhase.Playing) {
            InlineNotice("Keep this screen open while playing.", icon = Icons.Default.Visibility, tone = ChipTone.Positive)
        }
        ui.bridge.message?.let {
            Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        // Firmware/app bridge-contract skew. Shown here and nowhere else: this is
        // the screen the user is on when battery, motion and rumble are missing
        // while buttons still work, which is exactly what a skew looks like.
        // Silent on the compatible path so it adds no clutter.
        if (BridgeContract.warrantsWarning(ui.bridgeCompatibility)) {
            InlineNotice(ui.bridgeCompatibility.summary, icon = Icons.Default.Warning, tone = ChipTone.Error)
        }
        when (phase) {
            BridgeLinkPhase.Idle, BridgeLinkPhase.Unsupported, BridgeLinkPhase.Failed ->
                Button(
                    onClick = onPrepare,
                    enabled = ui.selectedSourceDescriptor != null && ui.adapterRelationship != null,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(if (ui.adapterRelationship == null) "Pair the adapter first" else "Use this handheld") }

            BridgeLinkPhase.Playing ->
                Button(
                    onClick = viewModel::stopControllerBridge,
                    modifier = Modifier.fillMaxWidth(),
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                ) { Text("Stop playing") }

            BridgeLinkPhase.Ready -> {
                val host = viewModel.knownControllerHost()
                FilledTonalButton(
                    onClick = { host?.let(viewModel::connectControllerHost) },
                    enabled = host != null,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Reconnect controller mode") }
                Text(
                    if (host == null) "The saved adapter bond is unavailable. Reconnect or pair the adapter again."
                    else "Controller mode reuses the relationship established by Pair Adapter; there is no second chooser.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            else -> LinearProgressIndicator(Modifier.fillMaxWidth())
        }

        HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space1))

        // Deliberately NOT gated on a selected physical input. A phone or tablet
        // with no gamepad at all is a complete controller source once its own
        // screen is the controller, and requiring the user to pick a physical
        // device first would be asking them to choose something that has nothing
        // to do with what they are about to play with.
        Text(
            "Play using this screen instead of a physical controller.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        FilledTonalButton(
            onClick = onOpenTouchGamepad,
            enabled = ui.adapterRelationship != null,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Icon(Icons.Default.TouchApp, null, Modifier.size(LayoutTokens.IconSize))
            Spacer(Modifier.width(LayoutTokens.Space2))
            Text(if (ui.adapterRelationship == null) "Pair the adapter first" else "Touch Gamepad")
        }
    }
}

/**
 * The bridge phase in product language.
 *
 * The enum names are engineering vocabulary for the HID Device lifecycle; the
 * user's question is only whether this handheld is currently playing, on its
 * way there, or unable to.
 */
private fun BridgeLinkPhase.productLabel(): String = when (this) {
    BridgeLinkPhase.Idle -> "Not in use"
    BridgeLinkPhase.Preparing -> "Preparing"
    BridgeLinkPhase.Registering -> "Registering"
    BridgeLinkPhase.Ready -> "Ready"
    BridgeLinkPhase.Connecting -> "Connecting"
    BridgeLinkPhase.Playing -> "Playing"
    BridgeLinkPhase.Unsupported -> "Not supported"
    BridgeLinkPhase.Failed -> "Failed"
}

/**
 * What the adapter has stored, as opposed to what is plugged in right now.
 *
 * Three separations the copy has to keep straight, because collapsing any of
 * them is how a user ends up forgetting the wrong thing:
 *
 *  * **Bonded is not connected.** A saved controller that is switched off is
 *    still saved.
 *  * **A controller is not the phone.** This phone appears in the inventory in
 *    up to two roles -- BLE management and Controller Link -- and neither
 *    belongs in a list headed "Saved controllers".
 *  * **Unknown is not "none".** After a reboot the adapter can see its stored
 *    keys but cannot yet say whose they are; that is reported as unknown rather
 *    than guessed into the controller list.
 *
 * Read-only in this pass. Forgetting a pairing is a later phase, and offering
 * the action before the firmware can perform it safely would be worse than not
 * offering it.
 */
@Composable
private fun SavedPairingsCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val inventory = ui.snapshot.peers
    SectionCard(
        title = "Saved pairings",
        icon = Icons.Default.Bluetooth,
        trailing = {
            IconButton(
                onClick = viewModel::refreshPeers,
                enabled = ui.connection.connected && !ui.busy,
            ) { Icon(Icons.Default.Refresh, "Refresh saved pairings") }
        },
    ) {
        when {
            !ui.connection.connected ->
                InlineNotice("Connect to the adapter to see what it has paired.")
            ui.snapshot.capabilities.peers == CapabilityState.Unsupported ->
                InlineNotice("Update the adapter firmware to see its saved pairings here.")
            ui.snapshot.capabilities.peers == CapabilityState.Unknown ->
                InlineNotice("Tap refresh to read the adapter's saved pairings.")
            inventory.peers.isEmpty() ->
                InlineNotice("This adapter has no saved pairings.")
            else -> {
                val controllers = inventory.controllers
                if (controllers.isEmpty()) {
                    InlineNotice("No saved controller has been identified on this adapter yet.")
                } else {
                    controllers.forEach { peer -> PeerRow(peer) }
                }
                val others = inventory.companionsAndUnknown
                if (others.isNotEmpty()) {
                    HorizontalDivider()
                    // Explicitly not "controllers". This section is where the
                    // phone's own two relationships live, plus anything the
                    // adapter cannot yet name.
                    SubsectionLabel("This phone and unidentified peers")
                    others.forEach { peer -> PeerRow(peer) }
                }
            }
        }
    }
}

@Composable
private fun PeerRow(peer: PeerInfo) {
    SettingsRow(
        title = peer.name?.takeIf(String::isNotBlank)
            // Never the raw address as a name. A short suffix is enough to tell
            // two unnamed devices apart and reads as an identifier, not a label.
            ?: "Controller • ${peer.address.takeLast(4)}",
        supporting = peerSupportingText(peer),
        enabled = false,
        trailing = {
            if (peer.connected) StatusChip("Connected", tone = ChipTone.Positive)
            else if (peer.bonded) StatusChip("Saved", tone = ChipTone.Neutral)
        },
    )
}

private fun peerSupportingText(peer: PeerInfo): String {
    val role = when (peer.role) {
        PeerRole.ManagementCompanion -> "This phone — management"
        PeerRole.ControllerLink -> "This phone — Controller Link"
        PeerRole.PhysicalController -> "Controller"
        // Deliberately not "unrecognised device": the adapter holds a key for
        // it, so the honest statement is that it cannot say what it is yet.
        PeerRole.Unknown -> "Saved pairing, not yet identified"
    }
    val transports = when {
        peer.multiTransport -> "Bluetooth Classic + LE"
        peer.transports.contains(PeerTransport.Classic) -> "Bluetooth Classic"
        peer.transports.contains(PeerTransport.Le) -> "Bluetooth LE"
        // Connected with no stored key: a controller part-way through pairing.
        else -> "Not saved"
    }
    return "$role · $transports"
}

@Composable
private fun InputSourceCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val selectedName = ui.sourceDevices.firstOrNull { it.descriptor == ui.selectedSourceDescriptor }?.name
    SectionCard(
        title = "Built-in input",
        icon = Icons.Default.Gamepad,
        trailing = {
            IconButton(onClick = viewModel::refreshSources) { Icon(Icons.Default.Refresh, "Refresh inputs") }
        },
    ) {
        when {
            ui.sourceDevices.isEmpty() -> InlineNotice("No usable controller is visible to the app.")

            // One controller: state it and move on. Making the user select the
            // only possible option is pure friction, so no picker is drawn.
            !ui.sourceChoiceRequired && selectedName != null -> Row(
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Default.CheckCircle, null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(LayoutTokens.Space2))
                Text(
                    selectedName,
                    style = MaterialTheme.typography.bodyLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            // Two or more: the choice is real, so show it.
            else -> ui.sourceDevices.forEach { source ->
                SettingsRow(
                    title = source.name,
                    supporting = "VID %04X · PID %04X".format(source.vendorId, source.productId),
                    onClick = { viewModel.selectSource(source.descriptor) },
                    role = Role.RadioButton,
                    trailing = {
                        RadioButton(selected = source.descriptor == ui.selectedSourceDescriptor, onClick = null)
                    },
                )
            }
        }
    }
}

@Composable
private fun ControllerLayoutCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val enabled = ui.selectedSourceDescriptor != null
    SectionCard(title = "Face buttons", icon = Icons.Default.SwapHoriz) {
        Text(
            "Android reports button positions, which may not match the letters printed on a Nintendo-style handheld.",
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
                        enabled = enabled,
                        role = Role.RadioButton,
                        onClick = { viewModel.setControllerFaceLayout(layout) },
                    ),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(selected = ui.requestedFaceLayout == layout, onClick = null, enabled = enabled)
                Spacer(Modifier.width(LayoutTokens.Space2))
                Column(Modifier.weight(1f)) {
                    Text(layout.title, style = MaterialTheme.typography.bodyLarge)
                    Text(
                        layout.description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        if (ui.requestedFaceLayout == ControllerFaceLayout.Auto) {
            InlineNotice(
                "Auto resolved to ${ui.resolvedFaceLayout.layout.title}: ${ui.resolvedFaceLayout.reason}.",
                tone = ChipTone.Positive,
            )
        }
    }
}

/**
 * Live normalized controller state.
 *
 * A troubleshooting instrument rather than a product feature, so it renders in
 * Diagnostics; it is here as a composable because the state it reads belongs to
 * this screen's subject.
 */
@Composable
internal fun LiveInputBlock(state: dev.picoswitch.bridge.core.ControllerState) {
    Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
        AxisMeter("LX", state.leftX, Modifier.weight(1f))
        AxisMeter("LY", state.leftY, Modifier.weight(1f))
        AxisMeter("RX", state.rightX, Modifier.weight(1f))
        AxisMeter("RY", state.rightY, Modifier.weight(1f))
    }
    // Four retained directions, not the wire hat code: the UI shows the
    // normalized model, and the hat is a protocol detail the encoder owns.
    val dpad = listOfNotNull(
        "Up".takeIf { state.dpadUp }, "Right".takeIf { state.dpadRight },
        "Down".takeIf { state.dpadDown }, "Left".takeIf { state.dpadLeft },
    )
    LabelValueRow("Triggers", "L2 ${state.leftTrigger} · R2 ${state.rightTrigger}", monospace = true)
    LabelValueRow("D-pad", dpad.ifEmpty { listOf("centered") }.joinToString(" + "))
    LabelValueRow(
        "Buttons",
        if (state.buttons.isEmpty()) "None held" else state.buttons.joinToString(" · ") { it.name },
    )
}

@Composable
private fun AxisMeter(label: String, value: Int, modifier: Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        LinearProgressIndicator({ value / 255f }, Modifier.fillMaxWidth().height(6.dp))
        Spacer(Modifier.height(LayoutTokens.Space1))
        Text("$label $value", style = MaterialTheme.typography.labelSmall)
    }
}
