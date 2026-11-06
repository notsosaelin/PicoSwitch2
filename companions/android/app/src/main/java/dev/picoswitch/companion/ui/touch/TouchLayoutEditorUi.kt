package dev.picoswitch.companion.ui.touch

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowColumn
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Redo
import androidx.compose.material.icons.automirrored.filled.RotateLeft
import androidx.compose.material.icons.automirrored.filled.RotateRight
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.filled.AddBox
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DragIndicator
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.FlipToBack
import androidx.compose.material.icons.filled.FlipToFront
import androidx.compose.material.icons.filled.FormatAlignCenter
import androidx.compose.material.icons.filled.GridOn
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Restore
import androidx.compose.material.icons.filled.SelectAll
import androidx.compose.material.icons.filled.Workspaces
import androidx.compose.material.icons.filled.ZoomIn
import androidx.compose.material.icons.filled.ZoomOut
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.PlainTooltip
import androidx.compose.material3.TooltipBox
import androidx.compose.material3.TooltipDefaults
import androidx.compose.material3.rememberTooltipState
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.layout.positionInParent
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import kotlin.math.ceil
import kotlin.math.roundToInt
import dev.picoswitch.bridge.touch.TouchControlCategory
import dev.picoswitch.bridge.touch.TouchControlNaming
import dev.picoswitch.bridge.touch.TouchControllerProfile
import dev.picoswitch.bridge.touch.TouchLayoutDocument
import dev.picoswitch.bridge.touch.TouchLayoutEditor
import dev.picoswitch.bridge.touch.TouchLayoutProfile
import dev.picoswitch.bridge.touch.TouchLayoutRegion
import dev.picoswitch.bridge.touch.TouchProfileLibrary
import dev.picoswitch.bridge.touch.TouchTemplateControl
import dev.picoswitch.bridge.touch.TouchToolbarEdge
import dev.picoswitch.bridge.touch.TouchToolbarLayout
import dev.picoswitch.bridge.touch.TouchToolbarPlacement
import dev.picoswitch.bridge.touch.supportsLatch

/**
 * What the layout editor's floating chrome can be asked to do.
 *
 * Modelled as one type rather than three dozen lambdas because the toolbar, the
 * inspector and the dialogs all act on the SAME editor and would otherwise each
 * grow their own parameter list. A host screen implements this once.
 */
internal interface TouchEditorActions {
    fun setEditGroup(value: Boolean)
    fun setMultiSelect(value: Boolean)
    fun setGrid(value: Boolean)
    fun setSnap(value: Boolean)
    fun setToolbar(placement: TouchToolbarPlacement)
    fun nudgeScale(factor: Float)
    fun nudgeRotation(degrees: Float)
    fun resetRotation()
    fun duplicate()
    fun delete()
    fun group()
    fun ungroup()
    fun bringForward()
    fun sendBackward()
    fun undo()
    fun redo()
    /** `null` restores "follow the global Lock a button held setting". */
    fun setLatch(latch: Boolean?)
    fun resetSelection()
    fun resetProfile()
    fun openProfiles()
    fun openAddControl()
    fun openInspector()
    fun preview()
    fun save()
    fun exit()
}

/** Everything the chrome needs to draw itself, gathered so the call site stays readable. */
internal data class TouchEditorUiState(
    val profile: TouchControllerProfile,
    val document: TouchLayoutDocument,
    val selection: Set<String>,
    val effectiveTargets: Set<String>,
    val primaryId: String?,
    val placement: TouchToolbarPlacement,
    val editGroup: Boolean,
    val multiSelect: Boolean,
    val grid: Boolean,
    val snap: Boolean,
    val profileName: String,
    val canSave: Boolean,
    val canUndo: Boolean,
    val canRedo: Boolean,
    /**
     * Why the SELECTED control does not fit, if it does not.
     *
     * Only ever shown inside the menu, beneath the name of the control it is
     * about. The primary warning is the red outline on the canvas — a sentence
     * in the toolbar was the old behaviour, and it both clipped off the end of
     * the bar and left the user hunting for which control it meant.
     */
    val selectionProblem: String?,
    val dirty: Boolean,
    val manipulating: Boolean,
)

/**
 * The editor's floating chrome.
 *
 * The controls stay visible and the chrome stays out of the way: layout editing
 * is a SPATIAL task, and a panel covering the surface forces the user to map
 * editor widgets onto a layout they cannot see.
 *
 * ```text
 *  docked bottom            floating                docked right
 *  +---------------+        +---------------+       +---------------+
 *  |               |        |     [::::]    |       |          [:]  |
 *  |   controls    |        |   controls    |       | controls [:]  |
 *  | [ selection ] |        |               |       |          [:]  |
 *  | [ tool bar  ] |        |               |       |          [:]  |
 *  +---------------+        +---------------+       +---------------+
 * ```
 *
 * The toolbar is dragged by its HANDLE after a long press, never by its buttons:
 * a toolbar whose every control might instead start a drag is a toolbar whose
 * every control is slightly unreliable.
 */
@Composable
internal fun BoxScope.TouchEditorChrome(
    state: TouchEditorUiState,
    region: TouchLayoutRegion,
    actions: TouchEditorActions,
) {
    val haptics = LocalHapticFeedback.current
    var size by remember { mutableStateOf(IntSize.Zero) }
    // Live drag position of the toolbar's TOP-LEFT, in the surface's own
    // coordinates; null whenever the toolbar is resting.
    var dragged by remember { mutableStateOf<Offset?>(null) }
    // Where the toolbar is sitting when it is not being dragged, so a drag can
    // start from there instead of from wherever the finger landed.
    var origin by remember { mutableStateOf(Offset.Zero) }
    // Purely transient drag state, so it lives with the drag rather than in the
    // screen: the preview drawn under the toolbar and the placement a release
    // commits are then read from the same value on the same frame.
    var candidate by remember { mutableStateOf<TouchToolbarEdge?>(null) }

    // Get out of the way while a control is actually being moved or resized.
    // The chrome sits over the layout and a layout uses its edges; without this,
    // the one control the user is manipulating is the one they cannot see.
    val chromeAlpha by animateFloatAsState(
        targetValue = if (state.manipulating) MANIPULATING_ALPHA else 1f,
        label = "editorChromeAlpha",
    )

    val vertical = (state.placement as? TouchToolbarPlacement.Docked)?.edge?.vertical == true
    val resting = state.placement
    val currentActions = rememberUpdatedState(actions)
    val currentRegion = rememberUpdatedState(region)
    val currentSize = rememberUpdatedState(size)

    // One positioning rule for every state, in the interaction-safe region's own
    // coordinates. Aligning a docked toolbar to the WINDOW instead would put it
    // under the system gesture strip or behind a cutout, which is the same
    // mistake the layout resolver exists to prevent for controls.
    val at = dragged ?: TouchToolbarLayout
        .topLeft(resting, size.width.toFloat(), size.height.toFloat(), region)
        .let { (x, y) -> Offset(x, y) }
    val placementModifier = Modifier.offset { IntOffset(at.x.roundToInt(), at.y.roundToInt()) }

    // The dock preview, drawn under the toolbar so a release lands on something
    // the user has already seen highlighted.
    candidate?.let { edge -> DockPreview(edge, region) }

    Box(
        placementModifier
            .alpha(chromeAlpha)
            .onSizeChanged { size = it }
            // Read AFTER the placement modifier, so this is where the toolbar
            // actually ended up. A drag begins from here rather than from the
            // handle's own local origin, which is a few dp from its corner and
            // would teleport the toolbar to the top-left on the first frame.
            .onGloballyPositioned { if (dragged == null) origin = it.positionInParent() },
    ) {
        // ONE toolbar. A selection changes what its buttons do and what its menu
        // contains; it never adds a second bar, and the slot count is constant so
        // selecting something cannot make the toolbar reflow under the user's
        // finger. Geometry problems are drawn on the offending control instead of
        // narrated here -- see drawTouchEditorOverlay.
        TouchEditorToolbar(
            state = state,
            vertical = vertical,
            actions = actions,
            handle = {
                ToolbarHandle(
                    vertical = vertical,
                    // The ONLY drag origin. Action buttons never start a drag, so
                    // no toolbar control is ever ambiguous between "press me" and
                    // "move the toolbar" -- and a long press on an action is free
                    // to mean "tell me what you do".
                    modifier = Modifier.pointerInput(Unit) {
                        detectDragGesturesAfterLongPress(
                            onDragStart = {
                                haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                                // From where the toolbar IS, not from where the
                                // finger landed on the handle.
                                dragged = origin
                            },
                            onDrag = { change, delta ->
                                change.consume()
                                val moved = dragged?.plus(delta)
                                if (moved != null) {
                                    dragged = moved
                                    val next = TouchToolbarLayout.dockCandidate(
                                        moved.x,
                                        moved.y,
                                        currentSize.value.width.toFloat(),
                                        currentSize.value.height.toFloat(),
                                        currentRegion.value,
                                    )
                                    // One tick when the offered edge CHANGES, so
                                    // acquiring a dock is felt once rather than
                                    // buzzing every frame inside the zone.
                                    if (next != candidate) {
                                        candidate = next
                                        if (next != null) {
                                            haptics.performHapticFeedback(
                                                HapticFeedbackType.TextHandleMove,
                                            )
                                        }
                                    }
                                }
                            },
                            onDragCancel = { dragged = null; candidate = null },
                            onDragEnd = {
                                val released = dragged
                                dragged = null
                                candidate = null
                                if (released != null) {
                                    currentActions.value.setToolbar(
                                        TouchToolbarLayout.placementFor(
                                            released.x,
                                            released.y,
                                            currentSize.value.width.toFloat(),
                                            currentSize.value.height.toFloat(),
                                            currentRegion.value,
                                        ),
                                    )
                                }
                            },
                        )
                    },
                )
            },
        )
    }
}

/** A translucent slot along the edge a release would dock to. */
@Composable
private fun BoxScope.DockPreview(edge: TouchToolbarEdge, region: TouchLayoutRegion) {
    val density = LocalDensity.current
    // One button plus the surface's own padding either side, so the slot is the
    // size of the bar that will land in it.
    val thickness = TOOLBAR_TARGET + SPACE_S * 2
    val alignment = when (edge) {
        TouchToolbarEdge.Top -> Alignment.TopCenter
        TouchToolbarEdge.Bottom -> Alignment.BottomCenter
        TouchToolbarEdge.Left -> Alignment.CenterStart
        TouchToolbarEdge.Right -> Alignment.CenterEnd
    }
    val length = with(density) {
        if (edge.vertical) region.height.toDp() else region.width.toDp()
    }
    Surface(
        modifier = Modifier
            .align(alignment)
            .then(
                if (edge.vertical) {
                    Modifier.size(width = thickness, height = length)
                } else {
                    Modifier.size(width = length, height = thickness)
                },
            )
            .alpha(DOCK_PREVIEW_ALPHA),
        color = MaterialTheme.colorScheme.primaryContainer,
        shape = MaterialTheme.shapes.medium,
        content = {},
    )
}

/** The grab area. Long-press then drag; a plain tap does nothing at all. */
@Composable
private fun ToolbarHandle(vertical: Boolean, modifier: Modifier = Modifier) {
    Box(
        modifier
            .size(
                width = if (vertical) TOOLBAR_TARGET else HANDLE_LONG,
                height = if (vertical) HANDLE_LONG else TOOLBAR_TARGET,
            )
            .semantics { contentDescription = HANDLE_DESCRIPTION },
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            Icons.Default.DragIndicator,
            null,
            Modifier.size(ICON_SIZE),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * The primary toolbar: the small set of actions that are always available.
 *
 * The ONLY toolbar. A selection changes what these buttons do and never how many
 * there are; everything else contextual is behind More, because a toolbar wide
 * enough to hold every operation is the modal panel this design exists to avoid.
 * Every button carries a content description, so the labels are still there for
 * accessibility services and long-press tooltips.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TouchEditorToolbar(
    state: TouchEditorUiState,
    vertical: Boolean,
    actions: TouchEditorActions,
    handle: @Composable () -> Unit,
) {
    var moreOpen by remember { mutableStateOf(false) }
    val selected = state.selection.isNotEmpty()
    val grouped = state.effectiveTargets.any { state.document.instance(it)?.groupId != null }
    Surface(
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        contentColor = MaterialTheme.colorScheme.onSurface,
        tonalElevation = ELEVATION,
        shadowElevation = ELEVATION,
    ) {
        val items = @Composable {
            handle()
            EditorIcon(Icons.Default.AddBox, "Add", actions::openAddControl)
            EditorIcon(
                Icons.Default.SelectAll,
                "Select several",
                { actions.setMultiSelect(!state.multiSelect) },
                active = state.multiSelect,
            )
            // The contextual middle. These four are always PRESENT and only
            // change enablement, so a selection can never reflow the toolbar --
            // the thing that made a second bar appear and the buttons move.
            EditorIcon(
                Icons.Default.ContentCopy, "Duplicate", actions::duplicate, enabled = selected,
            )
            EditorIcon(
                Icons.Default.Workspaces,
                if (grouped) "Ungroup" else "Group",
                { if (grouped) actions.ungroup() else actions.group() },
                active = grouped,
                enabled = grouped || state.selection.size > 1,
            )
            EditorIcon(Icons.Default.Delete, "Delete", actions::delete, enabled = selected)
            EditorIcon(
                Icons.AutoMirrored.Filled.Undo, "Undo", actions::undo, enabled = state.canUndo,
            )
            EditorIcon(
                Icons.AutoMirrored.Filled.Redo, "Redo", actions::redo, enabled = state.canRedo,
            )
            Box {
                EditorIcon(Icons.Default.MoreHoriz, "More", { moreOpen = true })
                EditorMenu(
                    open = moreOpen,
                    state = state,
                    grouped = grouped,
                    actions = actions,
                    onDismiss = { moreOpen = false },
                )
            }
            EditorIcon(Icons.Default.PlayArrow, "Preview", actions::preview)
            EditorIcon(
                Icons.Default.Check,
                "Save",
                actions::save,
                enabled = state.canSave && state.dirty,
                emphasised = true,
            )
            EditorIcon(Icons.Default.Close, "Done", actions::exit)
        }
        EditorButtonFlow(
            vertical = vertical,
            count = TOOLBAR_ITEMS,
            leadExtent = if (vertical) HANDLE_LONG else TOOLBAR_TARGET,
            content = items,
        )
    }
}

/**
 * The toolbar's one menu, whose contents depend on what is selected.
 *
 * This is where the second bar went. Everything that used to sit in a permanent
 * contextual strip lives here, headed by the name of the control it will act on
 * — which is also the only place that name needs to be, because the outline and
 * corner handles on the canvas already say WHICH control, and a name chip in the
 * toolbar would change its width every time the selection did.
 */
@Composable
private fun EditorMenu(
    open: Boolean,
    state: TouchEditorUiState,
    grouped: Boolean,
    actions: TouchEditorActions,
    onDismiss: () -> Unit,
) {
    val selected = state.selection.isNotEmpty()
    DropdownMenu(open, onDismissRequest = onDismiss) {
        if (selected) {
            MenuHeader(describeSelection(state), state.selectionProblem)
            // Kept open: sizing and turning are adjustments, and reopening the
            // menu between every nudge would make them unusable.
            MenuItem("Smaller", Icons.Default.ZoomOut) { actions.nudgeScale(1f / SCALE_STEP) }
            MenuItem("Larger", Icons.Default.ZoomIn) { actions.nudgeScale(SCALE_STEP) }
            MenuItem("Turn left", Icons.AutoMirrored.Filled.RotateLeft) {
                actions.nudgeRotation(-ROTATION_STEP)
            }
            MenuItem("Turn right", Icons.AutoMirrored.Filled.RotateRight) {
                actions.nudgeRotation(ROTATION_STEP)
            }
            MenuItem("Reset orientation", Icons.Default.Restore) { actions.resetRotation() }
            MenuItem("Bring forward", Icons.Default.FlipToFront) { actions.bringForward() }
            MenuItem("Send backward", Icons.Default.FlipToBack) { actions.sendBackward() }
            LatchMenuItems(state) { actions.setLatch(it) }
            MenuItem("Exact position…", Icons.Default.Edit) {
                onDismiss(); actions.openInspector()
            }
            MenuItem("Reset control", Icons.Default.Restore) {
                onDismiss(); actions.resetSelection()
            }
            HorizontalDivider()
        }
        // Qualified rather than bare. A saved layout is called "Default" until
        // the user renames it, which put two unrelated rows called "Default" in
        // one menu -- one of them the latch mode directly above it.
        MenuItem("Layout: ${state.profileName}", Icons.Default.Layers) {
            onDismiss(); actions.openProfiles()
        }
        MenuToggle("Whole groups", Icons.Default.Workspaces, state.editGroup) {
            actions.setEditGroup(!state.editGroup)
        }
        MenuToggle("Grid", Icons.Default.GridOn, state.grid) { actions.setGrid(!state.grid) }
        MenuToggle("Snapping", Icons.Default.FormatAlignCenter, state.snap) {
            actions.setSnap(!state.snap)
        }
        HorizontalDivider()
        MenuItem("Reset layout", Icons.Default.Restore) { onDismiss(); actions.resetProfile() }
    }
}

@Composable
private fun MenuHeader(title: String, problem: String?) {
    Column(Modifier.padding(horizontal = SPACE_L, vertical = SPACE_M)) {
        Text(
            title,
            style = MaterialTheme.typography.labelLarge,
            color = if (problem == null) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.error
            },
        )
        problem?.let {
            Text(
                it,
                Modifier.widthIn(max = MENU_TEXT_MAX),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }
    }
}

/** A quiet subject line for the rows beneath it. Not a row: it does nothing. */
@Composable
private fun MenuCaption(title: String) {
    Text(
        title,
        Modifier.padding(start = SPACE_L, end = SPACE_L, top = SPACE_M, bottom = SPACE_XS),
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

/**
 * Hold-to-latch, as three rows rather than a nested menu.
 *
 * Per control and tri-state: `null` follows the global setting, and choosing it
 * again is how a user gets back to that. Absent entirely when nothing in the
 * selection can hold — a stick has no single state to keep — so the menu never
 * offers a setting that would be silently dropped.
 */
@Composable
private fun LatchMenuItems(state: TouchEditorUiState, onSelect: (Boolean?) -> Unit) {
    val latchable = state.effectiveTargets.mapNotNull { state.document.instance(it) }
        .filter { state.profile.catalogEntry(it.catalogId)?.interaction?.supportsLatch == true }
    if (latchable.isEmpty()) return
    val values = latchable.mapTo(mutableSetOf()) { it.latch }
    val current = values.singleOrNull()
    // Three rows reading "Default / Enabled / Disabled" say nothing on their own
    // about WHAT is being defaulted or enabled, and the padlock glyph is not
    // specific enough to carry it. One caption is cheaper than repeating the
    // subject in all three labels.
    MenuCaption(LATCH_CAPTION)
    LATCH_CHOICES.forEach { (value, title) ->
        DropdownMenuItem(
            text = { Text(title) },
            leadingIcon = {
                Icon(if (value == false) Icons.Default.LockOpen else Icons.Default.Lock, null)
            },
            trailingIcon = {
                if (values.size == 1 && value == current) Icon(Icons.Default.Check, null)
            },
            onClick = { onSelect(value) },
        )
    }
}

@Composable
private fun MenuItem(
    title: String,
    icon: ImageVector,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    DropdownMenuItem(
        text = { Text(title) },
        leadingIcon = { Icon(icon, null) },
        enabled = enabled,
        onClick = onClick,
    )
}

/** A menu row that is on or off, with the state shown rather than described. */
@Composable
private fun MenuToggle(title: String, icon: ImageVector, on: Boolean, onClick: () -> Unit) {
    DropdownMenuItem(
        text = { Text(title) },
        leadingIcon = {
            Icon(icon, null, tint = if (on) MaterialTheme.colorScheme.primary else LocalContentColor.current)
        },
        trailingIcon = { if (on) Icon(Icons.Default.Check, null) },
        onClick = onClick,
    )
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
private fun EditorButtonFlow(
    vertical: Boolean,
    count: Int,
    /** Extent of the first item when it is not a full-size button, as the handle is not. */
    leadExtent: Dp = TOOLBAR_TARGET,
    content: @Composable () -> Unit,
) {
    BoxWithConstraints(Modifier.padding(SPACE_S)) {
        val extent = if (vertical) maxHeight else maxWidth
        // Measure what the row or column ACTUALLY needs before deciding to wrap.
        // Dividing by the button size alone assumes every item is one, and the
        // drag handle is not — which was enough to push a ten-item vertical
        // toolbar into two columns on a 1080-tall window and split Undo from
        // Redo across them for no reason.
        val required = TOOLBAR_TARGET * (count - 1) + leadExtent
        val perLine = if (extent >= required) {
            count
        } else {
            (extent / TOOLBAR_TARGET).toInt().coerceIn(1, count)
        }
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
 * One toolbar button.
 *
 * [description] does three jobs from one string: it is the accessibility label,
 * the long-press tooltip, and the name a reader of this file sees. A long press
 * shows the tooltip and does NOT fire the action, which is safe here precisely
 * because dragging the toolbar is the handle's job alone — no action button has
 * to share long-press with anything.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EditorIcon(
    icon: ImageVector,
    description: String,
    onClick: () -> Unit,
    active: Boolean = false,
    enabled: Boolean = true,
    emphasised: Boolean = false,
) {
    TooltipBox(
        positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
        tooltip = {
            PlainTooltip(
                // Translucent: it sits over the layout being edited, and a solid
                // chip would hide the very control the user is naming an action
                // for.
                containerColor = MaterialTheme.colorScheme.inverseSurface.copy(
                    alpha = TOOLTIP_ALPHA,
                ),
            ) { Text(description) }
        },
        state = rememberTooltipState(),
    ) {
        if (emphasised) {
            FilledIconButton(
                onClick = onClick,
                enabled = enabled,
                modifier = Modifier.size(TOOLBAR_TARGET),
            ) { Icon(icon, description, Modifier.size(ICON_SIZE)) }
        } else {
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
            ) { Icon(icon, description, Modifier.size(ICON_SIZE)) }
        }
    }
}

/**
 * Create a control from the personality's catalog.
 *
 * The CATALOG, unconditionally — not "controls that are missing". Default layout
 * membership is not personality capability, so a control already on screen stays
 * addable and the user gets a second one. That is the entire feature.
 *
 * Grouped by category so a controller with twenty entries is still scannable.
 * A row carries no explanation: "A" with an Add button next to it is not a
 * concept that needs a paragraph, and the count is shown only where there is
 * one, because that is the only case a user is curious about.
 */
@Composable
internal fun TouchAddControlDialog(
    profile: TouchControllerProfile,
    document: TouchLayoutDocument,
    onAdd: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val counts = document.controls.groupingBy { it.catalogId }.eachCount()
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Done") } },
        title = { Text("Add a control") },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(SPACE_XS),
            ) {
                TouchControlCategory.entries.forEach { category ->
                    val entries = profile.catalog.filter { it.category == category }
                    if (entries.isEmpty()) return@forEach
                    Text(
                        category.title,
                        Modifier.padding(top = SPACE_M, bottom = SPACE_XS),
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.primary,
                    )
                    entries.forEach { control ->
                        AddControlRow(
                            title = controlTitle(profile, control),
                            optional = !control.inDefaultLayout,
                            existing = counts[control.id] ?: 0,
                            onAdd = { onAdd(control.id) },
                        )
                    }
                }
            }
        },
    )
}

@Composable
private fun AddControlRow(
    title: String,
    optional: Boolean,
    existing: Int,
    onAdd: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = TOOLBAR_TARGET),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(SPACE_M),
    ) {
        Text(title, Modifier.weight(1f))
        // A count only when there is one, and "Optional" only when that is the
        // interesting fact. The common row says nothing at all, because "Not on
        // screen" is what a control absent from the list above already means.
        val note = when {
            existing > 0 -> "$existing on screen"
            optional -> "Optional"
            else -> null
        }
        note?.let {
            Text(
                it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        OutlinedButton(onClick = onAdd) { Text("Add") }
    }
}

/**
 * Exact numbers for one control.
 *
 * The accessible alternative to pinch and rotate, and the answer to placements a
 * gesture cannot reach. The primary workflow stays direct manipulation; this is
 * a fallback rather than the default way to work.
 */
@Composable
internal fun TouchControlInspectorDialog(
    profile: TouchControllerProfile,
    document: TouchLayoutDocument,
    instanceId: String,
    onApply: (anchorX: Float, anchorY: Float, scale: Float, rotation: Float) -> Unit,
    onDismiss: () -> Unit,
) {
    val instance = document.instance(instanceId) ?: return
    val entry = profile.catalogEntry(instance.catalogId) ?: return
    var x by remember(instanceId) { mutableStateOf(percent(instance.anchorX)) }
    var y by remember(instanceId) { mutableStateOf(percent(instance.anchorY)) }
    var scale by remember(instanceId) { mutableStateOf(percent(instance.scale)) }
    var rotation by remember(instanceId) { mutableStateOf(instance.rotationDegrees.roundToInt().toString()) }

    AlertDialog(
        onDismissRequest = onDismiss,
        // Named from the instance: editing one of two copies has to say which.
        title = { Text(controlTitle(profile, entry, instanceId)) },
        confirmButton = {
            Button(
                onClick = {
                    onApply(
                        (x.toFloatOrNull() ?: 0f) / 100f,
                        (y.toFloatOrNull() ?: 0f) / 100f,
                        (scale.toFloatOrNull() ?: 100f) / 100f,
                        rotation.toFloatOrNull() ?: 0f,
                    )
                    onDismiss()
                },
            ) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        text = {
            // The units are in the labels; a paragraph restating them was
            // narrating the form.
            Column(verticalArrangement = Arrangement.spacedBy(SPACE_L)) {
                NumberField("Across %", x) { x = it }
                NumberField("Down %", y) { y = it }
                NumberField("Size %", scale) { scale = it }
                NumberField("Rotation °", rotation) { rotation = it }
            }
        },
    )
}

@Composable
private fun NumberField(label: String, value: String, onChange: (String) -> Unit) {
    OutlinedTextField(
        value = value,
        onValueChange = { onChange(it.filter { c -> c.isDigit() || c == '.' || c == '-' }) },
        label = { Text(label) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
        modifier = Modifier.fillMaxWidth(),
    )
}

private fun percent(value: Float): String = (value * 100f).roundToInt().toString()

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
            // The rows say it: the factory entry carries its own one-line note
            // and is the only one whose actions are restricted.
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(SPACE_XS),
            ) {
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
        Modifier.fillMaxWidth().heightIn(min = TOOLBAR_TARGET),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = onSelect)
        Column(Modifier.weight(1f)) {
            Text(entry.name)
            // The one genuinely non-obvious fact in this list: the shipped entry
            // cannot be lost, which is why its actions are greyed out.
            if (entry.isFactory) {
                Text(
                    "Always recoverable",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Box {
            IconButton(onClick = { menuOpen = true }, modifier = Modifier.size(TOOLBAR_TARGET)) {
                Icon(Icons.Default.MoreHoriz, "More actions for ${entry.name}")
            }
            DropdownMenu(menuOpen, onDismissRequest = { menuOpen = false }) {
                DropdownMenuItem(
                    text = { Text("Duplicate") },
                    onClick = { menuOpen = false; onDuplicate() },
                )
                DropdownMenuItem(
                    text = { Text("Rename") },
                    enabled = !entry.isFactory,
                    onClick = { menuOpen = false; onRename() },
                )
                DropdownMenuItem(
                    text = { Text("Reset to default") },
                    enabled = !entry.isFactory,
                    onClick = { menuOpen = false; onReset() },
                )
                DropdownMenuItem(
                    text = { Text("Delete") },
                    enabled = !entry.isFactory,
                    onClick = { menuOpen = false; onDelete() },
                )
            }
        }
    }
}

@Composable
internal fun TouchProfileNameDialog(
    title: String,
    explanation: String?,
    initial: String,
    confirmLabel: String,
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var name by remember { mutableStateOf(initial) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        confirmButton = { Button(onClick = { onConfirm(name) }) { Text(confirmLabel) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(SPACE_L)) {
                // Kept where it exists: the save-as-new case is explaining a
                // REDIRECTION the user did not ask for, which is exactly the
                // sort of non-obvious behaviour that still earns a sentence.
                explanation?.let {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it.take(TouchProfileLibrary.MAX_NAME_LENGTH) },
                    label = { Text("Name") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        },
    )
}

@Composable
internal fun TouchUnsavedChangesDialog(
    canSave: Boolean,
    onSave: () -> Unit,
    onDiscard: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Unsaved layout changes") },
        // Only the blocked case says anything. With Save, Keep editing and
        // Discard on the buttons, "Save this layout before leaving?" was the
        // title and the buttons said a third time.
        text = if (canSave) {
            null
        } else {
            { Text("This layout cannot be saved as it is. Leaving now discards the changes.") }
        },
        confirmButton = {
            if (canSave) Button(onClick = onSave) { Text("Save") }
        },
        dismissButton = {
            Row(horizontalArrangement = Arrangement.spacedBy(SPACE_M)) {
                TextButton(onClick = onDismiss) { Text("Keep editing") }
                TextButton(onClick = onDiscard) { Text("Discard") }
            }
        },
    )
}

/**
 * The selection's name, counted the way it is DRAWN.
 *
 * The count comes from the effective targets rather than the tapped ids, so it
 * always matches the number of highlighted outlines. Reporting the tapped count
 * would say "+1" while three controls are outlined and three controls move.
 */
internal fun describeSelection(state: TouchEditorUiState): String {
    val instance = state.primaryId?.let { state.document.instance(it) }
    val entry = instance?.let { state.profile.catalogEntry(it.catalogId) }
    // Named from the INSTANCE, so the second copy of a control reads "B (2)"
    // and the header says which of two identical outlines the menu will act on.
    val name = entry?.let { controlTitle(state.profile, it, instance.instanceId) }
        ?: return "${state.effectiveTargets.size} controls"
    val extra = state.effectiveTargets.size - 1
    return if (extra > 0) "$name +$extra" else name
}

/**
 * What to call a control in the editor.
 *
 * ```text
 * a face binding      the letter it is DRAWN with   A, B, X, Y
 * an authored legend  that legend                   ZL, L3, GL, Z
 * anything else       its id, made readable         Stick left, Dpad
 * ```
 *
 * The rule itself lives in [TouchControlNaming], in the shared module, and this
 * only supplies a CATALOG entry's three inputs. Sharing it is the point: the
 * audit writes the same names into the sentences it produces, so the control
 * called "B" in the picker is called "B" in the warning about it too. Taking a
 * profile is what makes the face case work — Pro Controller 2's face controls
 * carry no authored legend, their letter comes from the binding.
 */
internal fun controlTitle(
    profile: TouchControllerProfile,
    control: TouchTemplateControl,
    /**
     * The INSTANCE being named, when there is one. It supplies only the copy
     * number, and defaults to the catalog id because the Add picker is naming a
     * kind of control rather than one that exists yet.
     */
    instanceId: String = control.id,
): String = TouchControlNaming.nameFor(
    profile.bindings[control.output],
    control.visual.label,
    instanceId,
)

/**
 * The editor's spacing scale.
 *
 * Four steps, used everywhere, instead of a padding value invented per call
 * site. The point is not the numbers but that peers end up visibly equal: a
 * dialog row, a menu row and a toolbar button all reserve the same target, and
 * the gap between two things is always one of these.
 */
private val SPACE_XS = 2.dp
private val SPACE_S = 4.dp
private val SPACE_M = 8.dp
private val SPACE_L = 12.dp
private val SPACE_XL = 16.dp

/** Same target size as every other primary control in the app; the 48dp floor. */
private val TOOLBAR_TARGET = 48.dp

/** The glyph inside that target. Constant, so every icon box matches. */
private val ICON_SIZE = 20.dp

/** The drag handle is longer than it is thick, so it reads as a grab bar. */
private val HANDLE_LONG = 32.dp

/** One raised surface height for every piece of editor chrome. */
private val ELEVATION = 6.dp

/**
 * Handle, add, select-several, duplicate, group, delete, undo, redo, more,
 * preview, save, done.
 *
 * CONSTANT, and that is the requirement rather than the number: a selection
 * changes what these buttons do and never how many there are, so the toolbar
 * cannot reflow — or grow a second bar — under the user's finger.
 */
private const val TOOLBAR_ITEMS = 12

/**
 * Names the global setting these three rows override, in the same words the
 * settings sheet uses for it, so the two are recognisably the same thing.
 */
private const val LATCH_CAPTION = "Lock a button held"

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

/** One press of a rotate button. A twelfth of a quarter turn. */
private const val ROTATION_STEP = 7.5f

private const val HANDLE_DESCRIPTION = "Move the toolbar: press and hold, then drag"

/** Faded, not hidden: the chrome has to come back the instant a finger lifts. */
private const val MANIPULATING_ALPHA = 0.18f

/** Visible enough to read as a target, faint enough not to hide the layout. */
private const val DOCK_PREVIEW_ALPHA = 0.35f

/** Readable over the layout without hiding the control being named. */
private const val TOOLTIP_ALPHA = 0.82f

/** Keeps a menu from stretching to the width of a long sentence. */
private val MENU_TEXT_MAX = 220.dp
