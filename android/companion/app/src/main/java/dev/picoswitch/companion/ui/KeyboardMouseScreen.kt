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
import dev.picoswitch.companion.model.*

/**
 * The Keyboard & Mouse management surface.
 *
 * Four conceptual areas, in the order someone actually works through them:
 * what is connected, how the adapter should treat it, what each input does,
 * and how the mouse feels. Arbitration internals the firmware also reports --
 * connection indices, generation counters, rejection tallies -- are not here;
 * they belong to Diagnostics.
 *
 * Every change on this page applies to adapter RAM immediately and is only
 * written to flash by Save. The unsaved marker therefore tracks "changed on
 * this connection", which is the strongest claim the protocol supports.
 */
@Composable
fun KeyboardMouseScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val kbm = ui.kbm
    var editing by remember { mutableStateOf<EditTarget?>(null) }
    var resetAllOpen by rememberSaveable { mutableStateOf(false) }
    var resetProfileOpen by rememberSaveable { mutableStateOf<KbmProfile?>(null) }
    var advancedOpen by rememberSaveable { mutableStateOf(false) }
    var selectedProfile by rememberSaveable { mutableStateOf(kbm.activeProfile) }
    var nameDialog by remember { mutableStateOf<NameDialog?>(null) }
    var deleteOpen by rememberSaveable { mutableStateOf(false) }

    // Follow the adapter when the active layout changes underneath the page
    // (plugging a mouse in switches it), but never fight a deliberate choice
    // made while looking at the other layout.
    LaunchedEffect(kbm.activeProfile) { selectedProfile = kbm.activeProfile }

    // Open this layout's applied profile on arrival, so what is on screen is
    // what the console is using rather than an arbitrary row.
    LaunchedEffect(selectedProfile, kbm.profiles) {
        if (!kbm.profiles.supported) return@LaunchedEffect
        if (kbm.draft?.layout == selectedProfile) return@LaunchedEffect
        val applied = kbm.profiles.activeFor(selectedProfile)?.sourceId
            ?: KbmProfileIds.DEFAULT
        val row = kbm.profiles.forLayout(selectedProfile)
            .firstOrNull { it.id == applied }
            ?: kbm.profiles.forLayout(selectedProfile).first()
        viewModel.openKbmProfile(row)
    }

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
                            enabled = ui.connection.connected && !ui.kbmBusy,
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
                !ui.connection.connected -> EmptyStateBlock(
                    Icons.Default.Keyboard,
                    "Adapter not connected",
                    "Connect to the adapter to manage keyboard and mouse settings.",
                    Modifier.fillMaxSize(),
                )

                // TOP-LEVEL STATE FIRST, and no legacy editor behind it. When the
                // profile contract could not be loaded the screen used to fall
                // through to a pre-profile mapping page, which looked like a
                // half-built feature rather than a failed read.
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

                kbm.readiness != KbmReadiness.Ready -> EmptyStateBlock(
                    Icons.Default.Keyboard,
                    "Not read yet",
                    "Reload to read this adapter's keyboard and mouse settings.",
                    Modifier.fillMaxSize(),
                )

                else -> {
                    val status: @Composable () -> Unit = { KbmStatusCard(kbm, ui, viewModel) }
                    val profiles: @Composable () -> Unit = {
                        ProfilesCard(
                            kbm = kbm,
                            layout = selectedProfile,
                            connected = ui.connection.connected,
                            enabled = !ui.kbmBusy,
                            onOpen = viewModel::openKbmProfile,
                            onSave = viewModel::saveKbmDraft,
                            onDiscard = viewModel::discardKbmDraft,
                            onApply = { viewModel.applyKbmProfile(selectedProfile, it) },
                            onNew = { nameDialog = NameDialog.New },
                            onRename = { nameDialog = NameDialog.Rename },
                            onDelete = { deleteOpen = true },
                        )
                    }
                    val mapping: @Composable () -> Unit = {
                        MappingCard(
                            kbm = kbm,
                            profile = selectedProfile,
                            onProfile = { selectedProfile = it },
                            enabled = !ui.kbmBusy,
                            onEdit = { editing = it },
                            onResetProfile = { resetProfileOpen = it },
                        )
                    }
                    val mouse: @Composable () -> Unit = {
                        MouseTuningCard(
                            kbm = kbm,
                            enabled = !ui.kbmBusy,
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
                            ) { status(); mouse() }
                            Column(
                                Modifier.weight(1f),
                                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
                            ) { profiles(); mapping() }
                        }
                    } else {
                        Column(
                            Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
                        ) {
                            status(); profiles(); mapping(); mouse()
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
                // With a draft open this edits the LOCAL copy and sends nothing.
                // Without one -- firmware with no profile library -- the
                // per-binding command is genuinely the only mapping surface
                // that adapter has.
                if (kbm.draft?.layout == target.profile && destination != null) {
                    viewModel.editKbmBinding(target.source, destination)
                } else {
                    viewModel.bindKbm(target.profile, target.source, destination)
                }
                editing = null
            },
            onRestoreDefault = {
                if (kbm.draft?.layout == target.profile) {
                    viewModel.editKbmBinding(target.source, KbmDestination.None)
                } else {
                    viewModel.bindKbm(target.profile, target.source, null)
                }
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
        body = "This restores the adapter's built-in mouse settings and both mapping profiles. Custom bindings are lost. Settings apply immediately; use Save to keep them.",
        confirmLabel = "Restore defaults",
        destructive = true,
        icon = Icons.Default.RestartAlt,
        onConfirm = { resetAllOpen = false; viewModel.resetKbmAll() },
    )

    resetProfileOpen?.let { profile ->
        ConfirmDialog(
            onDismiss = { resetProfileOpen = null },
            title = "Restore ${profile.title} mapping?",
            body = "This clears every custom binding in the ${profile.title} profile. Mouse settings and the other profile are not affected.",
            confirmLabel = "Restore",
            destructive = true,
            icon = Icons.Default.RestartAlt,
            onConfirm = { resetProfileOpen = null; viewModel.resetKbmProfile(profile) },
        )
    }

    nameDialog?.let { which ->
        val existing = kbm.profiles.forLayout(selectedProfile).map { it.name }
        val draft = kbm.draft
        ProfileNameDialog(
            title = if (which == NameDialog.New) "New profile" else "Rename profile",
            body = when {
                which == NameDialog.Rename -> "Only the name changes. The mapping " +
                    "the console is using is not affected."
                draft?.isBuiltin != false -> "Starts from the built-in Default mapping."
                else -> "Starts as a copy of the profile you are viewing."
            },
            initial = if (which == NameDialog.Rename) draft?.name.orEmpty()
                      else suggestProfileName(existing, "My mapping"),
            taken = existing.filter { which == NameDialog.New || it != draft?.name },
            onDismiss = { nameDialog = null },
            onConfirm = { name ->
                nameDialog = null
                when {
                    which == NameDialog.Rename -> draft?.let {
                        viewModel.renameKbmProfile(it.profileId, name)
                    }
                    // From the built-in template there is nothing stored to
                    // duplicate: save the draft under a new name instead.
                    draft?.isBuiltin != false -> {
                        viewModel.editKbmDraftName(name)
                        viewModel.saveKbmDraft()
                    }
                    else -> viewModel.duplicateKbmProfile(draft.profileId, name)
                }
            },
        )
    }

    if (deleteOpen) {
        val draft = kbm.draft
        val applied =
            kbm.profiles.activeFor(selectedProfile)?.sourceId == draft?.profileId
        ConfirmDialog(
            onDismiss = { deleteOpen = false },
            title = "Delete '${draft?.name.orEmpty()}'?",
            body = if (applied) {
                "This profile is what the console is using. Deleting it switches " +
                    "this layout back to the built-in Default mapping."
            } else {
                "This profile is removed from the adapter. The mapping the console " +
                    "is using does not change."
            },
            confirmLabel = "Delete",
            destructive = true,
            icon = Icons.Default.RestartAlt,
            onConfirm = {
                deleteOpen = false
                draft?.let { viewModel.deleteKbmProfile(it.profileId) }
            },
        )
    }
}

/** A name that is not already taken in this layout. */
private fun suggestProfileName(taken: List<String>, basis: String): String {
    if (taken.none { it.equals(basis, ignoreCase = true) }) return basis
    return (2..99).asSequence()
        .map { "$basis $it" }
        .firstOrNull { candidate -> taken.none { it.equals(candidate, ignoreCase = true) } }
        ?: basis
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

/** Which naming prompt is open. */
private enum class NameDialog { New, Rename }

/** Which input, in which profile, the focused editor is currently editing. */
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
 * The profile library for one layout.
 *
 * Three separate ideas, kept visibly separate because collapsing any two is what
 * made a mapping edit feel like it had silently failed:
 *
 *  - selecting a profile OPENS it. It does not apply it.
 *  - Save stores it in the adapter's library. It does not change the console.
 *  - Set Active is what changes the console.
 *
 * All the rules live in the ViewModel and the draft model; this composable reads
 * state and raises events.
 */
@Composable
private fun ProfilesCard(
    kbm: KbmState,
    layout: KbmProfile,
    connected: Boolean,
    enabled: Boolean,
    onOpen: (KbmProfileInfo) -> Unit,
    onSave: () -> Unit,
    onDiscard: () -> Unit,
    onApply: (Int) -> Unit,
    onNew: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit,
) {
    // Only ever composed in the Ready state, where the library is loaded and
    // Default always exists. An adapter without a profile library does not reach
    // this screen at all: it is reported as needing a firmware update.
    val rows = kbm.profiles.forLayout(layout)
    if (rows.isEmpty()) return

    val draft = kbm.draft?.takeIf { it.layout == layout }
    val state = kbm.draftState(connected)
    // With a draft open the selection is what the draft is editing; without one
    // it is the profile the console is actually running, never nothing.
    val selected = rows.firstOrNull { it.id == draft?.profileId }
        ?: rows.firstOrNull { it.id == kbm.profiles.activeFor(layout)?.sourceId }

    SectionCard(title = "Profile", icon = Icons.Default.Tune) {
        SegmentedSelector(
            options = rows,
            selected = selected ?: rows.first(),
            label = { if (it.builtin) "${it.name} (built-in)" else it.name },
            onSelect = onOpen,
            enabled = enabled,
        )

        val (headline, detail) = when (state) {
            KbmDraftState.Active -> "Active" to null
            KbmDraftState.Dirty -> "Unsaved changes" to
                "Nothing has been sent to the adapter yet. Save to store these " +
                "changes, or Discard to go back to the saved profile."
            KbmDraftState.SavedNotApplied -> "Saved — not applied" to
                "The adapter has these changes saved, but the console is still " +
                "using the mapping that was applied earlier. Set Active to use them."
            KbmDraftState.Conflict -> "Changed on another device" to
                "This profile was changed elsewhere since you opened it. Reload " +
                "to see those changes, or save it as a new profile."
            KbmDraftState.Disconnected -> "Not connected" to
                "Showing the last mapping read from this adapter. What the " +
                "console is using right now cannot be confirmed while disconnected."
            KbmDraftState.Clean -> "Saved" to null
        }

        Text(headline, style = MaterialTheme.typography.titleSmall)
        if (detail != null) InlineNotice(detail)

        val live = connected && enabled
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Button(
                onClick = onSave,
                // Save is offered for a real edit, and for turning the built-in
                // Default into a profile of the user's own.
                enabled = live && (state == KbmDraftState.Dirty || selected?.builtin == true),
            ) { Text("Save") }
            TextButton(
                onClick = onDiscard,
                enabled = live && state == KbmDraftState.Dirty,
            ) { Text("Discard") }
            TextButton(
                onClick = { selected?.let { onApply(it.id) } },
                // Never automatic: applying is the user saying "use this now".
                enabled = live && selected != null &&
                    state != KbmDraftState.Dirty && state != KbmDraftState.Active,
            ) { Text("Set Active") }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            TextButton(onClick = onNew, enabled = live && !kbm.profiles.full) { Text("New") }
            TextButton(
                onClick = onRename,
                enabled = live && selected != null && !selected.builtin,
            ) { Text("Rename") }
            TextButton(
                onClick = onDelete,
                enabled = live && selected != null && !selected.builtin,
            ) { Text("Delete") }
        }

        if (kbm.profiles.full) {
            InlineNotice(
                "All ${kbm.profiles.max} profile slots are in use. Delete one to " +
                    "make room for another.",
            )
        }
    }
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------

@Composable
private fun MappingCard(
    kbm: KbmState,
    profile: KbmProfile,
    onProfile: (KbmProfile) -> Unit,
    enabled: Boolean,
    onEdit: (EditTarget) -> Unit,
    onResetProfile: (KbmProfile) -> Unit,
) {
    val mapping = kbm.mapping(profile)
    var modifiedOnly by rememberSaveable { mutableStateOf(false) }

    SectionCard(
        title = "Mapping",
        icon = Icons.Default.Tune,
        trailing = {
            TextButton(onClick = { onResetProfile(profile) }, enabled = enabled) { Text("Reset") }
        },
    ) {
        SegmentedSelector(
            options = KbmProfile.entries,
            selected = profile,
            label = { it.title },
            onSelect = onProfile,
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                when (profile) {
                    KbmProfile.Keyboard -> "The keyboard drives both sticks."
                    KbmProfile.KeyboardMouse -> "The mouse aims; the keyboard moves and acts."
                },
                Modifier.weight(1f),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            // The two profiles are independent layouts, so which one the adapter
            // is actually running is a distinct fact from which one is on screen.
            if (profile == kbm.activeProfile) {
                Spacer(Modifier.width(LayoutTokens.Space2))
                StatusChip("In use", tone = ChipTone.Positive)
            }
        }

        if (!mapping.loaded) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(LayoutTokens.Space3))
                Text("Reading bindings…", style = MaterialTheme.typography.bodySmall)
            }
            return@SectionCard
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (mapping.customCount > 0) "${mapping.customCount} changed from default"
                else "All defaults",
                Modifier.weight(1f),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            FilterChip(
                selected = modifiedOnly,
                onClick = { modifiedOnly = !modifiedOnly },
                label = { Text("Changed only") },
                enabled = mapping.customCount > 0 || modifiedOnly,
            )
        }

        val keys = mapping.keyBindings.filter { !modifiedOnly || it.custom }
        val mice = mapping.mouseBindings.filter { !modifiedOnly || it.custom }

        if (keys.isEmpty() && mice.isEmpty()) {
            InlineNotice(
                if (modifiedOnly) "No bindings changed. Every input uses the ${profile.title} default."
                else "This profile has no bindings.",
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
                        onEdit(EditTarget(profile, binding.source, binding.destination, binding.custom))
                    }
                }
            }
        }
        if (mice.isNotEmpty()) {
            SubsectionLabel("Mouse buttons")
            Column(Modifier.fillMaxWidth()) {
                mice.forEach { binding ->
                    BindingRow(binding, enabled) {
                        onEdit(EditTarget(profile, binding.source, binding.destination, binding.custom))
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
