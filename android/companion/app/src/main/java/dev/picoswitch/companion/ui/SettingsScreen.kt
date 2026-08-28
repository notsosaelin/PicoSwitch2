package dev.picoswitch.companion.ui

import android.text.format.DateUtils
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
import dev.picoswitch.companion.data.PeerListing
import dev.picoswitch.companion.model.BondInfo
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.model.PeerRole
import dev.picoswitch.companion.model.PeerTransport

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
    var removeHistory by remember { mutableStateOf<PeerListing?>(null) }

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

        // Two cards, not one. "Which adapters does this app know" and "what has
        // this adapter paired with" are different subjects with different
        // consequences -- removing an adapter is app-local, forgetting a
        // controller is not -- and one card carrying both invited the user to
        // read a destructive action against the wrong list.
        val pairedAdapters: @Composable () -> Unit = {
            SectionCard(title = "Paired adapters", icon = Icons.Default.Link) {
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
            }
        }

        val pairedControllers: @Composable () -> Unit = {
            PairedControllersCard(
                ui = ui,
                viewModel = viewModel,
                onRemoveHistory = { removeHistory = it },
            )
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
                        pairedAdapters(); pairedControllers(); about()
                    }
                }
            } else {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(gap),
                ) {
                    appearance(); amiibo(); pairedAdapters(); pairedControllers(); about()
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

    removeHistory?.let { listing ->
        ConfirmDialog(
            onDismiss = { removeHistory = null },
            title = "Remove ${listing.displayName} from history?",
            // Explicitly not a Bluetooth operation, and said so plainly: the
            // adapter already holds no key for this device. Confusing this with
            // "Forget pairing" is how a user believes a controller was unpaired
            // when nothing on the adapter changed.
            body = "This app forgets that ${listing.displayName} was ever connected to this adapter. " +
                "The adapter already has no saved pairing for it, so nothing on the adapter changes.",
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = {
                viewModel.removePeerFromHistory(listing.peerId)
                removeHistory = null
            },
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
 * What the adapter has paired with, as opposed to what is connected right now.
 *
 * Four separations the copy has to keep straight, because collapsing any of them
 * is how a user ends up acting on the wrong device:
 *
 *  * **Bonded is not connected.** A saved controller that is switched off is
 *    still saved, and belongs under Saved pairings rather than nowhere.
 *  * **A controller is not the phone.** This phone appears in the adapter's
 *    inventory in up to two roles -- BLE management and Controller Link -- and
 *    neither belongs in a list of controllers.
 *  * **Unknown is not "none".** After the adapter reboots it can see its stored
 *    keys but cannot yet say whose they are. That is reported as unidentified,
 *    never guessed into a controller; where this app has seen the adapter
 *    identify the device before, the row says the name is remembered.
 *  * **Recent is not saved.** A row under Recent has no key on the adapter at
 *    all. Its only action removes an app-local memory, which is why it says
 *    "Remove from history" and not "Forget".
 *
 * Forgetting an actual pairing is a later phase and is deliberately not offered
 * here: the firmware cannot yet perform it atomically, and an action that half
 * works is worse than one that is absent.
 *
 * **This card holds physical controllers and nothing else.** The management
 * phone is not a controller, and neither is a raw LE bond slot, a CTKD-derived
 * Classic record, or a security record nothing can attribute. Those are real,
 * and they are in Diagnostics, where a reader is asking a different question.
 * Note this is a presentation boundary resting on a corrected firmware identity
 * model, not a filter hiding one: the adapter now reports the companion under
 * its resolved identity, so it is one peer with role `management` rather than
 * an identity row plus a stray RPA row.
 */
@Composable
private fun PairedControllersCard(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onRemoveHistory: (PeerListing) -> Unit,
) {
    val inventory = ui.controllerInventory
    SectionCard(
        title = "Paired controllers",
        icon = Icons.Default.Bluetooth,
        trailing = {
            IconButton(
                onClick = viewModel::refreshPeers,
                enabled = ui.connection.connected && !ui.busy,
            ) { Icon(Icons.Default.Refresh, "Refresh paired controllers") }
        },
    ) {
        when {
            ui.snapshot.capabilities.peers == CapabilityState.Unsupported ->
                InlineNotice("Update the adapter firmware to see its paired controllers here.")
            // Recent survives a disconnect on purpose: it is this app's memory,
            // not the adapter's, and it is the only thing there is to show while
            // the adapter is away. Everything else needs the adapter present.
            !ui.connection.connected && inventory.recent.isEmpty() ->
                InlineNotice("Connect to the adapter to see what it has paired.")
            !inventory.hasControllers && ui.snapshot.capabilities.peers == CapabilityState.Unknown ->
                InlineNotice("Tap refresh to read the adapter's paired controllers.")
            !inventory.hasControllers && inventory.hasDiagnosticPeers ->
                // The adapter holds records, but none of them is a controller.
                // Saying "no controllers" would be true and unhelpful; the rows
                // it does hold are in Diagnostics, and this says where.
                InlineNotice(
                    "No paired controllers. This adapter's remaining Bluetooth records are this " +
                        "phone's own, or ones it cannot identify; both are in Diagnostics.",
                )
            !inventory.hasControllers ->
                InlineNotice("This adapter has no paired controllers.")
            else -> {
                if (inventory.connected.isNotEmpty()) {
                    SubsectionLabel("Connected")
                    inventory.connected.forEach { PeerRow(it) }
                }
                if (inventory.saved.isNotEmpty()) {
                    if (inventory.connected.isNotEmpty()) HorizontalDivider()
                    SubsectionLabel("Saved pairings")
                    inventory.saved.forEach { PeerRow(it) }
                }
                if (inventory.recent.isNotEmpty()) {
                    HorizontalDivider()
                    SubsectionLabel("Recent")
                    inventory.recent.forEach { listing ->
                        PeerRow(
                            listing,
                            trailing = {
                                IconButton(
                                    onClick = { onRemoveHistory(listing) },
                                    enabled = !ui.busy,
                                ) {
                                    // Deliberately not the icon a bond removal
                                    // uses. One trash can for both meanings is
                                    // exactly what the design forbids.
                                    Icon(
                                        Icons.Default.HistoryToggleOff,
                                        "Remove ${listing.displayName} from history",
                                    )
                                }
                            },
                        )
                    }
                }
            }
        }

    }
}

@Composable
private fun PeerRow(
    listing: PeerListing,
    trailing: @Composable (RowScope.() -> Unit)? = null,
) {
    SettingsRow(
        title = listing.displayName,
        supporting = peerSupportingText(listing),
        // Read-only rows. Nothing here is tappable until selective forget
        // exists, and a row that highlights but does nothing reads as broken.
        enabled = trailing != null,
        trailing = trailing ?: {
            when {
                listing.connected -> StatusChip("Connected", tone = ChipTone.Positive)
                listing.bonded -> StatusChip("Saved", tone = ChipTone.Neutral)
                else -> StatusChip("Not paired", tone = ChipTone.Neutral)
            }
        },
    )
}

/**
 * One line describing a peer, sourced honestly.
 *
 * Anything the adapter cannot currently prove is attributed to this app's
 * memory in the text itself. The protocol requires that an `unknown` role be
 * rendered as unidentified rather than promoted, and remembering what the
 * adapter once proved is not the same claim as asserting it now.
 */
private fun peerSupportingText(listing: PeerListing): String {
    val role = when {
        listing.role == PeerRole.ManagementCompanion -> "This phone — management"
        listing.role == PeerRole.ControllerLink -> "This phone — Controller Link"
        listing.role == PeerRole.PhysicalController -> "Controller"
        listing.rememberedRole == PeerRole.ManagementCompanion -> "This phone — management, remembered"
        listing.rememberedRole == PeerRole.ControllerLink -> "This phone — Controller Link, remembered"
        listing.rememberedRole == PeerRole.PhysicalController -> "Controller, remembered"
        // Deliberately not "unrecognised device": the adapter holds a key for
        // it, so the honest statement is that it cannot say what it is yet.
        else -> "Saved pairing, not yet identified"
    }
    val transports = when {
        // A row that exists only in history describes a device the adapter no
        // longer stores anything for, so it has no transport to report.
        listing.historyOnly -> null
        listing.transports.size > 1 -> "Bluetooth Classic + LE"
        listing.transports.contains(PeerTransport.Classic) -> "Bluetooth Classic"
        listing.transports.contains(PeerTransport.Le) -> "Bluetooth LE"
        // Connected with no stored key: a controller part-way through pairing.
        else -> "Not saved"
    }
    val lastConnected = listing.lastConnectedAtMillis
        ?.takeIf { !listing.connected }
        ?.let { "Last connected ${relativeTime(it)}" }
    return listOfNotNull(role, transports, lastConnected).joinToString(" · ")
}

/**
 * Human phrasing for a timestamp this app recorded itself.
 *
 * Only ever applied to app-side history. The adapter has no real-time clock and
 * no time sync, so it never supplies a wall-clock time and none is invented for
 * it; every value formatted here was stamped on this phone.
 */
private fun relativeTime(millis: Long): String = DateUtils.getRelativeTimeSpanString(
    millis,
    System.currentTimeMillis(),
    DateUtils.MINUTE_IN_MILLIS,
    DateUtils.FORMAT_ABBREV_RELATIVE,
).toString()

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
