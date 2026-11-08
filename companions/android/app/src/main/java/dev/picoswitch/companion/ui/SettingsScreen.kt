package dev.picoswitch.companion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.BuildConfig

/**
 * This app's own preferences (docs/architecture/companion-shell-layout.md):
 * theme, face buttons, and the door to Diagnostics. Opened from the top bar's
 * gear, so no dock item is selected.
 *
 * What used to live here and moved: paired adapters and paired controllers are
 * facts about the adapter (Adapter → Adapters / Controllers); the Amiibo
 * library settings and key are the Amiibo page's (its overflow menu).
 * Development and troubleshooting instruments are in Diagnostics.
 */
@Composable
fun SettingsScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    theme: ThemeSelection,
) {
    var themeOpen by rememberSaveable { mutableStateOf(false) }
    var paletteOpen by rememberSaveable { mutableStateOf(false) }
    var faceOpen by rememberSaveable { mutableStateOf(false) }
    var navOpen by rememberSaveable { mutableStateOf(false) }

    // ONE capped column of rows, the shared settings-page pattern
    // (companion-shell-layout.md, "Other pages") and what the desktops draw: a
    // name and short description on the left, the value or a chevron on the
    // right. The earlier two-column card grid left a hole under the shorter
    // column and spent a card title on every one or two rows.
    PageFrame(LayoutTokens.PageMaxWidth) {
        ScreenHeader(AppSection.Settings.title)
        SectionCard {
            Column(Modifier.fillMaxWidth()) {
                SettingsRow(
                    title = "Theme",
                    supporting = theme.mode.title,
                    onClick = { themeOpen = true },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                HorizontalDivider()
                SettingsRow(
                    title = "Accent",
                    supporting = theme.palette.title,
                    onClick = { paletteOpen = true },
                    trailing = {
                        ColorSwatch(theme.palette.leftSwatch, "Left accent sample", size = 20.dp)
                        ColorSwatch(theme.palette.rightSwatch, "Right accent sample", size = 20.dp)
                        Icon(Icons.Default.ChevronRight, null)
                    },
                )
                HorizontalDivider()
                SettingsRow(
                    title = "Navigation",
                    supporting = theme.navigation.title,
                    onClick = { navOpen = true },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                HorizontalDivider()
                // Gated on a chosen controller because the choice is stored for
                // that controller; with none chosen there is nothing to store it
                // against.
                val faceEnabled = ui.selectedSourceDescriptor != null
                SettingsRow(
                    title = "Face buttons",
                    supporting = if (faceEnabled) {
                        listOf(
                            FaceLayoutChoices.label(ui.requestedFaceLayout) + ".",
                            FaceLayoutChoices.reason(ui.requestedFaceLayout, ui.resolvedFaceLayout.layout),
                        ).filter { it.isNotEmpty() }.joinToString(" ")
                    } else {
                        "Choose a controller on Play first."
                    },
                    enabled = faceEnabled,
                    onClick = { faceOpen = true },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                HorizontalDivider()
                SettingsRow(
                    title = "Diagnostics",
                    supporting = "Connection, bridge and adapter detail",
                    onClick = { viewModel.openOverlay(AppOverlay.Diagnostics) },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                HorizontalDivider()
                SettingsRow(
                    title = "Version",
                    trailing = {
                        Text(
                            BuildConfig.VERSION_NAME,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                )
            }
        }
    }

    if (themeOpen) PicoDialog(
        onDismiss = { themeOpen = false },
        title = "Theme",
        dismissLabel = "Done",
    ) {
        ThemeMode.entries.forEach { mode ->
            SettingsRow(
                title = mode.title,
                supporting = mode.description,
                onClick = { viewModel.setThemeMode(mode) },
                role = Role.RadioButton,
                trailing = { RadioButton(selected = theme.mode == mode, onClick = null) },
            )
        }
    }

    if (paletteOpen) PicoDialog(
        onDismiss = { paletteOpen = false },
        title = "Accent",
        dismissLabel = "Done",
    ) {
        Text(
            "Application accents only. These never change the colours the adapter reports to the console.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        AccentPalette.entries.forEach { palette ->
            SettingsRow(
                title = palette.title,
                supporting = palette.description,
                onClick = { viewModel.setAccentPalette(palette) },
                role = Role.RadioButton,
                trailing = {
                    ColorSwatch(palette.leftSwatch, "${palette.title} left sample", size = 20.dp)
                    Spacer(Modifier.width(LayoutTokens.Space1))
                    ColorSwatch(palette.rightSwatch, "${palette.title} right sample", size = 20.dp)
                    Spacer(Modifier.width(LayoutTokens.Space2))
                    RadioButton(selected = theme.palette == palette, onClick = null)
                },
            )
        }
    }

    if (navOpen) PicoDialog(
        onDismiss = { navOpen = false },
        title = "Navigation",
        dismissLabel = "Done",
    ) {
        NavPlacement.entries.forEach { placement ->
            SettingsRow(
                title = placement.title,
                onClick = { viewModel.setNavPlacement(placement) },
                role = Role.RadioButton,
                trailing = { RadioButton(selected = theme.navigation == placement, onClick = null) },
            )
        }
    }

    if (faceOpen) PicoDialog(
        onDismiss = { faceOpen = false },
        title = "Face buttons",
        dismissLabel = "Done",
    ) {
        Text(
            "Android reports button positions, which may not match the letters printed on a Nintendo-style handheld.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        FaceLayoutChoices.options.forEach { (layout, label) ->
            SettingsRow(
                title = label,
                onClick = { viewModel.setControllerFaceLayout(layout) },
                role = Role.RadioButton,
                trailing = { RadioButton(selected = ui.requestedFaceLayout == layout, onClick = null) },
            )
        }
    }
}
