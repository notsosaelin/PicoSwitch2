package dev.picoswitch.companion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.BuildConfig
import dev.picoswitch.companion.data.AdapterAlias
import dev.picoswitch.companion.data.AdapterRecord
import dev.picoswitch.companion.data.AndroidBondState
import dev.picoswitch.companion.data.CompanionAssociationState
import dev.picoswitch.companion.model.BondInfo
import dev.picoswitch.companion.model.CapabilityState

/**
 * Ordinary product settings.
 *
 * What belongs here is what a user intentionally configures. Development and
 * troubleshooting instruments moved to Diagnostics: they previously dominated
 * this page, and one of them -- the wireless management gate -- asked the user
 * to understand an internal transport concept and could silently end the very
 * session issuing the command.
 */
@Composable
fun SettingsScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onImportAmiiboKeys: () -> Unit,
    theme: ThemeSelection,
) {
    var themeOpen by rememberSaveable { mutableStateOf(false) }
    var paletteOpen by rememberSaveable { mutableStateOf(false) }
    var renameTarget by remember { mutableStateOf<AdapterRecord?>(null) }
    var removeTarget by remember { mutableStateOf<AdapterRecord?>(null) }
    var removeBond by remember { mutableStateOf<BondInfo?>(null) }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)
        val gap = if (LocalShortWindow.current) LayoutTokens.Space3 else LayoutTokens.Space4

        val appearance: @Composable () -> Unit = {
            SectionCard(title = "Appearance", icon = Icons.Default.Palette) {
                SettingsRow(
                    title = "Theme",
                    supporting = theme.mode.title,
                    onClick = { themeOpen = true },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                SettingsRow(
                    title = "Accent",
                    supporting = theme.palette.title,
                    onClick = { paletteOpen = true },
                    trailing = {
                        ColorSwatch(theme.palette.leftSwatch, "Left accent sample", size = 20.dp)
                        Spacer(Modifier.width(LayoutTokens.Space1))
                        ColorSwatch(theme.palette.rightSwatch, "Right accent sample", size = 20.dp)
                        Icon(Icons.Default.ChevronRight, null)
                    },
                )
            }
        }

        val amiibo: @Composable () -> Unit = {
            SectionCard(title = "Amiibo", icon = Icons.Default.Contactless) {
                SettingsRow(
                    title = "Amiibo settings",
                    supporting = "Library, import and export, metadata key",
                    onClick = { viewModel.openOverlay(AppOverlay.AmiiboSettings) },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                SettingsRow(
                    title = "Metadata key",
                    // Says what the key is for, not whether it is present -- the
                    // chip beside it already answers that.
                    supporting = "Reads owner, nickname, and game data",
                    enabled = !ui.busy,
                    onClick = onImportAmiiboKeys,
                    trailing = {
                        StatusChip(
                            if (ui.amiiboKeysLoaded) "Loaded" else "None",
                            tone = if (ui.amiiboKeysLoaded) ChipTone.Positive else ChipTone.Neutral,
                        )
                    },
                )
            }
        }

        val adapter: @Composable () -> Unit = {
            SectionCard(title = "Adapters", icon = Icons.Default.Link) {
                if (ui.adapters.isEmpty()) {
                    InlineNotice("No adapters yet. Use Pair Adapter to add one.")
                } else {
                    // Several adapters may be known; exactly one is active. The
                    // row says which, because "connected" alone cannot: an
                    // adapter can be the selected one and still be powered off.
                    ui.adapters.forEach { record ->
                        val active = record.id == ui.activeAdapterId
                        SettingsRow(
                            title = if (ui.adapters.needsShortLabel(record)) {
                                "${record.displayName} • ${record.id.shortLabel}"
                            } else record.displayName,
                            supporting = adapterSupportingText(ui, record, active),
                            enabled = !ui.busy && !ui.relationshipStatus.attemptActive,
                            // Tapping the selected-but-disconnected row retries it.
                            // The coordinator refuses to tear down a healthy
                            // session, so this is safe on the connected row too.
                            onClick = { viewModel.selectAdapter(record.id) },
                            trailing = {
                                if (active && ui.connection.connected) {
                                    StatusChip("Connected", tone = ChipTone.Positive)
                                } else if (record.repairRequired) {
                                    StatusChip("Repair", tone = ChipTone.Error)
                                } else if (active) {
                                    StatusChip("Selected", tone = ChipTone.Neutral)
                                }
                                IconButton(
                                    onClick = { renameTarget = record },
                                    enabled = !ui.busy,
                                ) { Icon(Icons.Default.DriveFileRenameOutline, "Rename ${record.displayName}") }
                                IconButton(
                                    onClick = { removeTarget = record },
                                    enabled = !ui.busy,
                                ) { Icon(Icons.Default.Delete, "Remove ${record.displayName} from this app") }
                            },
                        )
                    }
                }
                HorizontalDivider()
                LabelValueRow(
                    "Android companion association",
                    when (ui.associationStates[ui.activeAdapterId] ?: ui.relationshipStatus.companionAssociation) {
                        CompanionAssociationState.Present -> "Present"
                        CompanionAssociationState.Missing -> "Not present"
                        // No longer "multiple adapters": several adapters are
                        // normal. This is two association records claiming the
                        // SAME adapter, which repair resolves.
                        CompanionAssociationState.Ambiguous -> "Duplicate records; repair needed"
                        CompanionAssociationState.Unknown -> "Not checked"
                    },
                )
                LabelValueRow(
                    "Android Bluetooth pairing",
                    when (ui.relationshipStatus.bond) {
                        AndroidBondState.Bonded -> "Paired"
                        AndroidBondState.Bonding -> "Pairing"
                        AndroidBondState.None -> "Not paired"
                        AndroidBondState.Unknown -> "Not checked"
                    },
                )
                HorizontalDivider()
                SubsectionLabel("Adapter Bluetooth LE bonds")
                when {
                    ui.snapshot.capabilities.bonds == CapabilityState.Unsupported ->
                        InlineNotice("This firmware does not report stored pairings.")
                    !ui.connection.connected ->
                        InlineNotice("Connect to the adapter to review its stored pairings.")
                    // Never present a partial enumeration as the whole list: a
                    // missing entry here is an entry the user cannot revoke.
                    ui.snapshot.bondsComplete != true ->
                        InlineNotice(
                            "The stored-pairing list could not be read completely, so none are shown.",
                            tone = ChipTone.Error,
                        )
                    ui.snapshot.bonds.isEmpty() -> InlineNotice("No Bluetooth LE bonds are stored on this adapter.")
                    else -> ui.snapshot.bonds.forEach { bond ->
                        SettingsRow(
                            title = bond.name?.takeIf(String::isNotBlank) ?: bond.address,
                            supporting = bond.name?.takeIf(String::isNotBlank)?.let { bond.address },
                            enabled = !ui.busy,
                            trailing = {
                                IconButton(onClick = { removeBond = bond }, enabled = !ui.busy) {
                                    Icon(Icons.Default.LinkOff, "Remove pairing ${bond.index}")
                                }
                            },
                        )
                    }
                }
            }
        }

        val about: @Composable () -> Unit = {
            SectionCard(title = "About", icon = Icons.Default.Info) {
                LabelValueRow("App", BuildConfig.VERSION_NAME)
                LabelValueRow("Firmware", ui.snapshot.firmware.version.ifBlank { "Not connected" })
                SettingsRow(
                    title = "Diagnostics",
                    supporting = "Connection, bridge, and adapter detail",
                    leading = Icons.Default.MonitorHeart,
                    onClick = { viewModel.openOverlay(AppOverlay.Diagnostics) },
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
            }
        }

        Column(Modifier.fillMaxSize()) {
            ScreenHeader(AppSection.Settings.title)
            Spacer(Modifier.height(LayoutTokens.Space3))
            if (twoColumn) {
                Row(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(gap),
                ) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                        appearance(); amiibo()
                    }
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                        adapter(); about()
                    }
                }
            } else {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(gap),
                ) {
                    appearance(); amiibo(); adapter(); about()
                    Spacer(Modifier.height(LayoutTokens.Space5))
                }
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

    renameTarget?.let { record ->
        AdapterRenameDialog(
            record = record,
            onDismiss = { renameTarget = null },
            onConfirm = { alias ->
                renameTarget = null
                viewModel.renameAdapter(record.id, alias)
            },
        )
    }

    removeTarget?.let { record ->
        ConfirmDialog(
            onDismiss = { removeTarget = null },
            title = "Remove ${record.displayName}?",
            // Say exactly what survives. This removes an app record; it is not a
            // Bluetooth operation, and claiming otherwise would leave the user
            // believing bonds were deleted that are still there.
            body = "This app forgets its name and saved details for this adapter. " +
                "Android Bluetooth pairing and the adapter's own stored pairings are not changed, " +
                "and your other adapters are unaffected.",
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = { removeTarget = null; viewModel.removeAdapterFromApp(record.id) },
        )
    }

    removeBond?.let { bond ->
        ConfirmDialog(
            onDismiss = { removeBond = null },
            title = "Remove this pairing?",
            // The app cannot tell whether the entry is this phone -- Android
            // does not expose our own Bluetooth address -- so the honest
            // statement is that it might be, and the session reconciles for real
            // afterwards rather than staying optimistically "Connected".
            body = "If this entry is this phone, the management session ends immediately and you will need to pair again.",
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = { viewModel.removeBond(bond.index); removeBond = null },
        )
    }
}

/**
 * Two rows reading "Living Room" are not a list.
 *
 * Duplicate aliases are allowed on purpose — the user may genuinely own two
 * adapters they think of the same way — so the disambiguation is additive
 * rather than a validation error.
 */
private fun List<AdapterRecord>.needsShortLabel(record: AdapterRecord): Boolean =
    count { it.displayName.equals(record.displayName, ignoreCase = true) } > 1

/**
 * What a row can honestly say about an adapter that may not be connected.
 *
 * Everything here except the live connection state is cache from the last
 * verified session, so it is phrased as history ("Last connected") rather than
 * as current truth.
 */
private fun adapterSupportingText(
    ui: CompanionUiState,
    record: AdapterRecord,
    active: Boolean,
): String {
    if (active && ui.connection.connected) {
        return listOfNotNull(
            record.lastFirmwareVersion?.let { "Firmware $it" },
            record.lastPersonality,
        ).joinToString(" · ").ifBlank { "Connected" }
    }
    // A failed switch is reported against the adapter that was chosen, and says
    // so plainly rather than pretending the previous adapter is still the one.
    if (active && ui.adapterSwitchFailure != null) return "Selected, not connected · tap to retry"
    if (record.repairRequired) return "Needs to be paired with this phone again"
    return when (ui.associationStates[record.id] ?: CompanionAssociationState.Unknown) {
        CompanionAssociationState.Ambiguous -> "Duplicate companion records; repair to resolve"
        else -> if (record.lastConnectedAtMillis != null) "Saved" else "Not connected yet"
    }
}

/** Local rename. The alias never leaves this phone and never becomes Bluetooth identity. */
@Composable
private fun AdapterRenameDialog(
    record: AdapterRecord,
    onDismiss: () -> Unit,
    onConfirm: (String?) -> Unit,
) {
    var text by rememberSaveable(record.id.value) { mutableStateOf(record.userAlias.orEmpty()) }
    PicoDialog(
        onDismiss = onDismiss,
        title = "Adapter name",
        confirmLabel = "Save",
        onConfirm = { onConfirm(text) },
    ) {
        OutlinedTextField(
            value = text,
            onValueChange = { text = it.take(AdapterAlias.MAX_LENGTH) },
            singleLine = true,
            label = { Text("Name on this phone") },
            placeholder = { Text(record.lastKnownName) },
            supportingText = { Text("Leave empty to use ${record.lastKnownName} • ${record.id.shortLabel}") },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}
