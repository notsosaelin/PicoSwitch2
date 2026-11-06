@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.data.ColorTarget
import dev.picoswitch.companion.model.*

/**
 * The home page for the physical adapter.
 *
 * It answers, in order: which adapter is this, what is it doing, what is
 * plugged into it, and what can I change about it. Personality and appearance
 * live here rather than on a separate page because they are properties of this
 * one device; splitting them across two destinations made changing a colour a
 * navigation task.
 *
 * Technical detail is deliberately absent -- it is in Diagnostics.
 */
@Composable
fun AdapterScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var colorTarget by remember { mutableStateOf<ColorTarget?>(null) }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)
        val short = LocalShortWindow.current
        val gap = if (short) LayoutTokens.Space3 else LayoutTokens.Space4

        // FOUR TILES, ONE SUBJECT EACH: what the adapter is and what the console
        // sees it as, how it looks, what it can press, and what tag it holds.
        // Controller mode used to be a fifth card of its own, which separated
        // "this is your adapter" from "this is what it pretends to be" — two
        // halves of the same sentence.
        val controller: @Composable () -> Unit = { AdapterOverviewCard(ui, viewModel) }
        // Half of a two-column page still leaves room for three swatches on a
        // tablet; a phone in one column does not, and neither does a narrow
        // window. Decided from the page's own width, which it already knows.
        val narrowColours = twoColumn && maxWidth < LayoutTokens.ColorTileRowMinWidth * 2
        val appearance: @Composable () -> Unit = {
            AppearanceCard(ui, narrowColours) { colorTarget = it }
        }
        val console: @Composable () -> Unit = { ConsoleButtonsCard(ui, viewModel) }
        val amiibo: @Composable () -> Unit = { AdapterAmiiboCard(ui, viewModel) }

        Column(Modifier.fillMaxSize()) {
            ScreenHeader(AppSection.Adapter.title, subtitle = AppSection.Adapter.subtitle)
            Spacer(Modifier.height(LayoutTokens.Space3))
            if (twoColumn) {
                // IntrinsicSize.Min so the two tiles in a row agree on a height:
                // otherwise each column stacks to its own content and the four
                // tiles form a ragged grid.
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(gap),
                ) {
                    TileRow(gap, controller, appearance)
                    TileRow(gap, console, amiibo)
                    Spacer(Modifier.height(LayoutTokens.Space5))
                }
            } else {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(gap),
                ) {
                    controller(); appearance(); console(); amiibo()
                    Spacer(Modifier.height(LayoutTokens.Space5))
                }
            }
        }
    }

    colorTarget?.let { target ->
        ColorPickerDialog(
            title = target.title,
            initial = target.current(ui.snapshot.config),
            enabled = ui.connection.connected && !ui.busy,
            onDismiss = { colorTarget = null },
            onApply = { viewModel.saveColor(target, it); colorTarget = null },
        )
    }
}

/**
 * Two tiles side by side, sharing a height.
 *
 * Each tile is a Row child measured at IntrinsicSize.Min, so the taller of the
 * pair sets the row and the shorter stretches to match. Without it the two
 * columns stack independently and the grid goes ragged — the same unevenness a
 * card whose height follows its content always produces beside one whose does
 * not.
 */
@Composable
private fun TileRow(
    gap: androidx.compose.ui.unit.Dp,
    left: @Composable () -> Unit,
    right: @Composable () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().height(IntrinsicSize.Min),
        horizontalArrangement = Arrangement.spacedBy(gap),
    ) {
        Box(Modifier.weight(1f).fillMaxHeight()) { left() }
        Box(Modifier.weight(1f).fillMaxHeight()) { right() }
    }
}

/**
 * The one high-level status block: identity, state, and what is attached.
 *
 * Human-readable throughout. The firmware build hash, bridge contract, and
 * capability states that used to sit near the top are diagnostics and are no
 * longer shown here.
 */
@Composable
private fun AdapterOverviewCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val snapshot = ui.snapshot
    val connected = ui.connection.connected
    SectionCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Surface(
                shape = CircleShape,
                color = MaterialTheme.colorScheme.primaryContainer,
                modifier = Modifier.size(44.dp),
            ) {
                Box(contentAlignment = Alignment.Center) { Icon(Icons.Default.Gamepad, null) }
            }
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f)) {
                Text(
                    ui.connection.deviceName ?: ui.adapterRelationship?.displayName ?: "PicoSwitch2",
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    if (connected) "Acting as ${snapshot.personality.current.title}" else "Not connected",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            IconButton(
                onClick = viewModel::wake,
                enabled = connected && !ui.busy && snapshot.capabilities.wake != CapabilityState.Unsupported,
            ) { Icon(Icons.Default.PowerSettingsNew, "Wake console") }
        }

        if (!connected) {
            InlineNotice("Connect to the adapter to see its state and change its settings.")
            return@SectionCard
        }

        HorizontalDivider()

        val controller = snapshot.controller
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

        // What the console sees this adapter as, beside what the adapter IS.
        PersonalityPicker(ui, viewModel)

        // The happy path applies automatically. This recovery action exists only
        // when persistence succeeded but USB re-enumeration did not.
        if (ui.identityRefreshPending) {
            Surface(
                Modifier.fillMaxWidth(),
                shape = MaterialTheme.shapes.small,
                color = MaterialTheme.colorScheme.tertiaryContainer,
            ) {
                Row(
                    Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("USB identity refresh pending", style = MaterialTheme.typography.titleSmall)
                        Text(
                            "The color is saved; retry the brief USB reconnect.",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    Spacer(Modifier.width(LayoutTokens.Space2))
                    TextButton(onClick = viewModel::applyIdentityChanges, enabled = !ui.busy) {
                        Text("Retry")
                    }
                }
            }
        }
    }
}

/**
 * Controller mode, as a single choice rather than a row of buttons.
 *
 * A chip per personality put four equally-weighted controls on screen for a
 * setting with exactly one value, and grew a row every time a personality was
 * added. A menu states the current mode in one line and keeps the alternatives
 * one tap away, which is the shape this setting actually has.
 */
@Composable
private fun PersonalityPicker(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var open by remember { mutableStateOf(false) }
    val choices = ui.snapshot.personality.available.ifEmpty {
        listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight)
    }
    val current = ui.snapshot.personality.current
    val enabled = ui.connection.connected && !ui.busy &&
        ui.snapshot.capabilities.personality != CapabilityState.Unsupported

    Box(Modifier.fillMaxWidth()) {
        SettingsRow(
            title = "Controller mode",
            supporting = if (enabled || current != Personality.Unknown) {
                current.title
            } else {
                "Connect to change"
            },
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
                        // Switching re-enumerates USB, so asking for the mode it
                        // is already in would drop the console's controller for
                        // no change at all.
                        if (mode != current) viewModel.switchPersonality(mode)
                    },
                )
            }
        }
    }
}

/**
 * One compact appearance block for all three identity colours.
 *
 * Previously each colour owned a whole card with three sliders, which made
 * appearance the tallest thing on the page for a setting most people change
 * once. The current values stay immediately visible as swatches; editing moves
 * into a focused dialog.
 */
@Composable
private fun AppearanceCard(
    ui: CompanionUiState,
    /** True when this tile occupies a half-width column and cannot fit three swatches. */
    narrow: Boolean,
    onEdit: (ColorTarget) -> Unit,
) {
    val config = ui.snapshot.config
    val connected = ui.connection.connected
    val enabled = connected && !ui.busy
    val targets = ColorTarget.entries
    SectionCard(title = "Appearance", icon = Icons.Default.Palette) {
        Text(
            "Colours the console reads when the adapter connects.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (!connected) {
            // The disconnected snapshot's colours are zeroes, not the adapter's.
            // Three black swatches labelled #000000 would be a confident,
            // wrong answer to "what colour is my controller".
            InlineNotice("Connect to see and change the adapter's colours.")
            return@SectionCard
        }
        // TOLD, NOT MEASURED. This used BoxWithConstraints, which is a
        // SubcomposeLayout and cannot answer an intrinsic measurement — so the
        // moment the page asked the tiles to agree on a height, the card
        // crashed. The page already knows whether this is a half-width column,
        // which is the only thing the measurement was ever used to decide.
        if (!narrow) {
            Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                targets.forEach { target ->
                    ColorTile(target, target.current(config), enabled, Modifier.weight(1f)) { onEdit(target) }
                }
            }
        } else {
            Column {
                targets.forEach { target ->
                    SettingsRow(
                        title = target.title,
                        supporting = target.current(config).hex(),
                        enabled = enabled,
                        onClick = { onEdit(target) },
                        trailing = {
                            ColorSwatch(Color(target.current(config).argb()), "${target.title} colour")
                            Icon(Icons.Default.ChevronRight, null, Modifier.size(LayoutTokens.IconSize))
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun ColorTile(
    target: ColorTarget,
    color: RgbColor,
    enabled: Boolean,
    modifier: Modifier,
    onClick: () -> Unit,
) {
    Surface(
        onClick = onClick,
        modifier = modifier.heightIn(min = 96.dp),
        enabled = enabled,
        shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surface,
    ) {
        Column(
            Modifier.padding(LayoutTokens.Space3).fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        ) {
            ColorSwatch(Color(color.argb()), "${target.title} colour", size = 32.dp)
            Text(
                target.title,
                style = MaterialTheme.typography.labelMedium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
            Text(color.hex(), style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/**
 * The colour editor.
 *
 * Channel sliders rather than a colour wheel: the value that matters is the
 * exact byte triple the console is told, and a wheel cannot express that
 * precisely. The live preview is what makes the numbers legible.
 */
@Composable
private fun ColorPickerDialog(
    title: String,
    initial: RgbColor,
    enabled: Boolean,
    onDismiss: () -> Unit,
    onApply: (RgbColor) -> Unit,
) {
    var red by remember(initial) { mutableFloatStateOf(initial.red.toFloat()) }
    var green by remember(initial) { mutableFloatStateOf(initial.green.toFloat()) }
    var blue by remember(initial) { mutableFloatStateOf(initial.blue.toFloat()) }
    val current = RgbColor(red.toInt(), green.toInt(), blue.toInt())
    PicoDialog(
        onDismiss = onDismiss,
        title = title,
        confirmLabel = "Save",
        onConfirm = { onApply(current) },
        confirmEnabled = enabled,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            ColorSwatch(Color(current.argb()), "Preview", size = 40.dp)
            Spacer(Modifier.width(LayoutTokens.Space3))
            Text(current.hex(), Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
        }
        ChannelSlider("Red", red, Color(0xFFE05B5B)) { red = it }
        ChannelSlider("Green", green, Color(0xFF4CAF6A)) { green = it }
        ChannelSlider("Blue", blue, Color(0xFF5B8FE0)) { blue = it }
    }
}

@Composable
private fun ChannelSlider(label: String, value: Float, tint: Color, onValue: (Float) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            label,
            Modifier.width(52.dp),
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.SemiBold,
            color = tint,
        )
        Slider(
            value = value,
            onValueChange = onValue,
            valueRange = 0f..255f,
            modifier = Modifier.weight(1f).semantics { contentDescription = "$label ${value.toInt()}" },
        )
        Text(
            value.toInt().toString().padStart(3),
            Modifier.width(36.dp),
            style = MaterialTheme.typography.labelMedium,
        )
    }
}

/**
 * Console buttons a typical Android handheld has no physical key for.
 *
 * These report press and release rather than a completed click, because the
 * console distinguishes a tap from a hold (Capture screenshots on release but
 * records on a long hold, and Home has its own hold behaviour).
 */
@Composable
private fun ConsoleButtonsCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    // Only meaningful while this handheld is actually streaming to the console;
    // at any other phase a press would go nowhere.
    val live = ui.bridge.phase == BridgeLinkPhase.Playing
    SectionCard(title = "Console buttons", icon = Icons.Default.Gamepad) {
        Text(
            if (live) "Buttons this handheld does not have"
            else "Available while this handheld is the controller",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            HoldButton(Modifier.weight(1f), Icons.Default.Home, "Home", live) {
                viewModel.setConsoleButton(ControllerButton.Home, it)
            }
            HoldButton(Modifier.weight(1f), Icons.Default.PhotoCamera, "Capture", live) {
                viewModel.setConsoleButton(ControllerButton.Capture, it)
            }
            HoldButton(Modifier.weight(1f), Icons.AutoMirrored.Filled.Chat, "GameChat", live) {
                viewModel.setConsoleButton(ControllerButton.C, it)
            }
        }
    }
}

@Composable
private fun HoldButton(
    modifier: Modifier,
    icon: ImageVector,
    label: String,
    enabled: Boolean,
    onHold: (Boolean) -> Unit,
) {
    Surface(
        modifier
            .heightIn(min = 56.dp)
            .then(
                if (!enabled) Modifier else Modifier.pointerInput(label) {
                    detectTapGestures(
                        onPress = {
                            onHold(true)
                            // Releases on cancel too, so a drag off the button or an
                            // interrupted gesture can never latch the button down.
                            tryAwaitRelease()
                            onHold(false)
                        },
                    )
                },
            )
            .semantics { contentDescription = label },
        shape = MaterialTheme.shapes.medium,
        color = if (enabled) MaterialTheme.colorScheme.secondaryContainer
        else MaterialTheme.colorScheme.surface,
    ) {
        Column(
            Modifier.padding(LayoutTokens.Space2).fillMaxWidth(),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Icon(icon, null, Modifier.size(LayoutTokens.IconSize))
            Spacer(Modifier.height(LayoutTokens.Space1))
            Text(
                label,
                style = MaterialTheme.typography.labelMedium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

private val ColorTarget.title: String
    get() = when (this) {
        ColorTarget.Body -> "Body"
        ColorTarget.LeftAccent -> "Left accent"
        ColorTarget.RightAccent -> "Right accent"
    }

private fun ColorTarget.current(config: AdapterConfig): RgbColor = when (this) {
    ColorTarget.Body -> config.bodyColor
    ColorTarget.LeftAccent -> config.leftAccent
    ColorTarget.RightAccent -> config.rightAccent
}

private fun RgbColor.hex(): String = "#%02X%02X%02X".format(red, green, blue)
