package dev.picoswitch.companion.ui

import android.content.Intent
import android.provider.Settings
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.data.AdapterId
import dev.picoswitch.companion.model.*

private val adapterTabs = listOf(
    PageTab("Console", Icons.Default.Tv),
    PageTab("Controllers", Icons.Default.Gamepad),
    PageTab("Adapters", Icons.Default.Cable),
)

/**
 * The adapter as a device (docs/architecture/companion-shell-layout.md, "What
 * lives where"): a header (name, state, firmware, Connect/Disconnect) over three
 * tabs, so the page fits the screen instead of scrolling.
 *
 *  - Console: the controller on the console, the emulated controller, the input
 *    source, and the Amiibo the adapter is serving.
 *  - Controllers: pair a controller; connected, paired and recent controllers.
 *  - Adapters: add an adapter; the adapters this phone remembers.
 *
 * Opens on Adapters when none is remembered yet, since that is the only tab
 * with anything to do. Colours are on Customization; Wake console is on Play.
 */
@Composable
fun AdapterScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onConnectAdapter: () -> Unit,
    onPairAdapter: () -> Unit,
    onRepairAdapter: (AdapterId?) -> Unit,
) {
    var tab by rememberSaveable { mutableIntStateOf(if (ui.adapters.isEmpty()) 2 else 0) }

    PageFrame(LayoutTokens.PageMaxWidth, scroll = false) {
        // On a short window (a landscape handheld) the tabs join the header row
        // and the header drops the adapter's name, which the top bar already
        // shows: one row of chrome instead of three.
        val tabs: @Composable () -> Unit = { PageTabs(adapterTabs, tab, { tab = it }) }
        if (LocalShortWindow.current) {
            AdapterHeader(ui, viewModel, onConnectAdapter, onRepairAdapter, leading = tabs)
        } else {
            AdapterHeader(ui, viewModel, onConnectAdapter, onRepairAdapter)
            tabs()
        }
        TabBody {
            when (tab) {
                0 -> ConsoleTab(ui, viewModel)
                1 -> AdapterControllersTab(ui, viewModel)
                else -> AdapterAdaptersTab(ui, viewModel, onPairAdapter, onRepairAdapter)
            }
        }
    }
}

@Composable
private fun AdapterHeader(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onConnectAdapter: () -> Unit,
    onRepairAdapter: (AdapterId?) -> Unit,
    leading: (@Composable () -> Unit)? = null,
) {
    val connected = ui.connection.connected
    val hasAdapter = ui.adapterRelationship != null
    val actionEnabled = !ui.busy && !ui.relationshipStatus.attemptActive
    val name = ui.adapterRelationship?.displayName ?: ui.connection.deviceName ?: "No adapter"

    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(LayoutTokens.Space3))
        }
        Column(Modifier.weight(1f)) {
            if (leading == null) {
                Text(
                    name,
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            val firmware = ui.snapshot.firmware.version.takeIf { connected && it.isNotBlank() }
            // No state here: the top bar is the app's ONE connection indicator
            // (companion-shell-layout.md), and repeating "Connected" a few
            // pixels under it read as two notifications for one fact.
            firmware?.let {
                Text(
                    "Firmware $it",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // Only a real message (why a connect failed). "Add one below." used
            // to sit here, repeating the Adapters tab's "Add an adapter" card
            // and pointing at nothing on the other two tabs.
            val detail = if (connected) null else ui.connection.message
            detail?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Spacer(Modifier.width(LayoutTokens.Space2))
        when {
            connected -> {
                IconButton(onClick = viewModel::refresh, enabled = !ui.busy) {
                    Icon(Icons.Default.Refresh, "Refresh adapter")
                }
                OutlinedButton(
                    onClick = viewModel::disconnect,
                    enabled = !ui.busy,
                    modifier = Modifier.widthIn(min = LayoutTokens.CommandMinWidth),
                ) { Text("Disconnect") }
            }
            ui.connection.phase == ConnectionPhase.RepairRequired -> {
                // Forgetting the Android bond is the one repair step this app
                // cannot perform: BluetoothDevice.removeBond() is a @SystemApi
                // gated on BLUETOOTH_PRIVILEGED. Offer the shortest supported
                // route to it instead of leaving the user to find it.
                val context = LocalContext.current
                TextButton(
                    onClick = {
                        runCatching {
                            context.startActivity(
                                Intent(Settings.ACTION_BLUETOOTH_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                            )
                        }
                    },
                    enabled = actionEnabled,
                ) { Text("Bluetooth settings") }
                Button(onClick = { onRepairAdapter(ui.activeAdapterId) }, enabled = actionEnabled) {
                    Text("Repair")
                }
            }
            hasAdapter -> Button(
                onClick = onConnectAdapter,
                enabled = actionEnabled && !AdapterStatus.inProgress(ui.connection.phase),
                modifier = Modifier.widthIn(min = LayoutTokens.CommandMinWidth),
            ) { Text("Connect") }
        }
    }
}

/** What the console sees: the controller on it, what the adapter pretends to be, and who drives it. */
@Composable
private fun ConsoleTab(ui: CompanionUiState, viewModel: CompanionViewModel) {
    if (!ui.connection.connected) {
        InlineNotice("Connect to the adapter to see what the console sees and change it.")
        return
    }
    // Three subjects side by side on a wide window, so a landscape handheld
    // (about 960 x 540 dp) shows the whole tab without scrolling.
    val cards: List<@Composable () -> Unit> = listOf(
        { ConsoleControllerCard(ui, viewModel) },
        { InputSourceCard(ui, viewModel) },
        // The Amiibo the adapter is serving. On Android it stays here because
        // it is the one place a tag that is NOT in the library can be seen,
        // saved or cleared; the library's inspector only shows tags it holds.
        { AdapterAmiiboCard(ui, viewModel) },
    )
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        if (maxWidth >= ConsoleThreeColumnWidth) {
            Row(Modifier.height(IntrinsicSize.Max), horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
                cards.forEach { card -> Box(Modifier.weight(1f)) { card() } }
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) { cards.forEach { it() } }
        }
    }
}

/** Three readable card columns need this much width; below it they stack. */
private val ConsoleThreeColumnWidth = 840.dp

@Composable
private fun ConsoleControllerCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val controller = ui.snapshot.controller
    SectionCard(title = "Controller on the console", icon = Icons.Default.SportsEsports) {
        DeviceStatusRow(
            role = "Controller",
            name = controller.name,
            connected = controller.attached,
            absentLabel = "None paired",
        )
        if (controller.attached && controller.batteryValid) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "Battery",
                    Modifier.widthIn(min = LayoutTokens.LabelWidth),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                LinearProgressIndicator(
                    { controller.batteryPercent / 100f },
                    Modifier.weight(1f).padding(horizontal = LayoutTokens.Space2),
                )
                Text(
                    "${controller.batteryPercent}%${if (controller.charging) " ⚡" else ""}",
                    style = MaterialTheme.typography.labelMedium,
                )
            }
        }
        if (ui.kbm.status.keyboardConnected || ui.kbm.status.mouseConnected) {
            SettingsRow(
                title = "Keyboard & Mouse",
                supporting = listOfNotNull(
                    "Keyboard".takeIf { ui.kbm.status.keyboardConnected },
                    "Mouse".takeIf { ui.kbm.status.mouseConnected },
                ).joinToString(" · ") + " connected",
                leading = Icons.Default.Keyboard,
                onClick = { viewModel.navigate(AppSection.Keyboard) },
                trailing = { Icon(Icons.Default.ChevronRight, null) },
            )
        }
        HorizontalDivider()
        EmulatedControllerRow(ui, viewModel)
    }
}

/**
 * The emulated controller, as a single choice rather than a row of buttons: a
 * menu states the current mode in one line and keeps the alternatives one tap
 * away, which is the shape this setting actually has.
 */
@Composable
private fun EmulatedControllerRow(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var open by remember { mutableStateOf(false) }
    val choices = ui.snapshot.personality.available.ifEmpty {
        listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight)
    }
    val current = ui.snapshot.personality.current
    val enabled = !ui.busy && ui.snapshot.capabilities.personality != CapabilityState.Unsupported

    run {
        Box(Modifier.fillMaxWidth()) {
            SettingsRow(
                title = "Emulated controller",
                supporting = current.title,
                leading = Icons.Default.Cable,
                enabled = enabled,
                onClick = { open = true },
                trailing = { Icon(Icons.Default.ArrowDropDown, null) },
            )
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                choices.forEach { mode ->
                    DropdownMenuItem(
                        text = { Text(mode.title) },
                        leadingIcon = if (mode == current) {
                            { Icon(Icons.Default.Check, null) }
                        } else null,
                        onClick = {
                            open = false
                            // Switching re-enumerates USB, so asking for the mode
                            // it is already in would drop the console's
                            // controller for no change at all.
                            if (mode != current) viewModel.switchPersonality(mode)
                        },
                    )
                }
            }
        }
    }
}

/** Which of the adapter's controllers drives the console. */
@Composable
private fun InputSourceCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val input = ui.snapshot.input
    SectionCard(title = "Input source", icon = Icons.Default.SwapHoriz) {
        when {
            ui.snapshot.capabilities.activeInput == CapabilityState.Unsupported -> InlineNotice(
                "This firmware does not support controller switching.",
            )
            input.sources.isEmpty() -> InlineNotice("No controller is currently available to the adapter.")
            else -> {
                input.sources.forEach { source ->
                    val selected = source.id == input.activeId || source.id == input.pendingId
                    SettingsRow(
                        title = source.name,
                        supporting = when {
                            // Active wins over pending: once the adapter has
                            // applied the switch both ids name this source, and
                            // reporting "switching" then would leave a streaming
                            // controller permanently labelled as waiting.
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
                ) { Text("Pause console input") }
            }
        }
    }
}
