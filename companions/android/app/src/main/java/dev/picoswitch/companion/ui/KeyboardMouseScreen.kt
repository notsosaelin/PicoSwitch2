@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.Mouse
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.RestartAlt
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.data.KbmBankSlot
import dev.picoswitch.companion.data.KbmBankView
import dev.picoswitch.companion.data.KbmProfileLibrary
import dev.picoswitch.companion.model.*

/**
 * The Keyboard & Mouse management surface.
 *
 * TWO STORES, KEPT VISIBLY APART. This is the organising idea of the screen and
 * the correction it exists to express:
 *
 *  - YOUR LIBRARY is the user's collection of profiles. It lives in this app,
 *    has no size limit, and needs no adapter: creating, editing, renaming,
 *    duplicating, saving and deleting all work with nothing paired and send zero
 *    management commands.
 *  - ON ADAPTER is the adapter's working set — three positions in each of two
 *    layout banks, plus a built-in Default that consumes none of them. It exists
 *    so the adapter keeps working with no app attached, and getting content into
 *    it is always an explicit act.
 *
 * The previous screen treated the adapter's six slots as the user's library, so
 * "New" erased flash, "Save" changed what the console might run, and neither
 * worked while disconnected. Save now means "keep this in my library", and only
 * Assign, Update, Remove, Activate and On startup reach the device.
 *
 * Arbitration internals the firmware also reports — connection indices,
 * generation counters, rejection tallies — are not here; they belong to
 * Diagnostics.
 */
@Composable
fun KeyboardMouseScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val kbm = ui.kbm
    val layout = ui.kbmLayout
    val draft = ui.kbmDraft
    var editing by remember { mutableStateOf<EditTarget?>(null) }
    var resetAllOpen by rememberSaveable { mutableStateOf(false) }
    var resetProfileOpen by rememberSaveable { mutableStateOf<KbmProfile?>(null) }
    var advancedOpen by rememberSaveable { mutableStateOf(false) }
    var nameDialog by remember { mutableStateOf<NameDialog?>(null) }
    var deleteOpen by rememberSaveable { mutableStateOf(false) }
    var assignOpen by rememberSaveable { mutableStateOf(false) }
    var removeSlot by remember { mutableStateOf<KbmBankSlot?>(null) }
    var switchKeyFor by rememberSaveable { mutableStateOf<Int?>(null) }

    // Open something on arrival so the editor is never blank. Deliberately the
    // BUILT-IN DEFAULT rather than whatever the adapter is running: opening a
    // library profile the user did not choose would make the first Save write
    // over it.
    LaunchedEffect(Unit) { if (draft == null) viewModel.openKbmLocalProfile(null) }

    // Follow the adapter when the derived layout changes underneath the screen
    // (plugging a mouse in switches it), but never while an edit is unsaved:
    // switching layouts reopens the editor, and a hardware event must not throw
    // away work the user has not saved.
    LaunchedEffect(kbm.activeProfile) {
        if (draft?.dirty != true) viewModel.selectKbmLayout(kbm.activeProfile)
    }

    // The adapter half of the screen. The library half never consults this.
    val adapterReady = ui.connection.connected && kbm.readiness == KbmReadiness.Ready
    val adapterLive = adapterReady && !ui.kbmBusy

    // Mapping, Mouse, Profiles (docs/architecture/companion-shell-layout.md,
    // "Keyboard & Mouse"). Saved so a dialog round trip or rotation keeps the tab.
    var tab by rememberSaveable { mutableIntStateOf(0) }
    // The ISO and international keys, behind a toggle because few keyboards
    // have them (companion-shell-layout.md, "Keyboard & Mouse").
    var moreKeys by rememberSaveable { mutableStateOf(false) }

    PageFrame(LayoutTokens.WidePageMaxWidth, scroll = false) {
        // On a short window (a landscape handheld) the tabs move up into the
        // header row and the Mapping actions into the toolbar, so the drawn
        // keyboard keeps the height. Six key rows are what this page is for;
        // two rows of chrome above them were what made it scroll.
        val short = LocalShortWindow.current
        val loaded = kbm.readiness != KbmReadiness.FirmwareUpdateRequired && kbm.readiness != KbmReadiness.Error
        val tabs: @Composable () -> Unit = { PageTabs(kbmTabs, tab, { tab = it }) }
        KbmHeader(kbm, ui, viewModel, adapterLive, leading = if (short && loaded) tabs else null)

        when {
            // TOP-LEVEL STATE FIRST. When the profile contract could not be
            // loaded the screen used to fall through to a pre-profile mapping
            // page, which looked like a half-built feature rather than a
            // failed read.
            kbm.readiness == KbmReadiness.FirmwareUpdateRequired -> EmptyStateBlock(
                Icons.Default.Keyboard,
                "Firmware update required",
                "This adapter's firmware predates the keyboard and mouse profile " +
                    "system, so this screen cannot configure it. Update the adapter, " +
                    "then reload. ${kbm.fault}",
                Modifier.fillMaxSize(),
            )

            kbm.readiness == KbmReadiness.Error -> EmptyStateBlock(
                Icons.Default.Keyboard,
                "The adapter's reply could not be used",
                "The adapter implements this feature but returned data this app " +
                    "could not read completely. ${kbm.fault}",
                Modifier.fillMaxSize(),
            )

            else -> {
                // ONE TOOLBAR: the tabs, and the open profile. The open profile
                // shows on every tab because Mapping and Profiles both act on it,
                // and an unsaved edit must never be hidden by a tab switch.
                val openProfile: @Composable () -> Unit = {
                    OpenProfileBar(
                        library = ui.kbmLibrary,
                        profiles = kbm.profiles,
                        layout = layout,
                        draft = draft,
                        selectedId = ui.kbmSelectedLocalId,
                        enabled = !ui.kbmBusy,
                        onLayout = viewModel::selectKbmLayout,
                        onOpen = viewModel::openKbmLocalProfile,
                        onSave = { viewModel.saveKbmProfile() },
                        onSaveAsNew = { nameDialog = NameDialog.SaveAsNew },
                        onDiscard = viewModel::discardKbmDraft,
                    )
                }
                val mappingActions: @Composable () -> Unit = {
                    MappingActions(
                        draft = draft,
                        moreKeys = moreKeys,
                        onToggleMoreKeys = { moreKeys = !moreKeys },
                        adapterLive = adapterLive,
                        onReset = { resetProfileOpen = layout },
                    )
                }
                FlowRow(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
                    verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space1),
                    itemVerticalAlignment = Alignment.CenterVertically,
                ) {
                    if (short) {
                        openProfile()
                        Spacer(Modifier.weight(1f))
                        if (tab == 0) mappingActions()
                    } else {
                        tabs()
                        Spacer(Modifier.weight(1f))
                        openProfile()
                    }
                }

                if (tab == 0) {
                    // Not TabBody: the drawn keyboard sizes itself to the height
                    // that is left, and a scroll would make that height infinite.
                    Column(Modifier.weight(1f).fillMaxWidth()) {
                        KeyMappingCard(
                            layout = layout,
                            draft = draft,
                            enabled = !ui.kbmBusy,
                            moreKeys = moreKeys,
                            actions = if (short) null else mappingActions,
                            onEdit = { editing = it },
                            // The desktop's middle-click: an edit to None, LOCAL
                            // ONLY, through the same path as the dialog.
                            onClear = { viewModel.editKbmBinding(it, KbmDestination.None) },
                        )
                    }
                } else TabBody {
                    when (tab) {
                        1 -> {
                            TwoColumns(
                                leftWeight = 1f,
                                rightWeight = 2f,
                                left = {
                                    // Mouse buttons are bindings of the
                                    // keyboard-and-mouse layout only.
                                    if (layout == KbmProfile.KeyboardMouse) {
                                        MouseButtonsCard(
                                            layout = layout,
                                            draft = draft,
                                            enabled = !ui.kbmBusy,
                                            onEdit = { editing = it },
                                            onClear = { viewModel.editKbmBinding(it, KbmDestination.None) },
                                        )
                                    } else {
                                        SectionCard(title = "Mouse buttons", icon = Icons.Default.Mouse) {
                                            InlineNotice("Mouse buttons are mapped in the Keyboard & mouse layout.")
                                        }
                                    }
                                },
                                right = {
                                    // Tuning belongs to the adapter, not to a profile.
                                    MouseTuningCard(
                                        kbm = kbm,
                                        enabled = adapterLive,
                                        advancedOpen = advancedOpen,
                                        onToggleAdvanced = { advancedOpen = !advancedOpen },
                                        onPreview = viewModel::previewMouseField,
                                        onCommit = viewModel::commitMouseField,
                                    )
                                },
                            )
                        }

                        else -> TwoColumns(
                            left = {
                                LibraryCard(
                                    library = ui.kbmLibrary,
                                    profiles = kbm.profiles,
                                    layout = layout,
                                    draft = draft,
                                    adapterReady = adapterReady,
                                    enabled = !ui.kbmBusy,
                                    onNew = { nameDialog = NameDialog.New },
                                    onDuplicate = { nameDialog = NameDialog.Duplicate },
                                    onRename = { nameDialog = NameDialog.Rename },
                                    onDelete = { deleteOpen = true },
                                    onAssign = { assignOpen = true },
                                )
                                BankCard(
                                    kbm = kbm,
                                    layout = layout,
                                    adapterReady = adapterReady,
                                    enabled = adapterLive,
                                    onActivate = viewModel::activateKbmPosition,
                                    onBoot = viewModel::setKbmBootPosition,
                                    onRemove = { removeSlot = it },
                                    onCopy = viewModel::copyKbmPositionToLibrary,
                                )
                            },
                            right = {
                                SectionCard(title = "Reset everything", icon = Icons.Default.RestartAlt) {
                                    SettingsRow(
                                        title = "Adapter defaults",
                                        supporting = "Mouse settings and both mappings. Your library is kept.",
                                        enabled = adapterLive,
                                        trailing = {
                                            OutlinedButton(
                                                onClick = { resetAllOpen = true },
                                                enabled = adapterLive,
                                                modifier = Modifier.widthIn(min = LayoutTokens.CommandMinWidth),
                                            ) { Text("Reset") }
                                        },
                                    )
                                }
                                SwitchKeysCard(
                                    switches = kbm.switches,
                                    adapterReady = adapterReady,
                                    enabled = adapterLive,
                                    onChoose = { switchKeyFor = it },
                                    onClear = { key -> viewModel.bindKbmSwitch(key, null) },
                                )
                            },
                        )
                    }
                }
            }
        }
    }

    editing?.let { target ->
        BindingEditorDialog(
            target = target,
            onDismiss = { editing = null },
            onApply = { destination ->
                // LOCAL ONLY. Every rebind edits the open draft and sends
                // nothing; thirty edits still cost zero flash erases.
                viewModel.editKbmBinding(target.source, destination)
                editing = null
            },
            onRestoreDefault = {
                // Distinct from binding None: this REMOVES the override so the
                // layout's canonical binding applies again, where None is an
                // override meaning "this input does nothing".
                viewModel.restoreKbmBinding(target.source)
                editing = null
            },
        )
    }

    if (resetAllOpen) ConfirmDialog(
        onDismiss = { resetAllOpen = false },
        title = "Restore all defaults?",
        // Stated precisely because the firmware's only reset that reaches the
        // mouse settings also clears both mapping profiles. Offering it as a
        // mouse-only reset would be a claim the adapter cannot honour.
        body = "This restores the adapter's built-in mouse settings and both mapping profiles. Custom bindings are lost. Your library is not affected.",
        confirmLabel = "Restore defaults",
        destructive = true,
        icon = Icons.Default.RestartAlt,
        onConfirm = { resetAllOpen = false; viewModel.resetKbmAll() },
    )

    resetProfileOpen?.let { profile ->
        ConfirmDialog(
            onDismiss = { resetProfileOpen = null },
            title = "Restore ${profile.title} mapping?",
            body = "This clears every custom binding in the ${profile.title} mapping on the adapter. Mouse settings, the other layout, and your library are not affected.",
            confirmLabel = "Restore",
            destructive = true,
            icon = Icons.Default.RestartAlt,
            onConfirm = { resetProfileOpen = null; viewModel.resetKbmProfile(profile) },
        )
    }

    nameDialog?.let { which ->
        // The LOCAL library, not the adapter's residents: a suggestion drawn from
        // the adapter would collide with the user's own profiles and would need a
        // connection to make.
        val existing = ui.kbmLibrary.forLayout(layout).map { it.name }
        ProfileNameDialog(
            title = when (which) {
                NameDialog.New -> "New profile"
                NameDialog.Duplicate -> "Duplicate profile"
                NameDialog.Rename -> "Rename profile"
                NameDialog.SaveAsNew -> "Save as new profile"
            },
            body = when (which) {
                NameDialog.New -> "Starts from the built-in Default mapping. It " +
                    "stays in your library until you assign it to the adapter."
                NameDialog.Duplicate -> "Copies what you are looking at, including " +
                    "unsaved changes, into a new library profile."
                NameDialog.Rename -> "Only the name changes. Nothing on the " +
                    "adapter is affected."
                NameDialog.SaveAsNew -> "Default is a template and is never " +
                    "written into, so this saves your changes as a new profile."
            },
            initial = when (which) {
                NameDialog.Rename -> draft?.name.orEmpty()
                NameDialog.Duplicate ->
                    ui.kbmLibrary.suggestName(layout, "${draft?.name.orEmpty()} copy")
                else -> ui.kbmLibrary.suggestName(layout, "My mapping")
            },
            taken = existing.filter { which != NameDialog.Rename || it != draft?.name },
            onDismiss = { nameDialog = null },
            onConfirm = { name ->
                nameDialog = null
                when (which) {
                    NameDialog.New -> viewModel.newKbmProfile(name)
                    NameDialog.Duplicate -> viewModel.duplicateKbmProfile(name)
                    NameDialog.Rename -> viewModel.renameKbmProfile(name)
                    NameDialog.SaveAsNew -> viewModel.saveKbmProfile(name)
                }
            },
        )
    }

    if (deleteOpen && draft != null && !draft.isBuiltin) {
        // A resident copy is a SEPARATE snapshot the adapter owns and may be
        // running. Deleting the library profile must not remove it, and the user
        // is told so rather than left to discover it.
        val resident = kbm.profiles.profiles.firstOrNull {
            it.layout == layout && it.fingerprint == draft.fingerprint
        }
        ConfirmDialog(
            onDismiss = { deleteOpen = false },
            title = "Delete '${draft.name}' from your library?",
            body = if (resident == null) {
                "This removes it from this app. Nothing on the adapter changes."
            } else {
                "This removes it from this app. The copy on the adapter " +
                    "(${KbmPositions.label(resident.position)}) stays, and the " +
                    "console keeps working as it does now."
            },
            confirmLabel = "Delete",
            destructive = true,
            icon = Icons.Default.RestartAlt,
            onConfirm = { deleteOpen = false; viewModel.deleteKbmProfile() },
        )
    }

    if (assignOpen && draft != null) {
        AssignDialog(
            name = draft.name,
            // Default is built in and cannot be assigned into.
            slots = KbmBankView.bank(kbm.profiles, kbm.switches, layout)
                .filterNot { it.isDefault },
            onDismiss = { assignOpen = false },
            onConfirm = { position ->
                assignOpen = false
                viewModel.assignKbmPosition(position)
            },
        )
    }

    removeSlot?.let { slot ->
        ConfirmDialog(
            onDismiss = { removeSlot = null },
            title = "Remove '${slot.residentLabel}' from ${slot.positionLabel}?",
            // The two stores are independent by design, so this is worth saying
            // out loud: a user who expects Remove to delete their profile, or
            // Delete to free an adapter position, will be surprised otherwise.
            body = "The adapter's copy is removed." +
                (if (slot.isRuntime || slot.isBoot) {
                    " This layout falls back to the built-in Default."
                } else {
                    ""
                }) +
                " Your library keeps this profile.",
            confirmLabel = "Remove",
            destructive = true,
            icon = Icons.Default.RestartAlt,
            onConfirm = {
                removeSlot = null
                viewModel.removeKbmPosition(slot.position)
            },
        )
    }

    switchKeyFor?.let { position ->
        SwitchKeyDialog(
            position = position,
            onDismiss = { switchKeyFor = null },
            onConfirm = { source ->
                switchKeyFor = null
                viewModel.bindKbmSwitch(source, position)
            },
        )
    }
}

/**
 * Ask for a profile name.
 *
 * Bounded to what the adapter can store and checked for collisions here as well
 * as in firmware, so the user is told before they press OK rather than after the
 * adapter refuses.
 */
@Composable
private fun ProfileNameDialog(
    title: String,
    body: String,
    initial: String,
    taken: List<String>,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    var text by rememberSaveable { mutableStateOf(initial) }
    val trimmed = text.trim()
    val collision = taken.any { it.equals(trimmed, ignoreCase = true) }
    val valid = trimmed.isNotEmpty() && !collision

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
                Text(body, style = MaterialTheme.typography.bodyMedium)
                OutlinedTextField(
                    value = text,
                    onValueChange = { if (it.length <= KBM_PROFILE_NAME_MAX) text = it },
                    singleLine = true,
                    isError = collision,
                    supportingText = {
                        if (collision) Text("That name is already used in this layout.")
                    },
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(trimmed) }, enabled = valid) { Text("OK") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Matches NS2_KBM_PROFILE_NAME_MAX minus its NUL. */
private const val KBM_PROFILE_NAME_MAX = 19

/**
 * A single-choice row.
 *
 * The radio button is real rather than decorative: selection here is a choice
 * among alternatives, and a row that conveyed it only by highlight would say
 * nothing to a screen reader.
 */
@Composable
private fun SelectableRow(
    title: String,
    supporting: String?,
    selected: Boolean,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    SettingsRow(
        title = title,
        supporting = supporting,
        enabled = enabled,
        onClick = onClick,
        role = Role.RadioButton,
        trailing = {
            RadioButton(selected = selected, onClick = null, enabled = enabled)
        },
    )
}

/** Which naming prompt is open. All four are LOCAL library operations. */
private enum class NameDialog { New, Duplicate, Rename, SaveAsNew }

/** Which input, in which layout, the focused editor is currently editing. */
private data class EditTarget(
    val profile: KbmProfile,
    val source: KbmSource,
    val current: KbmDestination,
    val custom: Boolean,
)

// ---------------------------------------------------------------------------
// Status and mode
// ---------------------------------------------------------------------------

private val kbmTabs = listOf(
    PageTab("Mapping", Icons.Default.Keyboard),
    PageTab("Mouse", Icons.Default.Mouse),
    PageTab("Profiles", Icons.Default.Tune),
)

/**
 * The page header: the mode in force and the devices the adapter sees, with the
 * mode selector on the right. No page title: the dock already says where the
 * user is (companion-shell-layout.md, "Other pages").
 *
 * [leading] is the tabs on a short window, which move up into this row.
 */
@Composable
private fun KbmHeader(
    kbm: KbmState,
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    adapterLive: Boolean,
    leading: (@Composable () -> Unit)? = null,
) {
    val status = kbm.status
    val sources = ui.snapshot.input.sources
    val devices = listOfNotNull(
        resolveKbmDeviceName(status.keyboardConnected, status.keyboardConn, sources)
            ?: "Keyboard".takeIf { status.keyboardConnected },
        resolveKbmDeviceName(status.mouseConnected, status.mouseConn, sources)
            ?: "Mouse".takeIf { status.mouseConnected },
    )
    // One switch instead of a four-way mode menu (owner, 2026-11-08: "fix that
    // automatic / controller / keyboard / keyboard and mouse"). On is
    // Automatic: the adapter already picks Keyboard or Keyboard + Mouse from
    // what is connected, so the line names what it picked. Off is Controller.
    // A Keyboard or Keyboard + Mouse override set elsewhere (the portal, an
    // older app) still reads as on, and the line says what is in force.
    val kbmOn = status.modeOverride != KbmMode.Controller
    val mode = when {
        !kbmOn -> "Off"
        status.mode == KbmMode.Controller || status.mode == KbmMode.Automatic -> "On"
        else -> status.mode.title
    }
    val line = when {
        !ui.connection.connected -> "Connect the adapter to use a keyboard or mouse."
        !kbmOn -> "Off · keyboards and mice are ignored."
        devices.isEmpty() -> "Pair a Bluetooth keyboard or mouse with the adapter."
        else -> "$mode · ${devices.joinToString(" · ")}"
    }

    val actions: @Composable () -> Unit = {
        Row(verticalAlignment = Alignment.CenterVertically) {
        if (ui.kbmBusy) {
            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
            Spacer(Modifier.width(LayoutTokens.Space2))
        }
        if (kbm.dirty) {
            Button(onClick = viewModel::saveAdapterConfiguration, enabled = adapterLive) {
                Icon(Icons.Default.Save, null, Modifier.size(18.dp))
                Spacer(Modifier.width(LayoutTokens.Space2))
                Text("Save")
            }
            Spacer(Modifier.width(LayoutTokens.Space2))
        }
        Text("Keyboard & mouse", style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.width(LayoutTokens.Space2))
        Switch(
            checked = kbmOn,
            onCheckedChange = { on -> viewModel.setKbmMode(if (on) KbmMode.Automatic else KbmMode.Controller) },
            enabled = ui.connection.connected && !ui.kbmBusy,
        )
        IconButton(
            onClick = viewModel::refreshKbm,
            enabled = ui.connection.connected && !ui.kbmBusy,
        ) { Icon(Icons.Default.Refresh, "Refresh keyboard and mouse state") }
        }
    }
    val text: @Composable (Modifier) -> Unit = { modifier ->
        Text(
            line,
            modifier,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
    }

    BoxWithConstraints(Modifier.fillMaxWidth()) {
        if (maxWidth >= LayoutTokens.NarrowPageWidth) {
            // One line: the text takes what the tabs and actions leave.
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
            ) {
                leading?.invoke()
                text(Modifier.weight(1f))
                actions()
            }
        } else {
            // A narrow window: the text takes the whole line and the actions
            // wrap under it instead of squeezing it to nothing.
            FlowRow(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space1),
                itemVerticalAlignment = Alignment.CenterVertically,
            ) {
                leading?.invoke()
                text(Modifier.weight(1f).widthIn(min = 200.dp))
                actions()
            }
        }
    }
}

/** Two columns on a wide window, one on a narrow one. */
@Composable
private fun TwoColumns(
    left: @Composable ColumnScope.() -> Unit,
    right: @Composable ColumnScope.() -> Unit,
    leftWeight: Float = 3f,
    rightWeight: Float = 2f,
) {
    BoxWithConstraints(Modifier.fillMaxWidth()) {
        if (twoColumnLayout(maxWidth)) {
            Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
                Column(Modifier.weight(leftWeight), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3), content = left)
                Column(Modifier.weight(rightWeight), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3), content = right)
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) { left(); right() }
        }
    }
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

/**
 * YOUR LIBRARY: the user's own profiles for one layout.
 *
 * NOTHING in this card requires a connection and nothing in it writes to an
 * adapter. New, Duplicate, Rename, Delete, Save and Discard are all local, so
 * the whole card stays live with nothing paired — which is the point, because a
 * mapping is composed rather than measured and never needed a device.
 *
 * The only button here that reaches the adapter is Assign, and it says so.
 *
 * Each row also carries where that profile stands relative to the adapter. That
 * is a RELATIONSHIP, computed by [KbmBankView], not a property of the draft: a
 * single flag could not express "I edited this locally, the adapter still has
 * the old copy, and it is running that old copy".
 */
/** Selector buttons in the toolbar: tighter than Material's 24 dp sides so tabs and the open profile share one row on a 960 dp handheld. */
private val CompactButtonPadding = PaddingValues(start = 16.dp, end = 8.dp, top = 8.dp, bottom = 8.dp)

@Composable
private fun OpenProfileBar(
    library: KbmProfileLibrary,
    profiles: KbmProfiles,
    layout: KbmProfile,
    draft: KbmLocalDraft?,
    selectedId: String?,
    enabled: Boolean,
    onLayout: (KbmProfile) -> Unit,
    onOpen: (String?) -> Unit,
    onSave: () -> Unit,
    onSaveAsNew: () -> Unit,
    onDiscard: () -> Unit,
) {
    val rows = KbmBankView.library(library, profiles, layout)
    val dirty = draft?.dirty == true
    val builtin = draft?.isBuiltin != false
    var open by remember { mutableStateOf(false) }
    var layoutOpen by remember { mutableStateOf(false) }

    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space1),
        itemVerticalAlignment = Alignment.CenterVertically,
    ) {
        Box {
            OutlinedButton(
                onClick = { open = true },
                enabled = enabled,
                modifier = Modifier.widthIn(min = LayoutTokens.CommandMinWidth, max = LayoutTokens.SelectorMinWidth),
                contentPadding = CompactButtonPadding,
            ) {
                Text(
                    draft?.name ?: "Default (built-in)",
                    Modifier.weight(1f, fill = false),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Icon(Icons.Default.ArrowDropDown, null)
            }
            // Default is always offered and is never a library row: it is the
            // template every profile starts from, it consumes no adapter
            // position, and it cannot be renamed or deleted.
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                DropdownMenuItem(
                    text = { Text("Default (built-in)") },
                    leadingIcon = if (selectedId == null) { { Icon(Icons.Default.Check, null) } } else null,
                    onClick = { open = false; onOpen(null) },
                )
                rows.forEach { row ->
                    DropdownMenuItem(
                        text = {
                            Column {
                                Text(row.profile.name)
                                Text(
                                    row.stateLabel,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        },
                        leadingIcon = if (selectedId == row.profile.id) { { Icon(Icons.Default.Check, null) } } else null,
                        onClick = { open = false; onOpen(row.profile.id) },
                    )
                }
            }
        }
        Box {
            OutlinedButton(onClick = { layoutOpen = true }, enabled = enabled, contentPadding = CompactButtonPadding) {
                Text(layout.title, maxLines = 1)
                Icon(Icons.Default.ArrowDropDown, null)
            }
            DropdownMenu(expanded = layoutOpen, onDismissRequest = { layoutOpen = false }) {
                KbmProfile.entries.forEach { option ->
                    DropdownMenuItem(
                        text = {
                            Column {
                                Text(option.title)
                                Text(
                                    when (option) {
                                        KbmProfile.Keyboard -> "The keyboard drives both sticks."
                                        KbmProfile.KeyboardMouse -> "The mouse aims; the keyboard moves and acts."
                                    },
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        },
                        leadingIcon = if (option == layout) { { Icon(Icons.Default.Check, null) } } else null,
                        onClick = { layoutOpen = false; onLayout(option) },
                    )
                }
            }
        }
        // Saved or not is said BY the Save button rather than by a chip beside
        // it, which cost the toolbar its one line on a landscape handheld:
        // "Saved" and disabled when there is nothing to write, "Save" when
        // there is. Both states are local; nothing here depends on a device.
        Button(
            onClick = if (builtin) onSaveAsNew else onSave,
            // Offered for a real edit, and for turning the built-in Default into
            // a profile of the user's own. Never for an unchanged draft: there
            // would be nothing to write.
            enabled = enabled && draft != null && (dirty || builtin),
            modifier = Modifier.semantics {
                stateDescription = if (dirty) "Unsaved changes" else "Saved"
            },
        ) {
            Text(
                when {
                    builtin -> "Save as new"
                    dirty -> "Save"
                    else -> "Saved"
                },
            )
        }
        TextButton(onClick = onDiscard, enabled = enabled && dirty) { Text("Discard") }
    }
}

/**
 * YOUR LIBRARY: the user's own profiles for one layout.
 *
 * NOTHING in this card requires a connection and nothing in it writes to an
 * adapter. New, Duplicate, Rename and Delete are all local, so the whole card
 * stays live with nothing paired. The only button here that reaches the adapter
 * is Assign, and it says so.
 *
 * Each row carries where that profile stands relative to the adapter. That is a
 * RELATIONSHIP, computed by [KbmBankView], not a property of the draft: a single
 * flag could not express "I edited this locally, the adapter still has the old
 * copy, and it is running that old copy".
 */
@Composable
private fun LibraryCard(
    library: KbmProfileLibrary,
    profiles: KbmProfiles,
    layout: KbmProfile,
    draft: KbmLocalDraft?,
    adapterReady: Boolean,
    enabled: Boolean,
    onNew: () -> Unit,
    onDuplicate: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit,
    onAssign: () -> Unit,
) {
    val rows = KbmBankView.library(library, profiles, layout)
    val builtin = draft?.isBuiltin != false

    SectionCard(
        title = "Your profiles",
        icon = Icons.Default.Tune,
        trailing = { TextButton(onClick = onNew, enabled = enabled) { Text("New") } },
    ) {
        rows.forEach { row -> LabelValueRow(row.profile.name, row.stateLabel) }
        if (rows.isEmpty()) {
            InlineNotice(
                "No ${layout.title.lowercase()} profiles yet. New copies the built-in Default.",
            )
        }
        // These act on the open profile, which the toolbar directly above names;
        // a second "Open profile:" line here only repeated it.
        FlowRow(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            TextButton(onClick = onDuplicate, enabled = enabled && draft != null) { Text("Duplicate") }
            TextButton(onClick = onRename, enabled = enabled && !builtin) { Text("Rename") }
            TextButton(onClick = onDelete, enabled = enabled && !builtin) { Text("Delete") }
            // The ONE button in this card that reaches the adapter. Disabled
            // rather than hidden while offline: a user who cannot see it cannot
            // tell that connecting would bring it back.
            FilledTonalButton(
                onClick = onAssign,
                enabled = enabled && adapterReady && !builtin,
            ) { Text("Assign open profile") }
        }
        // Offline, the On adapter card says why Assign is unavailable; saying
        // it here as well cost the tab a row.
    }
}

// ---------------------------------------------------------------------------
// On adapter
// ---------------------------------------------------------------------------

/**
 * ON ADAPTER: the working set, three positions per layout plus Default.
 *
 * Every row here is a live device operation. Empty positions are rows rather
 * than omissions — "Profile 3 · Empty" is what tells a user they have somewhere
 * to assign to, and a list of only the occupied ones would hide the capacity.
 *
 * Activate and On startup are separate buttons because they are separate facts:
 * a switch key moves the runtime choice and not the startup one, so after one
 * press they differ for the rest of the session.
 */
@Composable
private fun BankCard(
    kbm: KbmState,
    layout: KbmProfile,
    adapterReady: Boolean,
    enabled: Boolean,
    onActivate: (Int) -> Unit,
    onBoot: (Int) -> Unit,
    onRemove: (KbmBankSlot) -> Unit,
    onCopy: (Int) -> Unit,
) {
    val slots = KbmBankView.bank(kbm.profiles, kbm.switches, layout)
    val used = slots.count { !it.empty && !it.isDefault }

    SectionCard(title = "On adapter", icon = Icons.Default.Usb) {
        Text(
            if (!adapterReady) {
                "Connect the adapter to see and manage its profiles."
            } else {
                "$used of ${KbmLimits.POSITIONS_PER_LAYOUT} positions used. These work without the app."
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (!adapterReady) return@SectionCard

        slots.forEach { slot ->
            val marks = buildList {
                if (slot.isRuntime) add("active now")
                if (slot.isBoot) add("on startup")
                slot.switchKey?.let { add("key ${it.label}") }
            }

            Column(Modifier.fillMaxWidth()) {
                LabelValueRow(slot.positionLabel, slot.residentLabel)
                if (marks.isNotEmpty()) {
                    Text(
                        marks.joinToString(" · "),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                    TextButton(
                        onClick = { onActivate(slot.position) },
                        // Runtime only: zero flash writes. An empty position has
                        // nothing to activate, and the one already running does
                        // not need it.
                        enabled = enabled && !slot.isRuntime &&
                            (slot.isDefault || !slot.empty),
                    ) { Text("Activate") }
                    TextButton(
                        onClick = { onBoot(slot.position) },
                        // The one profile selection worth a flash write, and the
                        // only reason it is a separate button.
                        enabled = enabled && !slot.isBoot &&
                            (slot.isDefault || !slot.empty),
                    ) { Text("On startup") }
                    if (!slot.isDefault) {
                        TextButton(
                            onClick = { onCopy(slot.position) },
                            enabled = enabled && !slot.empty,
                        ) { Text("Copy to library") }
                        TextButton(
                            onClick = { onRemove(slot) },
                            enabled = enabled && !slot.empty,
                        ) { Text("Remove") }
                    }
                }
            }
        }
    }
}

/**
 * One row per semantic switch action, bound or not.
 *
 * Rendered from the ACTIONS rather than from the adapter's bindings: a list
 * built from the bindings would hide the actions the user has not set up yet,
 * which are exactly the ones they came here to set up.
 */
@Composable
private fun SwitchKeysCard(
    switches: List<KbmSwitchBinding>,
    adapterReady: Boolean,
    enabled: Boolean,
    onChoose: (Int) -> Unit,
    onClear: (KbmSource) -> Unit,
) {
    SectionCard(title = "Profile switch keys", icon = Icons.Default.Keyboard) {
        Text(
            if (adapterReady) {
                "Switch profile without the app. One key works in both layouts."
            } else {
                "Connect the adapter to set up profile switch keys."
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (!adapterReady) return@SectionCard

        KbmBankView.switchActions(switches).forEach { (position, key) ->
            Column(Modifier.fillMaxWidth()) {
                LabelValueRow(KbmPositions.label(position), key?.label ?: "Not assigned")
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                    TextButton(onClick = { onChoose(position) }, enabled = enabled) {
                        Text(if (key == null) "Assign key" else "Change")
                    }
                    if (key != null) {
                        TextButton(onClick = { onClear(key) }, enabled = enabled) {
                            Text("Clear")
                        }
                    }
                }
            }
        }
    }
}

/**
 * Choose which bank position to copy the open profile into.
 *
 * A free position is preselected: filling an empty one is the common case, and
 * replacing an occupied one should be a deliberate choice.
 */
@Composable
private fun AssignDialog(
    name: String,
    slots: List<KbmBankSlot>,
    onDismiss: () -> Unit,
    onConfirm: (Int) -> Unit,
) {
    var chosen by rememberSaveable {
        mutableStateOf(slots.firstOrNull { it.empty }?.position ?: slots.firstOrNull()?.position)
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Assign '$name' to") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                Text(
                    "The adapter keeps its own copy from now on. Assigning does " +
                        "not change what the console is running.",
                    style = MaterialTheme.typography.bodySmall,
                )
                slots.forEach { slot ->
                    SelectableRow(
                        title = slot.positionLabel,
                        supporting = if (slot.empty) {
                            "Empty"
                        } else {
                            "${slot.residentLabel} — will be replaced"
                        },
                        selected = chosen == slot.position,
                        onClick = { chosen = slot.position },
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { chosen?.let(onConfirm) },
                enabled = chosen != null,
            ) { Text("Assign") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/**
 * Choose a physical key for one semantic switch action.
 *
 * Any keyboard usage the model accepts. Function keys are offered first because
 * they are the obvious choice, but nothing is reserved.
 */
@Composable
private fun SwitchKeyDialog(
    position: Int,
    onDismiss: () -> Unit,
    onConfirm: (KbmSource) -> Unit,
) {
    val keys = remember {
        KeyboardKeys.allBindable
            .sortedWith(compareBy({ if (it in 0x3A..0x45) 0 else 1 }, { it }))
            .map { KbmSource(KbmSourceKind.Key, it) }
    }
    var chosen by rememberSaveable { mutableStateOf(keys.first().code) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Key for ${KbmPositions.label(position)}") },
        text = {
            LazyColumn(Modifier.heightIn(max = 360.dp)) {
                items(keys) { source ->
                    SelectableRow(
                        title = source.label,
                        supporting = null,
                        selected = chosen == source.code,
                        onClick = { chosen = source.code },
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(KbmSource(KbmSourceKind.Key, chosen)) },
            ) { Text("Assign") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------

/**
 * The open profile's KEY mapping, as the drawn keyboard.
 *
 * Drawn from the DRAFT, composed locally against the embedded canonical table —
 * not from the adapter's realized mapping. That is what lets every key be
 * rebound with nothing paired. Reading it from the device is what previously
 * made editing require a connection.
 *
 * The keyboard takes whatever height the card has left (see [DrawnKeyboard]).
 * "More keys" SWITCHES the card to the ISO and Japanese keys and any bound key
 * this build does not draw, rather than stacking them under the keyboard: on a
 * landscape handheld there is no height to share, and a keyboard squeezed to
 * make room was a keyboard that scrolled. A binding the user cannot see is one
 * they cannot remove, so the toggle counts the hidden ones.
 *
 * [actions] is null on a short window, where they sit in the toolbar instead.
 */
@Composable
private fun KeyMappingCard(
    layout: KbmProfile,
    draft: KbmLocalDraft?,
    enabled: Boolean,
    moreKeys: Boolean,
    actions: (@Composable () -> Unit)?,
    onEdit: (EditTarget) -> Unit,
    onClear: (KbmSource) -> Unit,
) {
    if (draft == null) return
    val effective = draft.effective.filter { it.source.kind == KbmSourceKind.Key }
    val customCount = effective.count { it.custom }
    val edit: (KbmBinding) -> Unit = { onEdit(EditTarget(layout, it.source, it.destination, it.custom)) }
    val clear: (KbmBinding) -> Unit = { onClear(it.source) }

    val header: @Composable () -> Unit = {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                (if (customCount > 0) "$customCount changed · " else "") +
                    "Tap a key to change it, hold to clear it.",
                Modifier.weight(1f),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            actions?.invoke()
        }
    }
    if (!moreKeys) {
        // The card hugs the keyboard and sits centred in the space, instead
        // of a full-size grey card with the keyboard pinned to its top and
        // the rest empty (owner, 2026-11-08). The key unit is worked out here
        // from the space the card can have, minus its padding and header.
        val short = LocalShortWindow.current
        val pad = LayoutTokens.Space3
        val headerAllowance = (if (actions == null) 24.dp else 52.dp) + (if (short) LayoutTokens.Space2 else LayoutTokens.Space3)
        BoxWithConstraints(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            val unit = DrawnKeyboardSize.unitFor(maxWidth - pad * 2, maxHeight - pad * 2 - headerAllowance)
            val boardWidth = DrawnKeyboardSize.width(unit)
            SectionCard(Modifier.width((boardWidth + pad * 2).coerceAtMost(maxWidth)), contentPadding = pad) {
                header()
                Box(Modifier.size(boardWidth, DrawnKeyboardSize.height(unit))) {
                    DrawnKeyboard(effective, enabled, edit, clear)
                }
            }
        }
        return
    }
    SectionCard(Modifier.fillMaxHeight(), contentPadding = LayoutTokens.Space3) {
        header()
        // The rare case gets the scroll, never the keyboard.
        Column(
            Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        ) {
            OtherKeys(effective, enabled, edit, clear)
            val undrawn = effective.filter {
                it.source.code !in KeyboardGeometry.drawnUsages && it.destination != KbmDestination.None
            }
            if (undrawn.isNotEmpty()) {
                SubsectionLabel("Keys not on this keyboard")
                Column(Modifier.fillMaxWidth()) {
                    undrawn.forEach { binding -> BindingRow(binding, enabled) { edit(binding) } }
                }
            }
        }
    }
}

/** Usages on the always-drawn keyboard; anything else is behind "More keys". */
private val keyboardUsages: Set<Int> =
    KeyboardGeometry.clusters.flatMap { cluster -> cluster.keys.map { it.usage } }.toSet()

/**
 * The Mapping tab's two commands: the More keys toggle and Reset mapping.
 *
 * "Reset mapping" is the one control here that reaches the adapter: it restores
 * the device's own realized mapping and is unrelated to the draft.
 */
@Composable
private fun MappingActions(
    draft: KbmLocalDraft?,
    moreKeys: Boolean,
    onToggleMoreKeys: () -> Unit,
    adapterLive: Boolean,
    onReset: () -> Unit,
) {
    val hiddenBound = draft?.effective?.count {
        it.source.kind == KbmSourceKind.Key && it.source.code !in keyboardUsages &&
            it.destination != KbmDestination.None
    } ?: 0
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
        FilterChip(
            selected = moreKeys,
            onClick = onToggleMoreKeys,
            label = { Text(if (hiddenBound > 0) "More keys ($hiddenBound)" else "More keys") },
        )
        TextButton(onClick = onReset, enabled = adapterLive) { Text("Reset mapping") }
    }
}

/** The Mouse tab's button bindings (keyboard-and-mouse layout only), as tiles like the keys. */
@Composable
private fun MouseButtonsCard(
    layout: KbmProfile,
    draft: KbmLocalDraft?,
    enabled: Boolean,
    onEdit: (EditTarget) -> Unit,
    onClear: (KbmSource) -> Unit,
) {
    if (draft == null) return
    val mice = draft.effective.filter { it.source.kind == KbmSourceKind.MouseButton }
    val customCount = mice.count { it.custom }

    SectionCard(
        title = "Mouse buttons",
        icon = Icons.Default.Mouse,
        trailing = {
            Text(
                if (customCount > 0) "$customCount changed" else "All defaults",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
    ) {
        if (mice.isEmpty()) {
            InlineNotice("This profile has no mouse bindings.")
            return@SectionCard
        }
        Text(
            "Tap to change, hold to clear.",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        MouseButtonTiles(
            mice,
            enabled,
            onEdit = { onEdit(EditTarget(layout, it.source, it.destination, it.custom)) },
            onClear = { onClear(it.source) },
        )
    }
}

/**
 * One `source -> destination` row.
 *
 * Changed rows are marked with a chip as well as weight, because a bold label
 * alone is not a state a screen reader or a low-contrast display conveys.
 */
@Composable
private fun BindingRow(binding: KbmBinding, enabled: Boolean, onClick: () -> Unit) {
    SettingsRow(
        title = binding.source.label,
        enabled = enabled,
        onClick = onClick,
        trailing = {
            if (binding.custom) {
                StatusChip("Changed", tone = ChipTone.Attention)
                Spacer(Modifier.width(LayoutTokens.Space2))
            }
            Text(
                binding.destination.title,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = if (binding.custom) FontWeight.SemiBold else FontWeight.Normal,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Icon(
                Icons.AutoMirrored.Filled.KeyboardArrowRight,
                null,
                Modifier.size(LayoutTokens.IconSize),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
    )
}

/**
 * The focused editor for one input.
 *
 * A searchable, grouped destination list rather than a live "press a key"
 * capture: the management protocol reports the configured bindings but does not
 * stream the input events a capture flow would need, and inventing one would be
 * unsupported protocol behaviour.
 */
@Composable
private fun BindingEditorDialog(
    target: EditTarget,
    onDismiss: () -> Unit,
    onApply: (KbmDestination) -> Unit,
    onRestoreDefault: () -> Unit,
) {
    var query by remember { mutableStateOf("") }
    val groups = remember(query) {
        kbmDestinationGroups.mapNotNull { (group, entries) ->
            val matched = entries.filter {
                query.isBlank() || it.title.contains(query.trim(), ignoreCase = true) ||
                    group.contains(query.trim(), ignoreCase = true)
            }
            if (matched.isEmpty()) null else group to matched
        }
    }
    // Open on the current binding rather than at the top of a thirty-entry
    // list: the first thing a user needs from this dialog is to see what the
    // input does now, and the summary line above cannot show its position.
    val listState = rememberLazyListState()
    LaunchedEffect(target.source) {
        val index = currentDestinationIndex(groups, target.current)
        if (index > 0) runCatching { listState.scrollToItem(index) }
    }

    PicoDialog(
        onDismiss = onDismiss,
        title = target.source.label,
        dismissLabel = "Close",
        confirmLabel = if (target.custom) "Restore default" else null,
        onConfirm = if (target.custom) onRestoreDefault else null,
    ) {
        Text(
            "Currently ${target.current.title} in the ${target.profile.title} profile.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            label = { Text("Find a control") },
            leadingIcon = { Icon(Icons.Default.Search, null) },
        )
        LazyColumn(
            Modifier.fillMaxWidth().heightIn(max = LayoutTokens.DialogListMaxHeight),
            state = listState,
        ) {
            item {
                DestinationChoice(KbmDestination.None, target.current == KbmDestination.None) {
                    onApply(KbmDestination.None)
                }
            }
            groups.forEach { (group, entries) ->
                item { SubsectionLabel(group, Modifier.padding(top = LayoutTokens.Space2)) }
                items(entries, key = { it.wire }) { destination ->
                    DestinationChoice(destination, destination == target.current) { onApply(destination) }
                }
            }
        }
    }
}

/**
 * Lazy-list index of [current] in the grouped destination list.
 *
 * The list is one "Unassigned" row followed by, per group, a heading and its
 * entries, so the index has to be counted rather than looked up. Returns 0 --
 * the top -- when the current destination is filtered out or is Unassigned.
 */
private fun currentDestinationIndex(
    groups: List<Pair<String, List<KbmDestination>>>,
    current: KbmDestination,
): Int {
    if (current == KbmDestination.None) return 0
    var index = 1 // the Unassigned row
    groups.forEach { (_, entries) ->
        index++ // the group heading
        val position = entries.indexOf(current)
        if (position >= 0) return index + position
        index += entries.size
    }
    return 0
}

@Composable
private fun DestinationChoice(destination: KbmDestination, selected: Boolean, onClick: () -> Unit) {
    SettingsRow(
        title = destination.title,
        supporting = if (destination == KbmDestination.None) "This input does nothing" else null,
        onClick = onClick,
        role = Role.RadioButton,
        trailing = { RadioButton(selected = selected, onClick = null) },
    )
}

// ---------------------------------------------------------------------------
// Mouse tuning
// ---------------------------------------------------------------------------

@Composable
private fun MouseTuningCard(
    kbm: KbmState,
    enabled: Boolean,
    advancedOpen: Boolean,
    onToggleAdvanced: () -> Unit,
    onPreview: (KbmMouseField, Int) -> Unit,
    onCommit: (KbmMouseField, Int) -> Unit,
) {
    val mouse = kbm.mouse
    if (!mouse.ranged) {
        SectionCard(title = "Mouse", icon = Icons.Default.Mouse) {
            Text("Reading mouse settings…", style = MaterialTheme.typography.bodySmall)
        }
        return
    }
    // The user's link choice, seeded from whether the axes currently agree.
    var linked by rememberSaveable(mouse.sensitivityX, mouse.sensitivityY) { mutableStateOf(mouse.axesLinked) }
    val active = enabled && kbm.mouseTuningInEffect

    SectionCard(
        title = "Mouse tuning",
        icon = Icons.Default.Mouse,
    ) {
        if (!kbm.mouseTuningInEffect) {
            InlineNotice(
                "Off: the adapter is sending real pointer movement.",
                tone = ChipTone.Attention,
            )
        }

        val feel: @Composable ColumnScope.() -> Unit = {
            // No "Sensitivity" group heading: the linked slider is already labelled
            // that, and two identical words stacked on top of each other read as a
            // rendering fault rather than as structure.
            if (linked) {
                SensitivitySlider(
                    label = "Sensitivity",
                    value = mouse.sensitivityX,
                    config = mouse,
                    enabled = active,
                    onPreview = { onPreview(KbmMouseField.Sensitivity, it) },
                    onCommit = { onCommit(KbmMouseField.Sensitivity, it) },
                )
            } else {
                SubsectionLabel("Sensitivity")
                SensitivitySlider(
                    label = "Horizontal",
                    value = mouse.sensitivityX,
                    config = mouse,
                    enabled = active,
                    onPreview = { onPreview(KbmMouseField.SensitivityX, it) },
                    onCommit = { onCommit(KbmMouseField.SensitivityX, it) },
                )
                SensitivitySlider(
                    label = "Vertical",
                    value = mouse.sensitivityY,
                    config = mouse,
                    enabled = active,
                    onPreview = { onPreview(KbmMouseField.SensitivityY, it) },
                    onCommit = { onCommit(KbmMouseField.SensitivityY, it) },
                )
            }
            SettingsRow(
                title = "Separate axes",
                enabled = enabled,
                onClick = {
                    val next = !linked
                    linked = next
                    // Collapsing back to one control has to make the axes agree, or
                    // the single slider would silently misreport the vertical axis.
                    if (next) onCommit(KbmMouseField.Sensitivity, mouse.sensitivityX)
                },
                role = Role.Switch,
                trailing = {
                    Switch(
                        checked = !linked,
                        onCheckedChange = null,
                        enabled = enabled,
                    )
                },
            )
        }
        val direction: @Composable ColumnScope.() -> Unit = {
            ValueSlider(
                label = "Deadzone compensation",
                value = mouse.antiDeadzone,
                min = 0,
                max = mouse.antiDeadzoneMax,
                enabled = active,
                display = { if (it == 0) "Off" else "$it%" },
                onPreview = { onPreview(KbmMouseField.AntiDeadzone, it) },
                onCommit = { onCommit(KbmMouseField.AntiDeadzone, it) },
            )
            // Two toggles as chips on one row: a full settings row apiece was
            // a row of height each on a landscape handheld.
            FlowRow(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                InvertChip("Invert X", "Invert horizontal", mouse.invertX, enabled) {
                    onCommit(KbmMouseField.InvertX, if (it) 1 else 0)
                }
                InvertChip("Invert Y", "Invert vertical", mouse.invertY, enabled) {
                    onCommit(KbmMouseField.InvertY, if (it) 1 else 0)
                }
            }
            ExpandableSection(
                title = "Advanced",
                summary = "Movement window ${mouse.velocityWindowMs} ms",
                expanded = advancedOpen,
                onToggle = onToggleAdvanced,
            ) {
                ValueSlider(
                    label = "Movement window",
                    value = mouse.velocityWindowMs,
                    min = mouse.velocityWindowMinMs,
                    max = mouse.velocityWindowMaxMs,
                    enabled = active,
                    display = { "$it ms" },
                    onPreview = { onPreview(KbmMouseField.VelocityWindow, it) },
                    onCommit = { onCommit(KbmMouseField.VelocityWindow, it) },
                )
                // Named for what it does now. The wire field is still called
                // `recenterMs` because the stored value, range and direction of
                // effect are unchanged, but it is no longer a recentring delay --
                // it is the reference interval of the velocity model that replaced
                // the old decay. Labelling it "recenter time" would describe
                // behaviour the firmware no longer has.
                Text(
                    "Larger holds more stick deflection; smaller reacts and settles faster.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        // Two columns when the card is wide enough for two sliders side by side:
        // stacked, this card was taller than a landscape handheld's page.
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            if (maxWidth >= TuningTwoColumnWidth) {
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space5)) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), content = feel)
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), content = direction)
                }
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) { feel(); direction() }
            }
        }
    }
}

/** Two slider columns of at least ~220 dp each, plus the gap between them. */
private val TuningTwoColumnWidth = 460.dp

@Composable
private fun InvertChip(label: String, spoken: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    FilterChip(
        selected = checked,
        onClick = { onChange(!checked) },
        enabled = enabled,
        label = { Text(label) },
        leadingIcon = if (checked) { { Icon(Icons.Default.Check, null, Modifier.size(18.dp)) } } else null,
        modifier = Modifier.semantics { role = Role.Switch; contentDescription = spoken },
    )
}

@Composable
private fun SensitivitySlider(
    label: String,
    value: Int,
    config: KbmMouseConfig,
    enabled: Boolean,
    onPreview: (Int) -> Unit,
    onCommit: (Int) -> Unit,
) {
    var position by remember(value) {
        mutableFloatStateOf(SensitivityScale.toPosition(value, config.sensitivityMin, config.sensitivityMax))
    }
    val shown = SensitivityScale.fromPosition(position, config.sensitivityMin, config.sensitivityMax)
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Text(
                "%.2f×".format(config.multiplier(shown)),
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.SemiBold,
            )
        }
        Slider(
            value = position,
            onValueChange = {
                position = it
                onPreview(SensitivityScale.fromPosition(it, config.sensitivityMin, config.sensitivityMax))
            },
            onValueChangeFinished = {
                onCommit(SensitivityScale.fromPosition(position, config.sensitivityMin, config.sensitivityMax))
            },
            valueRange = 0f..1f,
            steps = SensitivityScale.steps(config.sensitivityMin, config.sensitivityMax),
            enabled = enabled,
            modifier = Modifier.semanticsValue("$label ${"%.2f".format(config.multiplier(shown))} times"),
        )
    }
}

/** A plain linear slider over an adapter-reported integer range. */
@Composable
private fun ValueSlider(
    label: String,
    value: Int,
    min: Int,
    max: Int,
    enabled: Boolean,
    display: (Int) -> String,
    onPreview: (Int) -> Unit,
    onCommit: (Int) -> Unit,
) {
    if (max <= min) return
    var current by remember(value) { mutableFloatStateOf(value.coerceIn(min, max).toFloat()) }
    val shown = current.toInt().coerceIn(min, max)
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
            Text(display(shown), style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold)
        }
        Slider(
            value = current,
            onValueChange = { current = it; onPreview(it.toInt().coerceIn(min, max)) },
            onValueChangeFinished = { onCommit(current.toInt().coerceIn(min, max)) },
            valueRange = min.toFloat()..max.toFloat(),
            enabled = enabled,
            modifier = Modifier.semanticsValue("$label ${display(shown)}"),
        )
    }
}

/**
 * Give a slider a spoken value.
 *
 * Compose otherwise announces the raw slider position, which is meaningless for
 * a logarithmic control and wrong for a millisecond one.
 */
private fun Modifier.semanticsValue(description: String): Modifier =
    this.semantics { contentDescription = description }
