package dev.picoswitch.companion.ui.touch

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.layout.exclude
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.LinkOff
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.bridge.touch.ResolvedTouchControl
import dev.picoswitch.bridge.touch.ResolvedTouchLayout
import dev.picoswitch.bridge.touch.TouchAlignmentSettings
import dev.picoswitch.bridge.touch.TouchEditorAlignment
import dev.picoswitch.bridge.touch.TouchEditorDelta
import dev.picoswitch.bridge.touch.TouchLayoutAudit
import dev.picoswitch.bridge.touch.TouchLayoutAuditMode
import dev.picoswitch.bridge.touch.TouchLayoutComposer
import dev.picoswitch.bridge.touch.TouchLayoutEditor
import dev.picoswitch.bridge.touch.TouchLayoutOverride
import dev.picoswitch.bridge.touch.TouchLayoutRegion
import dev.picoswitch.bridge.touch.TouchLayoutResolver
import dev.picoswitch.bridge.touch.TouchControllerProfile
import dev.picoswitch.bridge.touch.TouchLayoutProfile
import dev.picoswitch.bridge.touch.TouchProfileCatalog
import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.bridge.touch.TouchProfileLibrary
import dev.picoswitch.bridge.touch.TouchProfileLibraryEditor
import dev.picoswitch.bridge.touch.TouchReleaseReason
import dev.picoswitch.companion.bridge.AndroidTouchFeedback
import dev.picoswitch.companion.data.TouchEditorDock
import dev.picoswitch.companion.data.TouchGamepadSettings
import dev.picoswitch.companion.ui.CompanionUiState
import dev.picoswitch.companion.ui.CompanionViewModel
import dev.picoswitch.bridge.touch.TouchFeedbackBackend
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.withContext

/**
 * The on-screen controller, full screen.
 *
 * A dedicated application MODE rather than a destination inside the ordinary
 * scaffold: it owns edge-to-edge presentation, hides the navigation chrome, and
 * has its own back behaviour. Rendering it inside the content column would give
 * it the scaffold's insets and width limit, which is the opposite of what a
 * gameplay surface needs.
 *
 * ```text
 * measured area  -  system gesture / cutout insets
 *        |
 *        v
 * TouchLayoutResolver          portable
 *        |
 *        v            Compose pointer events
 * TouchControlEngine  <---- TouchContactTracker  <---- touchGamepadContacts
 *        |
 *        v
 * ControllerInputState -> BridgeSession -> the adapter
 * ```
 *
 * The background may draw edge to edge; the CONTROLS are resolved inside the
 * interaction-safe rectangle so nothing important lives under a back-gesture
 * strip or a display cutout.
 */
@Composable
fun TouchGamepadScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onPickBackgroundImage: () -> Unit,
    /** Start or restart the controller link without leaving the mode. */
    onRetryLink: () -> Unit,
) {
    val gamepad = viewModel.touchGamepad
    val view = LocalView.current
    val context = LocalContext.current
    val density = LocalDensity.current
    val layoutDirection = LocalLayoutDirection.current
    val viewConfiguration = LocalViewConfiguration.current
    val settings = ui.touchSettings

    var menuOpen by rememberSaveable { mutableStateOf(false) }
    var editing by rememberSaveable { mutableStateOf(false) }
    var area by remember { mutableStateOf(IntSize.Zero) }
    val profileId = ui.touchProfileId
    val profile = profileId?.let(TouchProfileCatalog::require)
    val library = ui.touchProfiles

    // Ordered so the LAST control added is the primary one: the contextual bar
    // names it, and alignment guides are computed from it.
    var selection by remember(profileId) { mutableStateOf<Set<String>>(emptySet()) }
    var primaryId by remember(profileId) { mutableStateOf<String?>(null) }
    var editGroup by rememberSaveable(profileId) { mutableStateOf(true) }
    /**
     * The authored layout the draft started from.
     *
     * Kept explicitly rather than re-read from the UI state, because "has the
     * user changed anything" and "has the ACTIVE PROFILE changed underneath the
     * editor" are different questions and the answer to the first must not
     * change just because the second happened.
     */
    val authored = profile?.let { ui.touchLayoutOverride ?: TouchLayoutEditor.empty(it) }
    var baseline by remember(profileId) { mutableStateOf(authored) }
    var draftOverride by remember(profileId) { mutableStateOf(authored) }
    var profilesOpen by remember { mutableStateOf(false) }
    var addControlOpen by remember { mutableStateOf(false) }
    var nameRequest by remember { mutableStateOf<TouchProfileNameRequest?>(null) }
    var pendingConfirm by remember { mutableStateOf<TouchEditorConfirm?>(null) }
    /** True while a finger is actually moving or resizing something. */
    var manipulating by remember { mutableStateOf(false) }
    val dirty = editing && draftOverride != baseline

    fun resetDraft() {
        draftOverride = baseline
        selection = emptySet()
        primaryId = null
    }

    LaunchedEffect(profileId) {
        editing = false
        resetDraft()
    }
    // A profile switch, a save or an import replaces the authored layout. Adopt
    // it whenever the draft has nothing of its own to lose; a dirty draft is
    // left alone until the user has answered for it, because silently replacing
    // an edit in progress is how an editor loses somebody's work.
    LaunchedEffect(authored) {
        if (!editing || draftOverride == baseline) {
            draftOverride = authored
            selection = emptySet()
            primaryId = null
        }
        baseline = authored
    }

    fun handleMenuEvent(event: TouchGamepadMenuEvent) {
        val result = resolveTouchGamepadMenuEvent(menuOpen, event)
        menuOpen = result.menuOpen
        if (result.exitTouchGamepad) viewModel.exitTouchGamepad()
    }

    // Visual state is LOCAL and mutated from the pointer handler. Hanging it off
    // the application state would recompose the whole app for every pixel a thumb
    // moves; the semantic input has already reached the bridge by then, and this
    // is only the picture.
    val visual = remember { mutableStateOf(TouchVisualState()) }

    /**
     * Recompute the picture from what the engine is holding.
     *
     * Called from the pointer path after a whole batch and from each release
     * boundary — never on a timer. A per-frame poll would keep the frame clock
     * running for as long as the controller is on screen, and this surface is
     * idle whenever no thumb is moving.
     */
    fun refreshVisual() {
        val snapshot = gamepad.diagnostics()
        val layout = gamepad.engine.resolvedLayout
        val next = visual.value.copy(
            leftStick = snapshot.leftStick,
            rightStick = snapshot.rightStick,
            dpad = snapshot.dpad,
            pressed = pressedControlIds(layout, snapshot) { gamepad.engine.contactOn(it) != null },
            // Read from the LATCH, never from what is currently published: a
            // retrigger pulse momentarily un-presses the control, and blinking
            // the padlock off while the user taps a button they are deliberately
            // holding would say the hold had been lost.
            latched = snapshot.latchedControls,
            arming = snapshot.armedControls,
        )
        if (next != visual.value) visual.value = next
    }

    /**
     * Drives the engine's timed gesture work.
     *
     * Two things the engine cannot discover on its own need a clock: the
     * deliberate dwell that completes a latch, and the brief mask that gives a
     * retrigger pulse an observable release edge. A still finger produces no
     * pointer events, so nothing would ever wake the engine.
     *
     * A pull loop rather than a scheduled callback, and deliberately so: the
     * engine is asked what it is waiting for, this sleeps until then, and asks
     * again. Nothing is captured, so a teardown between the sleep and the tick
     * cannot resurrect a control — the tick simply finds no work. `collectLatest`
     * restarts the loop whenever a contact batch may have created or cancelled a
     * deadline.
     *
     * The clock is [touchClockNanos] because the deadlines are absolute values in
     * the same clock Compose stamps contacts with.
     */
    val gestureRevision = remember { MutableStateFlow(0L) }
    LaunchedEffect(gamepad) {
        gestureRevision.collectLatest {
            while (true) {
                val deadline = gamepad.nextDeadlineNanos() ?: break
                val waitNanos = deadline - touchClockNanos()
                if (waitNanos > 0) delay(waitNanos / NANOS_PER_MILLI + 1)
                gamepad.tick(touchClockNanos())
                refreshVisual()
            }
        }
    }

    // The IME is deliberately excluded. Nothing on the gameplay surface takes
    // text; only the editor's own name prompts do, and those are dialogs with
    // their own insets. Letting the keyboard shrink the interaction rectangle
    // would re-resolve the whole controller while a profile is being renamed --
    // and could report the window as too small for it.
    val insets = WindowInsets.safeContent.exclude(WindowInsets.ime)
    val left = insets.getLeft(density, layoutDirection)
    val top = insets.getTop(density)
    val right = insets.getRight(density, layoutDirection)
    val bottom = insets.getBottom(density)

    val composition = remember(profile, ui.touchLayoutOverride, draftOverride, editing) {
        profile?.let {
            TouchLayoutComposer.compose(
                it,
                if (editing) draftOverride else ui.touchLayoutOverride,
            )
        }
    }
    val attemptedResolved = remember(
        area, left, top, right, bottom, density.density, composition, editing,
    ) {
        if (area.width == 0 || area.height == 0 || composition == null) {
            ResolvedTouchLayout.Empty
        } else {
            TouchLayoutResolver.resolve(
                composition.layout,
                TouchLayoutRegion(
                    left = left.toFloat(),
                    top = top.toFloat(),
                    right = (area.width - right).toFloat(),
                    bottom = (area.height - bottom).toFloat(),
                    unitScale = density.density,
                ),
                if (editing) TouchLayoutAuditMode.UserDraft else TouchLayoutAuditMode.Runtime,
            )
        }
    }
    val defaultResolved = remember(
        area, left, top, right, bottom, density.density, profile,
    ) {
        if (area.width == 0 || area.height == 0 || profile == null) {
            ResolvedTouchLayout.Empty
        } else {
            TouchLayoutResolver.resolve(
                TouchLayoutComposer.compose(profile).layout,
                TouchLayoutRegion(
                    left = left.toFloat(),
                    top = top.toFloat(),
                    right = (area.width - right).toFloat(),
                    bottom = (area.height - bottom).toFloat(),
                    unitScale = density.density,
                ),
                TouchLayoutAuditMode.ShippedTemplate,
            )
        }
    }
    val runtimeFallback = !editing && ui.touchLayoutOverride != null && !attemptedResolved.fits && defaultResolved.fits
    val resolved = if (runtimeFallback) defaultResolved else attemptedResolved
    val layoutWarning = ui.touchLayoutWarning ?: composition?.warning ?: if (runtimeFallback) {
        "Your saved layout was unsafe here, so the immutable default is active: ${attemptedResolved.problem}"
    } else null

    // Geometry changed: every retained contact position was measured against the
    // previous rectangle and means nothing now.
    var installedProfileId by remember { mutableStateOf<TouchProfileId?>(null) }
    LaunchedEffect(resolved, profileId, editing) {
        if (!editing) {
            val reason = if (installedProfileId != null && installedProfileId != profileId) {
                TouchReleaseReason.PersonalityChanged
            } else {
                TouchReleaseReason.GeometryInvalidated
            }
            gamepad.setLayout(resolved, reason)
            installedProfileId = profileId
        }
        visual.value = TouchVisualState(
            enabled = !editing && profile != null && ui.bridge.phase == BridgeLinkPhase.Playing,
        )
    }

    // The chrome fades while something is being manipulated and is restored when
    // the gesture ends. A gesture can also end by having its pointer input torn
    // down -- the window resized, the mode left -- and a permanently faded
    // toolbar is unusable, so both boundaries clear the flag as well.
    LaunchedEffect(editing, resolved.region) { manipulating = false }

    // Platform gesture timing, not invented constants: the same numbers every
    // other double tap on this device uses, including whatever the user's
    // accessibility settings have done to them. Set here rather than in the view
    // model because this is the layer that has a view configuration at all.
    LaunchedEffect(viewConfiguration) {
        gamepad.setConfig(
            gamepad.config.copy(
                latch = gamepad.config.latch.copy(
                    doubleTapWindowNanos = viewConfiguration.doubleTapTimeoutMillis * NANOS_PER_MILLI,
                    minTapGapNanos = viewConfiguration.doubleTapMinTimeMillis * NANOS_PER_MILLI,
                    maxTapDurationNanos = viewConfiguration.longPressTimeoutMillis * NANOS_PER_MILLI,
                ),
            ),
        )
    }

    LaunchedEffect(settings.hapticsEnabled) {
        gamepad.setFeedbackBackend(
            if (settings.hapticsEnabled) AndroidTouchFeedback(view) else TouchFeedbackBackend.None,
        )
    }

    // Opening the menu is an interruption like any other: whatever was held has
    // no contact left that will release it.
    LaunchedEffect(menuOpen) {
        if (menuOpen) gamepad.release(TouchReleaseReason.HostInactive)
        refreshVisual()
    }

    LaunchedEffect(ui.bridge.phase) {
        visual.value = visual.value.copy(enabled = ui.bridge.phase == BridgeLinkPhase.Playing)
    }

    ImmersivePresentation(landscapePreferred = true)
    ReleaseOnHostInactive(
        onRelease = {
            viewModel.releaseTouchInput(it)
            refreshVisual()
        },
    )

    BackHandler(enabled = true) {
        if (editing) {
            if (dirty) pendingConfirm = TouchEditorConfirm.Exit else editing = false
            return@BackHandler
        }
        // Android routes either left- or right-edge back gesture here. Gameplay
        // opens the menu instead of exiting; while the menu is visible, the same
        // gesture closes it. Leaving is an explicit action inside the menu.
        handleMenuEvent(TouchGamepadMenuEvent.Back)
    }

    // Group expansion is applied ONCE, here, so what is highlighted, what the
    // guides are computed from, and what an edit actually moves are the same set.
    val effectiveTargets = remember(profile, selection, editGroup) {
        profile?.let { TouchLayoutEditor.targetIds(it.defaultTemplate, selection, editGroup) }
            .orEmpty()
    }
    val alignment = remember(settings.editorGrid, settings.editorSnap) {
        TouchAlignmentSettings(grid = settings.editorGrid, snap = settings.editorSnap)
    }
    val gridLines = remember(resolved.region, alignment, editing) {
        if (editing) TouchEditorAlignment.gridLines(resolved.region, alignment) else emptyList()
    }
    val guides = remember(resolved, effectiveTargets, primaryId, alignment, editing) {
        if (editing) {
            TouchEditorAlignment.matchedGuides(resolved, effectiveTargets, primaryId, alignment)
        } else {
            emptyList()
        }
    }

    val background = rememberTouchBackground(settings.backgroundImage)
    val textMeasurer = rememberTextMeasurer()
    val palette = touchControlPalette()
    val labelStyle = MaterialTheme.typography.titleMedium.copy(
        fontSize = 24.sp,
        fontWeight = FontWeight.SemiBold,
    )

    val haptics = LocalHapticFeedback.current

    // Both are read through State so the long-lived pointer coroutine never acts
    // on a layout or a callback captured from an earlier composition. Keying the
    // pointer input on the layout itself would instead restart the gesture the
    // moment a drag changed the draft -- which is every frame of a drag.
    val layoutState = rememberUpdatedState(resolved)
    val gestures = rememberUpdatedState(
        TouchEditorGestures(
            onTap = { id ->
                selection = if (id == null) emptySet() else setOf(id)
                primaryId = id
            },
            onLongPress = { id ->
                haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                if (id in selection && selection.size > 1) {
                    selection = selection - id
                    primaryId = selection.lastOrNull()
                } else {
                    selection = selection + id
                    primaryId = id
                }
            },
            onDragStart = { id ->
                if (id !in selection) selection = setOf(id)
                primaryId = id
                manipulating = true
            },
            onGestureEnd = { manipulating = false },
            onMove = { layout, deltaX, deltaY ->
                val current = draftOverride
                val active = primaryId
                if (profile != null && current != null && active != null) {
                    val adjusted = TouchEditorAlignment.snap(
                        layout = layout,
                        selection = effectiveTargets,
                        primaryId = active,
                        delta = TouchEditorDelta(deltaX, deltaY),
                        settings = alignment,
                    )
                    draftOverride = TouchLayoutEditor.move(
                        profile,
                        current,
                        selection,
                        adjusted.x / layout.region.width.coerceAtLeast(1f),
                        adjusted.y / layout.region.height.coerceAtLeast(1f),
                        editGroup,
                    )
                }
            },
            onZoom = { factor ->
                manipulating = true
                val current = draftOverride
                if (profile != null && current != null && selection.isNotEmpty()) {
                    draftOverride =
                        TouchLayoutEditor.scaleBy(profile, current, selection, factor, editGroup)
                }
            },
        ),
    )

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .onSizeChanged { area = it },
    ) {
        background?.let { image ->
            Image(
                bitmap = image,
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
            // Readability over a bright photo. Decoration only; it can never move
            // a hit region.
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = settings.backgroundDim)),
            )
        }

        val unusable = profile == null || !resolved.fits && (!editing || resolved.regionTooSmall)
        if (profile == null) {
            UnusableWindowNotice(layoutWarning, onExit = viewModel::exitTouchGamepad)
        } else if (unusable) {
            // The editor stays open for an audit failure -- moving or resizing a
            // control is exactly how that gets fixed -- but not for a window
            // that is simply too small, where no edit can help.
            UnusableWindowNotice(resolved.problem, onExit = viewModel::exitTouchGamepad)
        } else {
            Canvas(
                modifier = Modifier
                    .fillMaxSize()
                    .semantics { contentDescription = CONTROLLER_DESCRIPTION }
                    .then(
                        if (menuOpen) {
                            Modifier
                        } else if (editing) {
                            Modifier.editTouchLayout(
                                key = profileId,
                                region = resolved.region,
                                layout = layoutState,
                                gestures = gestures,
                            )
                        } else {
                            Modifier.touchGamepadContacts(
                                key = resolved,
                                tracker = gamepad.contacts,
                                // A gesture deadline can only ever appear as a
                                // result of a contact going down, so bumping the
                                // driver here cannot miss one.
                                afterBatch = { refreshVisual(); gestureRevision.value++ },
                            )
                        },
                    ),
            ) {
                // Read INSIDE the draw lambda: the write from the pointer handler
                // then invalidates drawing only, never composition.
                drawTouchControls(
                    layout = resolved,
                    visual = visual.value,
                    palette = palette,
                    faceLayout = ui.touchFaceLayout,
                    opacity = settings.controlOpacity,
                    textMeasurer = textMeasurer,
                    labelStyle = labelStyle,
                )
                if (editing) {
                    drawTouchEditorOverlay(
                        layout = resolved,
                        targets = effectiveTargets,
                        primaryId = primaryId,
                        grid = gridLines,
                        guides = guides,
                        palette = palette,
                    )
                }
            }
        }

        // In the layout's quiet centre, not over the controls. The middle band is
        // kept free by the layout itself, so a status strip there cannot shadow
        // anything the user is trying to press.
        val bannerTop = with(density) {
            (resolved.region.top + resolved.region.height * BANNER_BAND).toDp()
        }
        // Not while the surface is explaining why there is no controller: the
        // banner sits in the layout's quiet band, which is where that
        // explanation is, and two overlapping messages read as neither.
        if (!unusable) LinkStatusBanner(
            phase = ui.bridge.phase,
            message = ui.bridge.message,
            onRetry = onRetryLink,
            modifier = Modifier.align(Alignment.TopCenter).padding(top = bannerTop),
        )

        if (menuOpen) {
            TouchGamepadMenu(
                settings = settings,
                faceLayout = ui.touchFaceLayout,
                profileId = profileId,
                profileName = profile?.displayName,
                library = library,
                layoutWarning = layoutWarning,
                onSettings = viewModel::setTouchSettings,
                onFaceLayout = viewModel::setTouchFaceLayout,
                onEditLayout = {
                    profile?.let {
                        resetDraft()
                        editing = true
                        menuOpen = false
                        viewModel.beginTouchLayoutEdit()
                    }
                },
                onProfiles = { menuOpen = false; profilesOpen = true },
                onRestoreDefaults = viewModel::restoreTouchLayoutDefaults,
                onPickBackground = onPickBackgroundImage,
                onClearBackground = { viewModel.setTouchBackground(null) },
                onExit = { handleMenuEvent(TouchGamepadMenuEvent.Exit) },
                onClose = { menuOpen = false },
            )
        }

        if (editing && profile != null && draftOverride != null && library != null &&
            !resolved.regionTooSmall
        ) {
            val draft = requireNotNull(draftOverride)
            val activeProfile = library.selected

            /** Commit the draft; the library decides whether that needs a new profile. */
            fun commit(targetId: String, newName: String) {
                val findings = TouchLayoutAudit.audit(
                    requireNotNull(composition).layout,
                    attemptedResolved.controls,
                    attemptedResolved.region,
                    profile,
                    TouchLayoutAuditMode.UserDraft,
                )
                if (findings.none { it.blocking } && attemptedResolved.fits) {
                    viewModel.saveTouchLayoutOverride(draft, targetId, newName)
                    editing = false
                }
            }

            fun requestSave() {
                if (activeProfile.isFactory && draft.controls.isNotEmpty()) {
                    // The shipped layout is never written. Name the copy instead
                    // of failing, so the edit that has just been made survives.
                    nameRequest = TouchProfileNameRequest.SaveAsNew(
                        TouchProfileLibraryEditor.DEFAULT_NEW_PROFILE_NAME,
                    )
                } else {
                    commit(activeProfile.id, TouchProfileLibraryEditor.DEFAULT_NEW_PROFILE_NAME)
                }
            }

            TouchEditorChrome(
                profile = profile,
                draft = draft,
                selection = selection,
                effectiveTargets = effectiveTargets,
                primaryId = primaryId,
                dock = settings.editorToolbarDock,
                editGroup = editGroup,
                grid = settings.editorGrid,
                snap = settings.editorSnap,
                profileName = activeProfile.name,
                canSave = attemptedResolved.fits,
                blockingProblem = attemptedResolved.problem.takeIf { !attemptedResolved.fits },
                dirty = dirty,
                manipulating = manipulating,
                actions = object : TouchEditorActions {
                    override fun setEditGroup(value: Boolean) { editGroup = value }

                    override fun setGrid(value: Boolean) {
                        viewModel.setTouchSettings(settings.copy(editorGrid = value))
                    }

                    override fun setSnap(value: Boolean) {
                        viewModel.setTouchSettings(settings.copy(editorSnap = value))
                    }

                    override fun setDock(dock: TouchEditorDock) {
                        viewModel.setTouchSettings(settings.copy(editorToolbarDock = dock))
                    }

                    override fun nudgeScale(factor: Float) {
                        if (selection.isEmpty()) return
                        draftOverride = TouchLayoutEditor.scaleBy(
                            profile, draft, selection, factor, editGroup,
                        )
                    }

                    override fun setVisible(visible: Boolean) {
                        if (selection.isEmpty()) return
                        draftOverride = TouchLayoutEditor.setVisible(
                            profile, draft, selection, visible, editGroup,
                        )
                        // A hidden control has no geometry to select or drag.
                        if (!visible) { selection = emptySet(); primaryId = null }
                    }

                    override fun setLatch(latch: Boolean?) {
                        if (selection.isEmpty()) return
                        draftOverride = TouchLayoutEditor.setLatch(
                            profile, draft, selection, latch, editGroup,
                        )
                    }

                    override fun resetSelection() {
                        if (selection.isEmpty()) return
                        draftOverride = TouchLayoutEditor.reset(profile, draft, selection, editGroup)
                    }

                    override fun resetProfile() {
                        draftOverride = TouchLayoutEditor.resetAll(profile)
                        selection = emptySet()
                        primaryId = null
                    }

                    override fun openProfiles() { profilesOpen = true }

                    override fun openAddControl() { addControlOpen = true }

                    override fun save() = requestSave()

                    override fun exit() {
                        if (dirty) pendingConfirm = TouchEditorConfirm.Exit else editing = false
                    }
                },
            )

            if (addControlOpen) {
                TouchAddControlDialog(
                    profile = profile,
                    draft = draft,
                    onRestore = { id ->
                        draftOverride = TouchLayoutEditor.setVisible(
                            profile, draft, setOf(id), true, editGroup = false,
                        )
                        selection = setOf(id)
                        primaryId = id
                    },
                    onDismiss = { addControlOpen = false },
                )
            }

            nameRequest?.let { request ->
                if (request is TouchProfileNameRequest.SaveAsNew) {
                    TouchProfileNameDialog(
                        title = "Save as a new profile",
                        explanation = "${library.factoryProfile.name} is the shipped layout and " +
                            "is never overwritten, so this becomes a profile of your own.",
                        initial = request.initial,
                        confirmLabel = "Save",
                        onConfirm = { name ->
                            nameRequest = null
                            commit(TouchProfileLibrary.FACTORY_PROFILE_ID, name)
                        },
                        onDismiss = { nameRequest = null },
                    )
                }
            }

            pendingConfirm?.let { confirm ->
                TouchUnsavedChangesDialog(
                    canSave = attemptedResolved.fits,
                    onSave = { pendingConfirm = null; requestSave() },
                    onDiscard = {
                        pendingConfirm = null
                        resetDraft()
                        when (confirm) {
                            TouchEditorConfirm.Exit -> editing = false
                            is TouchEditorConfirm.SelectProfile ->
                                viewModel.selectTouchProfile(confirm.profileId)
                        }
                    },
                    onDismiss = { pendingConfirm = null },
                )
            }
        }

        if (profilesOpen && library != null) {
            TouchProfileDialog(
                library = library,
                onSelect = { id ->
                    if (id != library.selectedProfileId) {
                        if (dirty) {
                            pendingConfirm = TouchEditorConfirm.SelectProfile(id)
                        } else {
                            viewModel.selectTouchProfile(id)
                            resetDraft()
                        }
                    }
                },
                onCreate = { profilesOpen = false; nameRequest = TouchProfileNameRequest.Create("") },
                onDuplicate = { id ->
                    profilesOpen = false
                    nameRequest = TouchProfileNameRequest.Duplicate(
                        id,
                        library.profile(id)?.name.orEmpty(),
                    )
                },
                onRename = { entry ->
                    profilesOpen = false
                    nameRequest = TouchProfileNameRequest.Rename(entry.id, entry.name)
                },
                onDelete = viewModel::deleteTouchProfile,
                onReset = viewModel::resetTouchProfile,
                onDismiss = { profilesOpen = false },
            )
        }

        // The create/duplicate/rename prompts are shared by the in-editor
        // toolbar and the menu, so they live outside the editing branch.
        nameRequest?.let { request ->
            when (request) {
                is TouchProfileNameRequest.Create -> TouchProfileNameDialog(
                    title = "New layout profile",
                    explanation = "It starts as a copy of the shipped layout.",
                    initial = request.initial,
                    confirmLabel = "Create",
                    onConfirm = { name ->
                        nameRequest = null
                        viewModel.createTouchProfile(name)
                        resetDraft()
                    },
                    onDismiss = { nameRequest = null },
                )
                is TouchProfileNameRequest.Duplicate -> TouchProfileNameDialog(
                    title = "Duplicate profile",
                    explanation = null,
                    initial = request.initial,
                    confirmLabel = "Duplicate",
                    onConfirm = { name ->
                        nameRequest = null
                        viewModel.duplicateTouchProfile(request.profileId, name)
                        resetDraft()
                    },
                    onDismiss = { nameRequest = null },
                )
                is TouchProfileNameRequest.Rename -> TouchProfileNameDialog(
                    title = "Rename profile",
                    explanation = null,
                    initial = request.initial,
                    confirmLabel = "Rename",
                    onConfirm = { name ->
                        nameRequest = null
                        viewModel.renameTouchProfile(request.profileId, name)
                    },
                    onDismiss = { nameRequest = null },
                )
                is TouchProfileNameRequest.SaveAsNew -> Unit
            }
        }
    }

    DisposableEffect(gamepad) {
        onDispose {
            gamepad.release(TouchReleaseReason.Disposed)
        }
    }
}

internal enum class TouchGamepadMenuEvent { Back, Exit }

internal data class TouchGamepadMenuResult(
    val menuOpen: Boolean,
    val exitTouchGamepad: Boolean,
)

/** Host navigation policy kept separate from the portable gameplay layout. */
internal fun resolveTouchGamepadMenuEvent(
    menuOpen: Boolean,
    event: TouchGamepadMenuEvent,
): TouchGamepadMenuResult = when (event) {
    TouchGamepadMenuEvent.Back -> TouchGamepadMenuResult(
        menuOpen = !menuOpen,
        exitTouchGamepad = false,
    )
    TouchGamepadMenuEvent.Exit -> TouchGamepadMenuResult(
        menuOpen = false,
        exitTouchGamepad = true,
    )
}

/**
 * Edge-to-edge, bars hidden, landscape preferred while the controller is open.
 *
 * Every change is undone on the way out. The bars are hidden with the transient
 * behaviour so a swipe always brings them back — a gameplay surface the user
 * cannot leave by any ordinary means is not immersive, it is a trap.
 *
 * The orientation is a REQUEST. Large-screen and windowed configurations may
 * ignore it, which is correct and is why nothing below depends on it: the layout
 * resolves against whatever rectangle actually arrives.
 */
@Composable
private fun ImmersivePresentation(landscapePreferred: Boolean) {
    val view = LocalView.current
    val context = LocalContext.current
    DisposableEffect(landscapePreferred) {
        val activity = context.findActivity()
        val window = activity?.window
        val previousOrientation = activity?.requestedOrientation
        val controller = window?.let { WindowInsetsControllerCompat(it, view) }

        // Nothing here touches decorFitsSystemWindows. The activity is already
        // edge-to-edge for its whole life (see applyEdgeToEdgeChrome), and the
        // old restore-to-true on dispose would have dropped the rest of the app
        // back out of edge-to-edge on the API levels where that call still does
        // anything -- leaving the system-bar regions unpainted again after the
        // on-screen controller was closed. This effect only hides and reshows
        // the bars.
        controller?.apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(androidx.core.view.WindowInsetsCompat.Type.systemBars())
        }
        if (landscapePreferred && activity != null) {
            activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        }

        onDispose {
            controller?.show(androidx.core.view.WindowInsetsCompat.Type.systemBars())
            if (activity != null && previousOrientation != null) {
                activity.requestedOrientation = previousOrientation
            }
        }
    }
}

/**
 * Release on every host-inactivity boundary the surface can observe.
 *
 * ON_PAUSE covers the screen turning off, the app being backgrounded, a system
 * picker taking focus and the lock screen appearing. The Activity neutralizes
 * the session on the same event, so the console is told as well as the engine
 * being cleared — but this is the one that runs even if the surface is composed
 * somewhere the Activity's own hook does not reach.
 */
@Composable
private fun ReleaseOnHostInactive(onRelease: (TouchReleaseReason) -> Unit) {
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_PAUSE, Lifecycle.Event.ON_STOP ->
                    onRelease(TouchReleaseReason.HostInactive)
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
}

/**
 * The truth about the link, without replacing the controller with an error page.
 *
 * An automatic reconnect is plausible and usually quick, so the layout stays put
 * and the state is stated in a strip. Silence would be worse than either: a
 * controller that looks fine and does nothing is the hardest failure to report.
 */
@Composable
private fun LinkStatusBanner(
    phase: BridgeLinkPhase,
    message: String?,
    onRetry: () -> Unit,
    modifier: Modifier = Modifier,
) {
    if (phase == BridgeLinkPhase.Playing) return
    val text = when (phase) {
        BridgeLinkPhase.Idle -> "Not connected to the adapter"
        BridgeLinkPhase.Preparing, BridgeLinkPhase.Registering -> "Preparing the controller link…"
        BridgeLinkPhase.Connecting -> "Connecting to the adapter…"
        BridgeLinkPhase.Ready -> "Ready — reconnecting the controller link…"
        BridgeLinkPhase.Unsupported -> "This device cannot act as a controller"
        BridgeLinkPhase.Failed -> message ?: "The controller link failed"
        BridgeLinkPhase.Playing -> return
    }
    val failed = phase == BridgeLinkPhase.Failed || phase == BridgeLinkPhase.Unsupported
    // A spinner is a claim that something is happening. Idle is not something
    // happening -- nothing is in flight and nothing will be until the user acts —
    // so it gets an icon like any other resting state.
    val working = phase == BridgeLinkPhase.Preparing || phase == BridgeLinkPhase.Registering ||
        phase == BridgeLinkPhase.Connecting || phase == BridgeLinkPhase.Ready
    Surface(
        modifier = modifier.padding(8.dp),
        // A container colour rather than a translucent surface: over a black
        // background or a dark photograph, "surface at 86%" is indistinguishable
        // from nothing at all, and an unreadable status is worse than none.
        color = if (failed) MaterialTheme.colorScheme.errorContainer
        else MaterialTheme.colorScheme.secondaryContainer,
        contentColor = if (failed) MaterialTheme.colorScheme.onErrorContainer
        else MaterialTheme.colorScheme.onSecondaryContainer,
        shape = MaterialTheme.shapes.small,
        tonalElevation = 3.dp,
    ) {
        Row(
            Modifier
                .padding(horizontal = 14.dp, vertical = 8.dp)
                .semantics { liveRegion = LiveRegionMode.Polite },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            when {
                working -> CircularProgressIndicator(
                    Modifier.size(14.dp),
                    strokeWidth = 2.dp,
                    color = LocalContentColor.current,
                )
                failed -> Icon(Icons.Default.Warning, null, Modifier.size(16.dp))
                else -> Icon(Icons.Default.LinkOff, null, Modifier.size(16.dp))
            }
            Spacer(Modifier.width(8.dp))
            Text(text, style = MaterialTheme.typography.labelLarge)
            // A link that is not going to fix itself must be actionable from
            // here. Leaving the only recovery outside the mode would mean the
            // answer to "it says it cannot connect" is "leave and come back".
            if (!working) {
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = onRetry, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text("Retry")
                }
            }
        }
    }
}

/**
 * Shown instead of the controller when the window genuinely cannot hold it.
 *
 * Drawing an overlapping controller here would be worse than refusing: the user
 * would press one control and the console would receive another.
 */
@Composable
private fun UnusableWindowNotice(problem: String?, onExit: () -> Unit) {
    Box(Modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        // On its own card, not straight onto the surface. This mode paints its
        // own black background regardless of the app's theme, so text coloured
        // for the theme's surface can end up black on black -- and the one
        // message that explains why there is no controller is the last thing
        // that should be unreadable.
        Surface(
            shape = MaterialTheme.shapes.large,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
            contentColor = MaterialTheme.colorScheme.onSurface,
            tonalElevation = 6.dp,
        ) {
            Column(
                Modifier.padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    problem ?: "This window is too small for the on-screen controller",
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    "Turn the device sideways or make the window larger.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Button(onClick = onExit, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text("Leave the on-screen controller")
                }
            }
        }
    }
}

/**
 * The controller's own settings, and the way out.
 *
 * Scoped here rather than added to the application's Settings page: these are
 * decisions about one surface, and none of them mean anything anywhere else.
 */
@Composable
private fun TouchGamepadMenu(
    settings: TouchGamepadSettings,
    faceLayout: ControllerFaceLayout,
    profileId: TouchProfileId?,
    profileName: String?,
    library: TouchProfileLibrary?,
    layoutWarning: String?,
    onSettings: (TouchGamepadSettings) -> Unit,
    onFaceLayout: (ControllerFaceLayout) -> Unit,
    onEditLayout: () -> Unit,
    onProfiles: () -> Unit,
    onRestoreDefaults: () -> Unit,
    onPickBackground: () -> Unit,
    onClearBackground: () -> Unit,
    onExit: () -> Unit,
    onClose: () -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.6f)),
        contentAlignment = Alignment.Center,
    ) {
        Card(
            Modifier
                .widthIn(max = 460.dp)
                .fillMaxWidth(0.9f)
                .windowInsetsPadding(WindowInsets.safeContent),
        ) {
            Column(
                Modifier.padding(20.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Touch Gamepad",
                        Modifier.weight(1f),
                        style = MaterialTheme.typography.titleLarge,
                    )
                    IconButton(onClick = onClose, modifier = Modifier.size(48.dp)) {
                        Icon(Icons.Default.Close, "Close the Touch Gamepad menu")
                    }
                }

                profileName?.let {
                    Text("Controller: $it", style = MaterialTheme.typography.titleSmall)
                }
                library?.let {
                    Text(
                        "Layout profile: ${it.selected.name}",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                layoutWarning?.let {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }

                if (profileId == TouchProfileId.Pro2) {
                    Text("Face buttons", style = MaterialTheme.typography.titleSmall)
                    Text(
                        "Chooses the letters drawn on the diamond and what each position sends.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    // Auto is not offered: it exists to guess a PRINTED legend, and a
                    // drawn control has none.
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        listOf(ControllerFaceLayout.Nintendo, ControllerFaceLayout.Xbox).forEach { option ->
                            FilterChip(
                                selected = faceLayout == option,
                                onClick = { onFaceLayout(option) },
                                label = { Text(option.title) },
                                modifier = Modifier.heightIn(min = 48.dp),
                            )
                        }
                    }
                }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onEditLayout,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text("Edit layout") }
                    OutlinedButton(
                        onClick = onProfiles,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                        enabled = library != null,
                    ) { Text("Profiles") }
                }
                OutlinedButton(
                    onClick = onRestoreDefaults,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    enabled = library?.selected?.isFactory == false,
                ) { Text("Use the default layout") }

                SettingSlider(
                    label = "Control opacity",
                    value = settings.controlOpacity,
                    range = TouchGamepadSettings.MIN_OPACITY..TouchGamepadSettings.MAX_OPACITY,
                    onValue = { onSettings(settings.copy(controlOpacity = it)) },
                )
                SettingSlider(
                    label = "Stick deadzone",
                    value = settings.stickDeadzone,
                    range = 0f..TouchGamepadSettings.MAX_DEADZONE,
                    onValue = { onSettings(settings.copy(stickDeadzone = it)) },
                )
                SettingSlider(
                    label = "Background dim",
                    value = settings.backgroundDim,
                    range = 0f..TouchGamepadSettings.MAX_DIM,
                    onValue = { onSettings(settings.copy(backgroundDim = it)) },
                )

                Row(
                    Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    // The explanation is three lines long and would otherwise run
                    // right up against the switch.
                    Column(Modifier.weight(1f).padding(end = 12.dp)) {
                        // Named for the outcome, not the gesture: the gesture is
                        // three parts now, and the padlock badge is the word the
                        // rest of the surface already uses for it.
                        Text("Lock a button held", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "Double-tap a supported button, keep the second press down until " +
                                "it ticks, then slide your finger away to leave it held. Tap a " +
                                "held button to press it again without letting go, or press and " +
                                "hold it to let go.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(
                        checked = settings.doubleTapHold,
                        onCheckedChange = { onSettings(settings.copy(doubleTapHold = it)) },
                    )
                }

                Row(
                    Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text("Touch feedback", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "A short local buzz when a control is pressed. Console rumble is separate.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(
                        checked = settings.hapticsEnabled,
                        onCheckedChange = { onSettings(settings.copy(hapticsEnabled = it)) },
                    )
                }

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(
                        onClick = onPickBackground,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text("Background image") }
                    if (settings.backgroundImage != null) {
                        OutlinedButton(
                            onClick = onClearBackground,
                            modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                        ) { Text("Remove") }
                    }
                }

                Button(
                    onClick = onExit,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                ) { Text("Exit Touch Gamepad") }
            }
        }
    }
}

/**
 * What the editor's pointer handler is allowed to do.
 *
 * Read through a [State] by the gesture loop so the long-lived pointer coroutine
 * always calls the CURRENT callbacks. Bundling them keeps that one indirection
 * in one place instead of six.
 */
private class TouchEditorGestures(
    val onTap: (String?) -> Unit,
    val onLongPress: (String) -> Unit,
    val onDragStart: (String) -> Unit,
    val onMove: (ResolvedTouchLayout, Float, Float) -> Unit,
    val onZoom: (Float) -> Unit,
    val onGestureEnd: () -> Unit,
)

/**
 * Direct manipulation: tap to select, drag to move, pinch to resize, long-press
 * to extend the selection.
 *
 * Written as one gesture loop rather than composed from `detectTapGestures` plus
 * `detectTransformGestures` because those two compete for the same down event:
 * whichever consumes first decides, and the loser silently stops working. One
 * loop also makes the transitions explicit, in particular the pointer-count
 * change at the end of a pinch — the centroid jumps there, and forwarding that
 * jump as movement would fling the control across the screen when a second
 * finger lifts.
 *
 * The layout is passed in as [State] and NOT used as a `pointerInput` key: a
 * drag mutates the draft, which re-resolves the layout, which would restart the
 * gesture on the very next frame.
 */
private fun Modifier.editTouchLayout(
    key: Any?,
    region: TouchLayoutRegion,
    layout: State<ResolvedTouchLayout>,
    gestures: State<TouchEditorGestures>,
): Modifier = pointerInput(key, region) {
    val slop = viewConfiguration.touchSlop
    val longPressTimeout = viewConfiguration.longPressTimeoutMillis
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        val target = layout.value.pick(down.position.x, down.position.y)
        if (target == null) {
            // Empty space clears the selection, but only on a real tap: a stray
            // drag over the background should not deselect what is being worked on.
            var moved = false
            var travel = Offset.Zero
            while (true) {
                val event = awaitPointerEvent(PointerEventPass.Main)
                travel += event.calculatePan()
                if (travel.getDistance() > slop) moved = true
                if (event.changes.none { it.pressed }) break
            }
            if (!moved) gestures.value.onTap(null)
            gestures.value.onGestureEnd()
            return@awaitEachGesture
        }

        var travel = Offset.Zero
        var pastSlop = false
        var released = false
        var multiPointer = false

        // A long press is "held still, one finger, for the platform's timeout".
        // Anything else ends the wait early and falls through to drag or tap.
        val settled = withTimeoutOrNull(longPressTimeout) {
            var waiting = true
            while (waiting) {
                val event = awaitPointerEvent(PointerEventPass.Main)
                if (event.changes.count { it.pressed } > 1) {
                    multiPointer = true
                    waiting = false
                } else {
                    travel += event.calculatePan()
                    if (travel.getDistance() > slop) pastSlop = true
                    if (event.changes.none { it.pressed }) released = true
                    if (released || pastSlop) waiting = false
                }
            }
            true
        }
        if (settled == null) {
            gestures.value.onLongPress(target)
        } else if (released) {
            gestures.value.onTap(target)
            gestures.value.onGestureEnd()
            return@awaitEachGesture
        }

        if (pastSlop || multiPointer) gestures.value.onDragStart(target)

        var previousPressed = if (multiPointer) 2 else 1
        while (true) {
            val event = awaitPointerEvent(PointerEventPass.Main)
            val pressed = event.changes.count { it.pressed }
            if (pressed == 0) break
            val countChanged = pressed != previousPressed
            previousPressed = pressed
            if (pressed > 1) {
                val zoom = event.calculateZoom()
                if (!countChanged && zoom.isFinite() && zoom > 0f && zoom != 1f) {
                    gestures.value.onZoom(zoom)
                }
                event.changes.forEach { if (it.positionChanged()) it.consume() }
                continue
            }
            val pan = event.calculatePan()
            travel += pan
            if (!pastSlop && travel.getDistance() > slop) {
                pastSlop = true
                gestures.value.onDragStart(target)
            }
            // The centroid moves when a second finger lifts. That is not the
            // user moving anything, so it is never forwarded as movement.
            if (pastSlop && !countChanged && (pan.x != 0f || pan.y != 0f)) {
                gestures.value.onMove(layout.value, pan.x, pan.y)
                event.changes.forEach { if (it.positionChanged()) it.consume() }
            }
        }
        if (!pastSlop && !multiPointer && settled != null) gestures.value.onTap(target)
        gestures.value.onGestureEnd()
    }
}

/**
 * Which control a point belongs to, by the SAME rule the input router uses.
 *
 * Editing and playing must agree about what is under a thumb; if they disagree,
 * the user drags one control and plays another.
 */
internal fun ResolvedTouchLayout.pick(x: Float, y: Float): String? {
    var best: ResolvedTouchControl? = null
    var bestDistance = Float.MAX_VALUE
    controls.forEach { control ->
        if (!control.hitTest(x, y)) return@forEach
        val current = best
        val distance = control.normalizedDistance(x, y)
        if (current == null || control.spec.priority > current.spec.priority ||
            control.spec.priority == current.spec.priority && distance < bestDistance
        ) {
            best = control
            bestDistance = distance
        }
    }
    return best?.id
}

/** Which named prompt is open; the editor and the menu share all four. */
private sealed interface TouchProfileNameRequest {
    val initial: String

    data class Create(override val initial: String) : TouchProfileNameRequest
    data class Duplicate(val profileId: String, override val initial: String) : TouchProfileNameRequest
    data class Rename(val profileId: String, override val initial: String) : TouchProfileNameRequest
    data class SaveAsNew(override val initial: String) : TouchProfileNameRequest
}

/** What the user was trying to do when an unsaved draft got in the way. */
private sealed interface TouchEditorConfirm {
    data object Exit : TouchEditorConfirm
    data class SelectProfile(val profileId: String) : TouchEditorConfirm
}

@Composable
private fun SettingSlider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValue: (Float) -> Unit,
) {
    Column {
        Text(
            "$label  ${(value * 100).toInt()}%",
            style = MaterialTheme.typography.bodyMedium,
        )
        Slider(
            value = value,
            onValueChange = onValue,
            valueRange = range,
            modifier = Modifier.semantics { contentDescription = label },
        )
    }
}

/** Decoded off the contact path, and only when the stored copy actually changes. */
@Composable
private fun rememberTouchBackground(path: String?): ImageBitmap? {
    var image by remember(path) { mutableStateOf<ImageBitmap?>(null) }
    LaunchedEffect(path) {
        image = if (path == null) null else withContext(Dispatchers.IO) { TouchBackgroundStore.load(path) }
    }
    return image
}

@Composable
private fun touchControlPalette(): TouchControlPalette {
    val scheme = MaterialTheme.colorScheme
    return remember(scheme) {
        TouchControlPalette(
            idle = scheme.surfaceVariant,
            idleOutline = scheme.outline,
            pressed = scheme.primary,
            pressedOutline = scheme.primary,
            label = scheme.onSurface,
            pressedLabel = scheme.onPrimary,
            disabled = scheme.outline,
        )
    }
}

private fun Context.findActivity(): Activity? {
    var current = this
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

/**
 * Where the status strip sits, as a fraction down the interaction area.
 *
 * Inside the quiet middle band the layout deliberately keeps free — below the
 * top row of shoulders and centre controls, above `-` and `+`.
 */
private const val BANNER_BAND = 0.22f

private const val CONTROLLER_DESCRIPTION =
    "On-screen controller. Sticks, D-pad, face buttons, shoulders and triggers are " +
        "positional touch controls; swipe inward from either screen edge to open the menu."
