package dev.picoswitch.companion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.data.ColorTarget
import dev.picoswitch.companion.model.*

/**
 * How the emulated controller looks: its colours
 * (docs/architecture/companion-shell-layout.md, "What lives where"). The
 * clickable controller drawing replaces these rows when it exists.
 *
 * Saving a colour applies it: the adapter stores it and briefly re-enumerates
 * USB so the console reads it. The only extra action is the retry for when the
 * save succeeded and the re-enumeration did not.
 */
@Composable
fun CustomizationScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var colorTarget by remember { mutableStateOf<ColorTarget?>(null) }
    val config = ui.snapshot.config
    val connected = ui.connection.connected
    val enabled = connected && !ui.busy

    PageFrame(LayoutTokens.PageMaxWidth) {
        ScreenHeader(
            AppSection.Customization.title,
            subtitle = "Colours the console reads when the adapter connects.",
        )
        SectionCard(title = "Colours", icon = Icons.Default.Palette) {
            if (!connected) {
                // The disconnected snapshot's colours are zeroes, not the
                // adapter's. Three black swatches labelled #000000 would be a
                // confident, wrong answer to "what colour is my controller".
                InlineNotice("Connect to the adapter to see and change its colours.")
                return@SectionCard
            }
            ColorTarget.entries.forEach { target ->
                val color = target.current(config)
                SettingsRow(
                    title = target.title,
                    supporting = color.hex(),
                    enabled = enabled,
                    onClick = { colorTarget = target },
                    trailing = {
                        ColorSwatch(Color(color.argb()), "${target.title} colour")
                        Spacer(Modifier.width(LayoutTokens.Space2))
                        OutlinedButton(
                            onClick = { colorTarget = target },
                            enabled = enabled,
                            modifier = Modifier.widthIn(min = LayoutTokens.CommandMinWidth),
                        ) { Text("Change") }
                    },
                )
            }
        }

        if (ui.identityRefreshPending) {
            InlineNotice(
                "The colour is saved, but the console has not re-read it yet.",
                icon = Icons.Default.Warning,
                tone = ChipTone.Attention,
            )
            Button(onClick = viewModel::applyIdentityChanges, enabled = !ui.busy) {
                Text("Show on the console")
            }
        }
    }

    colorTarget?.let { target ->
        ColorPickerDialog(
            title = target.title,
            initial = target.current(config),
            enabled = enabled,
            onDismiss = { colorTarget = null },
            onApply = { viewModel.saveColor(target, it); colorTarget = null },
        )
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

/** The same names as the desktop companions' Customization page. */
private val ColorTarget.title: String
    get() = when (this) {
        ColorTarget.Body -> "Body"
        ColorTarget.LeftAccent -> "Left Joy-Con accent"
        ColorTarget.RightAccent -> "Right Joy-Con accent"
    }

private fun ColorTarget.current(config: AdapterConfig): RgbColor = when (this) {
    ColorTarget.Body -> config.bodyColor
    ColorTarget.LeftAccent -> config.leftAccent
    ColorTarget.RightAccent -> config.rightAccent
}

private fun RgbColor.hex(): String = "#%02X%02X%02X".format(red, green, blue)
