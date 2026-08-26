package dev.picoswitch.companion.ui.touch

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowColumn
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeContent
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AddBox
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.FormatAlignCenter
import androidx.compose.material.icons.filled.GridOn
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.OpenWith
import androidx.compose.material.icons.filled.Restore
import androidx.compose.material.icons.filled.ZoomIn
import androidx.compose.material.icons.filled.ZoomOut
import androidx.compose.material.icons.filled.SelectAll
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import kotlin.math.ceil
import dev.picoswitch.bridge.touch.TouchControllerProfile
import dev.picoswitch.bridge.touch.TouchLayoutOverride
import dev.picoswitch.bridge.touch.TouchLayoutProfile
import dev.picoswitch.bridge.touch.TouchProfileLibrary
import dev.picoswitch.bridge.touch.TouchTemplateControl
import dev.picoswitch.bridge.touch.supportsLatch
import dev.picoswitch.companion.data.TouchEditorDock

/**
 * What the layout editor's floating chrome can be asked to do.
 *
 * Modelled as one type rather than a dozen lambdas because the toolbar, the
 * contextual bar and the dialogs all act on the SAME editor and would otherwise
 * each grow their own parameter list. A host screen implements this once.
 */
internal interface TouchEditorActions {
    fun setEditGroup(value: Boolean)
    fun setGrid(value: Boolean)
    fun setSnap(value: Boolean)
    fun setDock(dock: TouchEditorDock)
    fun nudgeScale(factor: Float)
    fun setVisible(visible: Boolean)
    /** `null` restores "follow the global Lock a button held setting". */
    fun setLatch(latch: Boolean?)
    fun resetSelection()
    fun resetProfile()
    fun openProfiles()
    fun openAddControl()
    fun save()
    fun exit()
}

/**
 * The editor's floating chrome.
 *
 * The controls stay visible and the chrome stays out of the way: layout editing
 * is a SPATIAL task, and a panel covering the surface forces the user to map
 * editor widgets onto a layout they cannot see. Everything here is sized to the
 * smallest thing that can still be pressed reliably, and docks to whichever edge
 * suits the device in the user's hands.
 *
 * ```text
 *  dock = Bottom            dock = Right
 *  +---------------+        +---------------+
 *  |               |        |          [sel]|
 *  |   controls    |        | controls  [::]|
 *  | [selection]   |        |           [::]|
 *  | [tool bar]    |        |           [::]|
 *  +---------------+        +---------------+
 * ```
 */
@Composable
internal fun BoxScope.TouchEditorChrome(
    profile: TouchControllerProfile,
    draft: TouchLayoutOverride,
    selection: Set<String>,
    effectiveTargets: Set<String>,
    primaryId: String?,
    dock: TouchEditorDock,
    editGroup: Boolean,
    grid: Boolean,
    snap: Boolean,
    profileName: String,
    canSave: Boolean,
    blockingProblem: String?,
    dirty: Boolean,
    manipulating: Boolean,
    actions: TouchEditorActions,
) {
    val alignment = when (dock) {
        TouchEditorDock.Bottom -> Alignment.BottomCenter
        TouchEditorDock.Top -> Alignment.TopCenter
        TouchEditorDock.Left -> Alignment.CenterStart
        TouchEditorDock.Right -> Alignment.CenterEnd
    }
    // Get out of the way while a control is actually being moved or resized.
    // The chrome is docked to an edge and a layout uses its edges; without this,
    // the one control the user is manipulating is the one they cannot see.
    val chromeAlpha by animateFloatAsState(
        targetValue = if (manipulating) MANIPULATING_ALPHA else 1f,
        label = "editorChromeAlpha",
    )
    val modifier = Modifier
        .align(alignment)
        .alpha(chromeAlpha)
        .windowInsetsPadding(WindowInsets.safeContent)
        .padding(6.dp)
    // The contextual bar sits between the toolbar and the controls, so the
    // toolbar itself never moves when a selection appears or disappears.
    val toolbarFirst = dock == TouchEditorDock.Top || dock == TouchEditorDock.Left

    @Composable
    fun content() {
        val problem = @Composable {
            blockingProblem?.let { message ->
                Surface(
                    shape = MaterialTheme.shapes.large,
                    color = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    tonalElevation = 3.dp,
                ) {
                    Text(
                        message,
                        Modifier.widthIn(max = 420.dp).padding(horizontal = 12.dp, vertical = 6.dp),
                        style = MaterialTheme.typography.labelMedium,
                    )
                }
            }
        }
        val bar = @Composable {
            TouchEditorSelectionBar(
                profile = profile,
                draft = draft,
                effectiveTargets = effectiveTargets,
                primaryId = primaryId,
                vertical = dock.vertical,
                actions = actions,
            )
        }
        val toolbar = @Composable {
            TouchEditorToolbar(
                dock = dock,
                editGroup = editGroup,
                groupAvailable = hasGroup(profile, selection),
                grid = grid,
                snap = snap,
                profileName = profileName,
                canSave = canSave && dirty,
                dirty = dirty,
                actions = actions,
            )
        }
        if (toolbarFirst) { toolbar(); bar(); problem() } else { problem(); bar(); toolbar() }
    }

    if (dock.vertical) {
        Row(
            modifier,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) { content() }
    } else {
        Column(
            modifier,
            verticalArrangement = Arrangement.spacedBy(6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) { content() }
    }
}

/**
 * The small floating toolbar.
 *
 * Icon-only on purpose. The toolbar has to be reachable without covering the
 * layout, and the layout is the thing being judged; a labelled button row wide
 * enough to read would be the modal panel this design exists to remove. Every
 * button carries a content description, so the labels are still there for
 * accessibility services and long-press tooltips.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TouchEditorToolbar(
    dock: TouchEditorDock,
    editGroup: Boolean,
    groupAvailable: Boolean,
    grid: Boolean,
    snap: Boolean,
    profileName: String,
    canSave: Boolean,
    dirty: Boolean,
    actions: TouchEditorActions,
) {
    var dockMenuOpen by remember { mutableStateOf(false) }
    Surface(
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        contentColor = MaterialTheme.colorScheme.onSurface,
        tonalElevation = 6.dp,
        shadowElevation = 6.dp,
    ) {
        val items = @Composable {
            EditorIcon(Icons.Default.Layers, "Layout profile: $profileName", actions::openProfiles)
            EditorIcon(Icons.Default.AddBox, "Add a hidden control back", actions::openAddControl)
            EditorIcon(
                Icons.Default.SelectAll,
                if (editGroup) "Editing whole groups" else "Editing single controls",
                { actions.setEditGroup(!editGroup) },
                active = editGroup,
                enabled = groupAvailable,
            )
            EditorIcon(
                Icons.Default.GridOn,
                if (grid) "Hide the alignment grid" else "Show the alignment grid",
                { actions.setGrid(!grid) },
                active = grid,
            )
            EditorIcon(
                Icons.Default.FormatAlignCenter,
                if (snap) "Snapping on" else "Snapping off",
                { actions.setSnap(!snap) },
                active = snap,
            )
            EditorIcon(Icons.Default.Restore, "Reset this whole profile", actions::resetProfile)
            Box {
                EditorIcon(Icons.Default.OpenWith, "Move the toolbar", { dockMenuOpen = true })
                DropdownMenu(dockMenuOpen, onDismissRequest = { dockMenuOpen = false }) {
                    TouchEditorDock.entries.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option.title) },
                            onClick = { actions.setDock(option); dockMenuOpen = false },
                            trailingIcon = {
                                if (option == dock) Icon(Icons.Default.Check, null)
                            },
                        )
                    }
                }
            }
            EditorIcon(
                Icons.Default.Check,
                if (dirty) "Save the layout" else "Layout saved",
                actions::save,
                enabled = canSave,
                emphasised = true,
            )
            EditorIcon(Icons.Default.Close, "Leave edit mode", actions::exit)
        }
        EditorButtonFlow(vertical = dock.vertical, count = TOOLBAR_BUTTONS, content = items)
    }
}

/**
 * Lay a fixed set of editor buttons out in as few lines as will fit, evenly.
 *
 * Wrapping, not scrolling: a toolbar tall enough to push Save off the bottom of
 * a phone in landscape is a toolbar with no Save button, and a scrolling one
 * hides the same button behind a gesture nobody looks for there. The line count
 * is computed rather than fixed so the balance is even — plain flow would fill
 * one line to capacity and leave two buttons stranded in the next.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun EditorButtonFlow(vertical: Boolean, count: Int, content: @Composable () -> Unit) {
    BoxWithConstraints(Modifier.padding(3.dp)) {
        val extent = if (vertical) maxHeight else maxWidth
        val perLine = (extent / TOOLBAR_TARGET).toInt().coerceIn(1, count)
        val lines = ceil(count.toFloat() / perLine).toInt().coerceAtLeast(1)
        val balanced = ceil(count.toFloat() / lines).toInt().coerceAtLeast(1)
        // Lines start together rather than each centring itself: a short final
        // line then reads as one empty cell in a grid instead of a stagger.
        if (vertical) {
            FlowColumn(
                maxItemsInEachColumn = balanced,
                verticalArrangement = Arrangement.Top,
                horizontalArrangement = Arrangement.Start,
            ) { content() }
        } else {
            FlowRow(
                maxItemsInEachRow = balanced,
                horizontalArrangement = Arrangement.Start,
                verticalArrangement = Arrangement.Top,
            ) { content() }
        }
    }
}

/**
 * Contextual actions for whatever is selected.
 *
 * Present only when something is; the rest of the time the same space carries
 * the one sentence that explains how selection works, because a direct
 * manipulation editor with an undiscoverable long-press is a direct
 * manipulation editor with no multi-select.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TouchEditorSelectionBar(
    profile: TouchControllerProfile,
    draft: TouchLayoutOverride,
    effectiveTargets: Set<String>,
    primaryId: String?,
    vertical: Boolean,
    actions: TouchEditorActions,
) {
    val primary = profile.defaultTemplate.controls.firstOrNull { it.id == primaryId }
    Surface(
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        contentColor = MaterialTheme.colorScheme.onSurface,
        tonalElevation = 3.dp,
        shadowElevation = 3.dp,
    ) {
        if (primary == null) {
            Text(
                if (vertical) "Tap a control" else SELECTION_HINT,
                Modifier.widthIn(max = 460.dp).padding(horizontal = 12.dp, vertical = 8.dp),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@Surface
        }
        val visible = effectiveTargets.none { draft.controls[it]?.visible == false }
        val items = @Composable {
            // Boxed to the same height as the buttons so it centres in a row and
            // in a column without needing either layout's own alignment scope.
            Box(
                Modifier.heightIn(min = TOOLBAR_TARGET),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    describeSelection(primary, effectiveTargets),
                    Modifier.padding(horizontal = 8.dp),
                    style = MaterialTheme.typography.labelLarge,
                    maxLines = 1,
                )
            }
            EditorIcon(Icons.Default.ZoomOut, "Make smaller", { actions.nudgeScale(1f / SCALE_STEP) })
            EditorIcon(Icons.Default.ZoomIn, "Make larger", { actions.nudgeScale(SCALE_STEP) })
            EditorIcon(
                if (visible) Icons.Default.Visibility else Icons.Default.VisibilityOff,
                if (visible) "Hide this control" else "Show this control",
                { actions.setVisible(!visible) },
            )
            TouchLatchMenu(
                profile = profile,
                draft = draft,
                effectiveTargets = effectiveTargets,
                onSelect = actions::setLatch,
            )
            EditorIcon(Icons.Default.Restore, "Reset this selection", actions::resetSelection)
        }
        EditorButtonFlow(vertical = vertical, count = SELECTION_BAR_ITEMS, content = items)
    }
}

/**
 * Hold-to-latch support for the selected controls.
 *
 * A three-way choice on one toolbar button rather than another panel: the
 * property is per control, and a control is chosen by touching it on the layout,
 * so the setting belongs beside the other per-control actions. Tri-state is
 * shown as a menu because a cycling icon button cannot say what its third state
 * is before you have pressed it twice.
 *
 * Disabled outright for a selection that contains nothing latchable — a stick
 * and the D-pad have no single state to hold — so the menu never offers a
 * setting that would be silently dropped.
 */
@Composable
private fun TouchLatchMenu(
    profile: TouchControllerProfile,
    draft: TouchLayoutOverride,
    effectiveTargets: Set<String>,
    onSelect: (Boolean?) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    val latchable = profile.defaultTemplate.controls
        .filter { it.id in effectiveTargets && it.interaction.supportsLatch }
    // One answer only when every latchable target agrees; a mixed selection has
    // no current value to check, and claiming one would be a lie.
    val values = latchable.mapTo(mutableSetOf()) { draft.controls[it.id]?.latch }
    val current = values.singleOrNull()
    Box {
        EditorIcon(
            icon = if (current == false) Icons.Default.LockOpen else Icons.Default.Lock,
            description = "Lock a button held: " + when {
                latchable.isEmpty() -> "not available for this control"
                current == true -> "on"
                current == false -> "off"
                current == null && values.size == 1 -> "default"
                else -> "mixed"
            },
            onClick = { open = true },
            active = current == true,
            enabled = latchable.isNotEmpty(),
        )
        DropdownMenu(open, onDismissRequest = { open = false }) {
            Text(
                "Lock a button held",
                Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            LATCH_CHOICES.forEach { (value, title) ->
                DropdownMenuItem(
                    text = { Text(title) },
                    onClick = { onSelect(value); open = false },
                    trailingIcon = {
                        if (values.size == 1 && value == current) Icon(Icons.Default.Check, null)
                    },
                )
            }
        }
    }
}

@Composable
private fun EditorIcon(
    icon: ImageVector,
    description: String,
    onClick: () -> Unit,
    active: Boolean = false,
    enabled: Boolean = true,
    emphasised: Boolean = false,
) {
    if (emphasised) {
        FilledIconButton(
            onClick = onClick,
            enabled = enabled,
            modifier = Modifier.size(TOOLBAR_TARGET),
        ) { Icon(icon, description, Modifier.size(20.dp)) }
        return
    }
    IconButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier.size(TOOLBAR_TARGET),
        colors = if (active) {
            IconButtonDefaults.iconButtonColors(
                contentColor = MaterialTheme.colorScheme.primary,
            )
        } else {
            IconButtonDefaults.iconButtonColors(contentColor = LocalContentColor.current)
        },
    ) { Icon(icon, description, Modifier.size(20.dp)) }
}

/**
 * Restore a control the user hid.
 *
 * "Add control" in a system whose templates are immutable can only mean this.
 * Inventing a control outside the shipped template would create a second layout
 * representation and a control with no fixed binding on the emulated
 * personality — which the layout audit refuses, correctly.
 */
@Composable
internal fun TouchAddControlDialog(
    profile: TouchControllerProfile,
    draft: TouchLayoutOverride,
    onRestore: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val hidden = profile.defaultTemplate.controls.filter { draft.controls[it.id]?.visible == false }
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done") } },
        title = { Text("Hidden controls") },
        text = {
            if (hidden.isEmpty()) {
                Text(
                    "Every ${profile.displayName} control is on screen. Hide one with the " +
                        "eye button to bring it back here.",
                    style = MaterialTheme.typography.bodyMedium,
                )
            } else {
                Column(
                    Modifier.verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    hidden.forEach { control ->
                        Row(
                            Modifier.fillMaxWidth().heightIn(min = 48.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(controlTitle(control), Modifier.weight(1f))
                            OutlinedButton(onClick = { onRestore(control.id) }) { Text("Add back") }
                        }
                    }
                }
            }
        },
    )
}

/**
 * The profile picker and the profile lifecycle in one place.
 *
 * The factory profile is shown with the others but offers only the actions that
 * cannot damage it — use it, or copy it. That protection is structural: the
 * library synthesizes this entry rather than reading it from storage, so no
 * stored document can rename, overwrite or delete it.
 */
@Composable
internal fun TouchProfileDialog(
    library: TouchProfileLibrary,
    onSelect: (String) -> Unit,
    onCreate: () -> Unit,
    onDuplicate: (String) -> Unit,
    onRename: (TouchLayoutProfile) -> Unit,
    onDelete: (String) -> Unit,
    onReset: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done") } },
        dismissButton = {
            TextButton(
                onClick = onCreate,
                enabled = library.userProfiles.size < TouchProfileLibrary.MAX_USER_PROFILES,
            ) { Text("New") }
        },
        title = { Text("Layout profiles") },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Text(
                    "Profiles belong to this controller. ${library.factoryProfile.name} is the " +
                        "shipped layout and can always be returned to.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                library.profiles.forEach { entry ->
                    TouchProfileRow(
                        entry = entry,
                        selected = entry.id == library.selectedProfileId,
                        onSelect = { onSelect(entry.id) },
                        onDuplicate = { onDuplicate(entry.id) },
                        onRename = { onRename(entry) },
                        onDelete = { onDelete(entry.id) },
                        onReset = { onReset(entry.id) },
                    )
                }
            }
        },
    )
}

/**
 * One profile.
 *
 * The lifecycle actions live behind an overflow menu rather than inline: a
 * dialog in landscape on a phone is a narrow column, and four inline icon
 * buttons leave the profile's own name a two-character-wide ribbon.
 */
@Composable
private fun TouchProfileRow(
    entry: TouchLayoutProfile,
    selected: Boolean,
    onSelect: () -> Unit,
    onDuplicate: () -> Unit,
    onRename: () -> Unit,
    onDelete: () -> Unit,
    onReset: () -> Unit,
) {
    var menuOpen by remember { mutableStateOf(false) }
    Row(
        Modifier.fillMaxWidth().heightIn(min = 48.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = onSelect)
        Column(Modifier.weight(1f)) {
            Text(entry.name, style = MaterialTheme.typography.bodyLarge)
            Text(
                when {
                    entry.isFactory -> "Shipped layout, protected"
                    entry.isPristine -> "No changes yet"
                    else -> "${entry.override.controls.size} adjusted control(s)"
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Box {
            EditorIcon(Icons.Default.MoreVert, "Actions for ${entry.name}", { menuOpen = true })
            DropdownMenu(menuOpen, onDismissRequest = { menuOpen = false }) {
                DropdownMenuItem(
                    text = { Text("Duplicate") },
                    leadingIcon = { Icon(Icons.Default.ContentCopy, null) },
                    onClick = { menuOpen = false; onDuplicate() },
                )
                if (!entry.isFactory) {
                    DropdownMenuItem(
                        text = { Text("Rename") },
                        leadingIcon = { Icon(Icons.Default.Edit, null) },
                        onClick = { menuOpen = false; onRename() },
                    )
                    DropdownMenuItem(
                        text = { Text("Reset to default") },
                        leadingIcon = { Icon(Icons.Default.Restore, null) },
                        onClick = { menuOpen = false; onReset() },
                    )
                    DropdownMenuItem(
                        text = { Text("Delete") },
                        leadingIcon = { Icon(Icons.Default.Delete, null) },
                        onClick = { menuOpen = false; onDelete() },
                    )
                }
            }
        }
    }
}

/** One text field, used for creating, renaming and saving-as. */
@Composable
internal fun TouchProfileNameDialog(
    title: String,
    explanation: String?,
    initial: String,
    confirmLabel: String,
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var value by remember(initial) { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                explanation?.let {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                OutlinedTextField(
                    value = value,
                    onValueChange = {
                        value = it.take(TouchProfileLibrary.MAX_NAME_LENGTH)
                    },
                    singleLine = true,
                    label = { Text("Name") },
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    modifier = Modifier.semantics { contentDescription = "Layout profile name" },
                )
            }
        },
        confirmButton = {
            Button(onClick = { onConfirm(value) }, enabled = value.isNotBlank()) {
                Text(confirmLabel)
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/**
 * The one thing an editor must never do silently: throw away an edit.
 *
 * Offered on every path out of a dirty draft — leaving the mode, and switching
 * to another profile — because both discard the same work.
 */
@Composable
internal fun TouchUnsavedChangesDialog(
    canSave: Boolean,
    onSave: () -> Unit,
    onDiscard: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Keep your layout changes?") },
        text = {
            Text(
                if (canSave) "This layout has unsaved changes."
                else "This layout has unsaved changes, and cannot be saved as it is now.",
            )
        },
        confirmButton = {
            Button(onClick = onSave, enabled = canSave) { Text("Save") }
        },
        dismissButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onDismiss) { Text("Keep editing") }
                TextButton(onClick = onDiscard) { Text("Discard") }
            }
        },
    )
}

private fun hasGroup(profile: TouchControllerProfile, selection: Set<String>): Boolean =
    profile.defaultTemplate.controls.any { it.id in selection && it.editGroupId != null }

/**
 * The selection's name, counted the way it is DRAWN.
 *
 * The count comes from the effective targets rather than the tapped ids, so it
 * always matches the number of highlighted outlines. Reporting the tapped count
 * would say "+1" while three controls are outlined and three controls move.
 */
private fun describeSelection(
    primary: TouchTemplateControl,
    effectiveTargets: Set<String>,
): String {
    val name = controlTitle(primary)
    val extra = effectiveTargets.size - 1
    return if (extra > 0) "$name +$extra" else name
}

/** A control's user-facing name: its legend if it has one, otherwise its stable id. */
internal fun controlTitle(control: TouchTemplateControl): String =
    control.visual.label.ifBlank {
        control.id.replace('-', ' ').replaceFirstChar { it.uppercase() }
    }

/** Same target size as every other primary control in the app. */
private val TOOLBAR_TARGET = 48.dp

/** Kept beside the toolbar's own content so the wrap arithmetic cannot drift. */
private const val TOOLBAR_BUTTONS = 9

/** Name plus smaller, larger, visibility, hold-to-latch and reset. */
private const val SELECTION_BAR_ITEMS = 6

/**
 * Ordered so "Default" is first: it is what an untouched control already does,
 * and it is the answer a user comes back to the menu to restore.
 */
private val LATCH_CHOICES: List<Pair<Boolean?, String>> = listOf(
    null to "Default",
    true to "Enabled",
    false to "Disabled",
)

/** One press of `+` or `-`; small enough to tune, large enough to feel. */
private const val SCALE_STEP = 1.06f

private const val SELECTION_HINT =
    "Tap to select \u00b7 drag to move \u00b7 pinch to resize \u00b7 long-press to add another"

/** Faded, not hidden: the chrome has to come back the instant a finger lifts. */
private const val MANIPULATING_ALPHA = 0.18f
