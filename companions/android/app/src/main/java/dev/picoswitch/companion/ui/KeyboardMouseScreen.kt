@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
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
import androidx.compose.ui.semantics.semantics
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

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)

        Column(Modifier.fillMaxSize()) {
            ScreenHeader(
                AppSection.Keyboard.title,
                // No subtitle: Save and Refresh take most of a compact header's
                // width, and the leftover column wraps a subtitle onto two lines
                // beside a vertically centred button. The card titles below say
                // what the page contains.
                subtitle = "",
                actions = {
                    if (kbm.dirty) {
                        Button(
                            onClick = viewModel::saveAdapterConfiguration,
                            enabled = adapterLive,
                        ) {
                            Icon(Icons.Default.Save, null, Modifier.size(18.dp))
                            Spacer(Modifier.width(LayoutTokens.Space2))
                            Text("Save")
                        }
                    }
                    IconButton(
                        onClick = viewModel::refreshKbm,
                        enabled = ui.connection.connected && !ui.kbmBusy,
                    ) { Icon(Icons.Default.Refresh, "Refresh keyboard and mouse state") }
                },
            )
            Spacer(Modifier.height(LayoutTokens.Space3))

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
                    val status: @Composable () -> Unit = { KbmStatusCard(kbm, ui, viewModel) }
                    val library: @Composable () -> Unit = {
                        LibraryCard(
                            library = ui.kbmLibrary,
                            profiles = kbm.profiles,
                            layout = layout,
                            draft = draft,
                            selectedId = ui.kbmSelectedLocalId,
                            adapterReady = adapterReady,
                            enabled = !ui.kbmBusy,
                            onLayout = viewModel::selectKbmLayout,
                            onOpen = viewModel::openKbmLocalProfile,
                            onSave = { viewModel.saveKbmProfile() },
                            onSaveAsNew = { nameDialog = NameDialog.SaveAsNew },
                            onDiscard = viewModel::discardKbmDraft,
                            onNew = { nameDialog = NameDialog.New },
                            onDuplicate = { nameDialog = NameDialog.Duplicate },
                            onRename = { nameDialog = NameDialog.Rename },
                            onDelete = { deleteOpen = true },
                            onAssign = { assignOpen = true },
                        )
                    }
                    val mapping: @Composable () -> Unit = {
                        MappingCard(
                            layout = layout,
                            draft = draft,
                            enabled = !ui.kbmBusy,
                            onEdit = { editing = it },
                            onResetProfile = { resetProfileOpen = it },
                            adapterLive = adapterLive,
                        )
                    }
                    val bank: @Composable () -> Unit = {
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
                    }
                    val switches: @Composable () -> Unit = {
                        SwitchKeysCard(
                            switches = kbm.switches,
                            adapterReady = adapterReady,
                            enabled = adapterLive,
                            onChoose = { switchKeyFor = it },
                            onClear = { key -> viewModel.bindKbmSwitch(key, null) },
                        )
                    }
                    val mouse: @Composable () -> Unit = {
                        MouseTuningCard(
                            kbm = kbm,
                            enabled = adapterLive,
                            advancedOpen = advancedOpen,
                            onToggleAdvanced = { advancedOpen = !advancedOpen },
                            onPreview = viewModel::previewMouseField,
                            onCommit = viewModel::commitMouseField,
                            onResetAll = { resetAllOpen = true },
                        )
                    }

                    if (twoColumn) {
                        Row(
                            Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                            horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
                        ) {
                            Column(
                                Modifier.weight(1f),
                                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
                            ) { status(); bank(); switches(); mouse() }
                            Column(
                                Modifier.weight(1f),
                                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
                            ) { library(); mapping() }
                        }
                    } else {
                        Column(
                            Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
                        ) {
                            // Library first on a phone: it is the half that works
                            // whether or not anything is paired.
                            library(); mapping(); bank(); switches(); status(); mouse()
                            Spacer(Modifier.height(LayoutTokens.Space5))
                        }
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

@Composable
private fun KbmStatusCard(kbm: KbmState, ui: CompanionUiState, viewModel: CompanionViewModel) {
    val status = kbm.status
    val sources = ui.snapshot.input.sources
    SectionCard(title = "Devices", icon = Icons.Default.Keyboard) {
        // A device that is simply absent is ordinary operation, not a fault, so
        // it gets a quiet chip rather than a warning banner.
        DeviceStatusRow(
            "Keyboard",
            resolveKbmDeviceName(status.keyboardConnected, status.keyboardConn, sources),
            status.keyboardConnected,
        )
        DeviceStatusRow(
            "Mouse",
            resolveKbmDeviceName(status.mouseConnected, status.mouseConn, sources),
            status.mouseConnected,
        )

        if (!status.anyDeviceConnected) {
            InlineNotice(
                "Pair a Bluetooth keyboard or mouse with the adapter to use these settings. They can join in either order.",
                icon = Icons.Default.Keyboard,
            )
        }

        HorizontalDivider()

        SubsectionLabel("Input mode")
        SegmentedSelector(
            options = KbmMode.entries,
            selected = status.modeOverride,
            label = { it.title },
            onSelect = viewModel::setKbmMode,
            enabled = ui.connection.connected && !ui.kbmBusy,
        )
        Text(
            status.modeOverride.description,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        // The chosen mode and the mode actually in force legitimately differ:
        // Automatic infers from what is connected. Showing only one of them is
        // what made "why is my keyboard not doing anything" unanswerable.
        // Only meaningful under Automatic, where the live mode is inferred from
        // the admitted roles rather than chosen. Under an explicit override the
        // two always agree and a second row would just repeat the selection.
        if (status.modeOverride == KbmMode.Automatic) {
            LabelValueRow("Currently", status.mode.title)
        }
        if (status.nativeMouseOutput) {
            InlineNotice(
                "This adapter mode sends real pointer movement, so mouse tuning below is not in effect.",
                icon = Icons.Default.Usb,
                tone = ChipTone.Attention,
            )
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
@Composable
private fun LibraryCard(
    library: KbmProfileLibrary,
    profiles: KbmProfiles,
    layout: KbmProfile,
    draft: KbmLocalDraft?,
    selectedId: String?,
    adapterReady: Boolean,
    enabled: Boolean,
    onLayout: (KbmProfile) -> Unit,
    onOpen: (String?) -> Unit,
    onSave: () -> Unit,
    onSaveAsNew: () -> Unit,
    onDiscard: () -> Unit,
    onNew: () -> Unit,
    onDuplicate: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit,
    onAssign: () -> Unit,
) {
    val rows = KbmBankView.library(library, profiles, layout)
    val dirty = draft?.dirty == true
    val builtin = draft?.isBuiltin != false

    SectionCard(
        title = "Your library",
        icon = Icons.Default.Tune,
        trailing = {
            TextButton(onClick = onNew, enabled = enabled) { Text("New") }
        },
    ) {
        SegmentedSelector(
            options = KbmProfile.entries,
            selected = layout,
            label = { it.title },
            onSelect = onLayout,
            enabled = enabled,
        )
        Text(
            when (layout) {
                KbmProfile.Keyboard -> "The keyboard drives both sticks."
                KbmProfile.KeyboardMouse -> "The mouse aims; the keyboard moves and acts."
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        HorizontalDivider()

        // Default is always offered and is never a library row: it is the
        // template every profile starts from, it consumes no adapter position,
        // and it cannot be renamed or deleted.
        SelectableRow(
            title = "Default (built-in)",
            supporting = "The adapter's own mapping for this layout.",
            selected = selectedId == null,
            enabled = enabled,
            onClick = { onOpen(null) },
        )

        rows.forEach { row ->
            SelectableRow(
                title = row.profile.name,
                supporting = row.stateLabel,
                selected = selectedId == row.profile.id,
                enabled = enabled,
                onClick = { onOpen(row.profile.id) },
            )
        }

        if (rows.isEmpty()) {
            InlineNotice(
                "No profiles yet. New starts one from the built-in Default; it " +
                    "stays here until you assign it to the adapter.",
            )
        }

        HorizontalDivider()

        // Only two states, and both are local: there is no third that depends on
        // a device. Where the profile stands against the adapter is on its row.
        Text(
            if (dirty) "Unsaved changes" else "Saved to your library",
            style = MaterialTheme.typography.titleSmall,
        )
        if (dirty) {
            InlineNotice(
                if (builtin) {
                    "Default is a template and is never written into. Save keeps " +
                        "these changes as a new profile in your library."
                } else {
                    "Nothing has been sent to the adapter. Save keeps these " +
                        "changes in your library; Discard goes back."
                },
            )
        }

        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Button(
                onClick = if (builtin) onSaveAsNew else onSave,
                // Offered for a real edit, and for turning the built-in Default
                // into a profile of the user's own. Never for an unchanged draft:
                // there would be nothing to write.
                enabled = enabled && draft != null && (dirty || builtin),
            ) { Text(if (builtin) "Save as new" else "Save") }
            TextButton(onClick = onDiscard, enabled = enabled && dirty) { Text("Discard") }
            TextButton(
                onClick = onDuplicate,
                enabled = enabled && draft != null,
            ) { Text("Duplicate") }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            TextButton(onClick = onRename, enabled = enabled && !builtin) { Text("Rename") }
            TextButton(onClick = onDelete, enabled = enabled && !builtin) { Text("Delete") }
            // The ONE button in this card that reaches the adapter. Disabled
            // rather than hidden while offline: a user who cannot see it cannot
            // tell that connecting would bring it back.
            TextButton(
                onClick = onAssign,
                enabled = enabled && adapterReady && !builtin,
            ) { Text("Assign to adapter…") }
        }

        if (!adapterReady) {
            InlineNotice(
                "Connect the adapter to assign profiles to it. Everything else " +
                    "here works offline.",
            )
        }
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
                "$used of ${KbmLimits.POSITIONS_PER_LAYOUT} positions used for " +
                    "${layout.title.lowercase()}. These work with no app connected."
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
                "Change profile without the app. The same key works in both " +
                    "layouts and picks that layout's profile."
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
 * The open profile's mapping.
 *
 * Drawn from the DRAFT, composed locally against the embedded canonical table —
 * not from the adapter's realized mapping. That is what lets the grid render,
 * and every key be rebound, with nothing paired. Reading it from the device is
 * what previously made editing require a connection.
 *
 * "Reset" is the one control here that reaches the adapter: it restores the
 * device's own realized mapping and is unrelated to the draft.
 */
@Composable
private fun MappingCard(
    layout: KbmProfile,
    draft: KbmLocalDraft?,
    enabled: Boolean,
    adapterLive: Boolean,
    onEdit: (EditTarget) -> Unit,
    onResetProfile: (KbmProfile) -> Unit,
) {
    if (draft == null) return
    var modifiedOnly by rememberSaveable { mutableStateOf(false) }

    val effective = draft.effective
    val customCount = effective.count { it.custom }

    SectionCard(
        title = "Mapping",
        icon = Icons.Default.Tune,
        trailing = {
            TextButton(
                onClick = { onResetProfile(layout) },
                enabled = adapterLive,
            ) { Text("Reset adapter") }
        },
    ) {
        Text(
            draft.name,
            style = MaterialTheme.typography.titleSmall,
        )

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (customCount > 0) "$customCount changed from default" else "All defaults",
                Modifier.weight(1f),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            FilterChip(
                selected = modifiedOnly,
                onClick = { modifiedOnly = !modifiedOnly },
                label = { Text("Changed only") },
                enabled = customCount > 0 || modifiedOnly,
            )
        }

        val keys = effective.filter { it.source.kind == KbmSourceKind.Key }
            .filter { !modifiedOnly || it.custom }
        val mice = effective.filter { it.source.kind == KbmSourceKind.MouseButton }
            .filter { !modifiedOnly || it.custom }

        if (keys.isEmpty() && mice.isEmpty()) {
            InlineNotice(
                if (modifiedOnly) {
                    "No bindings changed. Every input uses the ${layout.title} default."
                } else {
                    "This profile has no bindings."
                },
            )
            return@SectionCard
        }

        // The rows carry their own 48dp touch target, so the card's section gap
        // is dropped between them. On a 26-binding profile that gap alone was
        // most of an extra screen of scrolling.
        if (keys.isNotEmpty()) {
            SubsectionLabel("Keyboard")
            Column(Modifier.fillMaxWidth()) {
                keys.forEach { binding ->
                    BindingRow(binding, enabled) {
                        onEdit(EditTarget(layout, binding.source, binding.destination, binding.custom))
                    }
                }
            }
        }
        if (mice.isNotEmpty()) {
            SubsectionLabel("Mouse buttons")
            Column(Modifier.fillMaxWidth()) {
                mice.forEach { binding ->
                    BindingRow(binding, enabled) {
                        onEdit(EditTarget(layout, binding.source, binding.destination, binding.custom))
                    }
                }
            }
        }
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
    onResetAll: () -> Unit,
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
        title = "Mouse",
        icon = Icons.Default.Mouse,
        trailing = { TextButton(onClick = onResetAll, enabled = enabled) { Text("Defaults") } },
    ) {
        if (!kbm.mouseTuningInEffect) {
            InlineNotice(
                "Not in effect while the adapter reports native pointer movement.",
                tone = ChipTone.Attention,
            )
        }

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
            title = "Separate horizontal and vertical",
            supporting = if (linked) "Both axes use one value" else "Each axis is tuned on its own",
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

        HorizontalDivider()

        SubsectionLabel("Deadzone compensation")
        ValueSlider(
            label = "Compensation",
            value = mouse.antiDeadzone,
            min = 0,
            max = mouse.antiDeadzoneMax,
            enabled = active,
            display = { if (it == 0) "Off" else "$it%" },
            onPreview = { onPreview(KbmMouseField.AntiDeadzone, it) },
            onCommit = { onCommit(KbmMouseField.AntiDeadzone, it) },
        )
        Text(
            "Raises very small translated stick movement so games with controller deadzones still respond. Use the lowest value that makes slow movement reliable — too much makes tiny movement feel abrupt.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        HorizontalDivider()

        SubsectionLabel("Direction")
        ToggleRow("Invert horizontal", mouse.invertX, enabled) {
            onCommit(KbmMouseField.InvertX, if (it) 1 else 0)
        }
        ToggleRow("Invert vertical", mouse.invertY, enabled) {
            onCommit(KbmMouseField.InvertY, if (it) 1 else 0)
        }

        HorizontalDivider()

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
                "How much of your movement the stick holds. Larger keeps more deflection for the same mouse speed; smaller makes it react and settle faster.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ToggleRow(title: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    SettingsRow(
        title = title,
        enabled = enabled,
        onClick = { onChange(!checked) },
        role = Role.Switch,
        trailing = { Switch(checked = checked, onCheckedChange = null, enabled = enabled) },
    )
}

/**
 * Sensitivity on a logarithmic slider.
 *
 * The adapter accepts a 512:1 range, so a linear control cannot resolve the
 * useful low end at all. The position is logarithmic and the exact value is
 * always shown as the multiplier the firmware applies, so the number stays
 * meaningful even though the travel is not proportional to it.
 */
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
