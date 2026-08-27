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
import androidx.compose.foundation.gestures.calculateRotation
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
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.bridge.touch.ResolvedTouchControl
import dev.picoswitch.bridge.touch.ResolvedTouchLayout
import dev.picoswitch.bridge.touch.TouchAlignmentSettings
import dev.picoswitch.bridge.touch.TouchEditorAlignment
import dev.picoswitch.bridge.touch.TouchEditorDelta
import dev.picoswitch.bridge.touch.TouchEditorHistory
import dev.picoswitch.bridge.touch.TouchLayoutAudit
import dev.picoswitch.bridge.touch.TouchLayoutAuditMode
import dev.picoswitch.bridge.touch.TouchLayoutComposer
import dev.picoswitch.bridge.touch.TouchLayoutDocument
import dev.picoswitch.bridge.touch.TouchLayoutEditor
import dev.picoswitch.bridge.touch.TouchLayoutRegion
import dev.picoswitch.bridge.touch.TouchLayoutResolver
import dev.picoswitch.bridge.touch.TouchControllerProfile
import dev.picoswitch.bridge.touch.TouchLayoutProfile
import dev.picoswitch.bridge.touch.TouchProfileCatalog
import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.bridge.touch.TouchProfileLibrary
import dev.picoswitch.bridge.touch.TouchProfileLibraryEditor
import dev.picoswitch.bridge.touch.TouchReleaseReason
import dev.picoswitch.bridge.touch.TouchToolbarEdge
import dev.picoswitch.bridge.touch.TouchToolbarPlacement
import dev.picoswitch.companion.bridge.AndroidTouchFeedback
import dev.picoswitch.companion.bridge.TouchProfileSelector
import dev.picoswitch.companion.data.TouchGamepadSettings
import dev.picoswitch.companion.model.title
import dev.picoswitch.management.Personality
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
    /**
     * Editing, but with gameplay routing temporarily switched back on.
     *
     * A mode of its own rather than "editing = false", because everything the
     * editor is holding — the draft, the selection, the undo history — has to
     * survive the round trip. That short edit/play/edit loop is the difference
     * between tuning a layout and guessing at one.
     */
    var previewing by remember { mutableStateOf(false) }
    var area by remember { mutableStateOf(IntSize.Zero) }
    val profileId = ui.touchProfileId
    val profile = profileId?.let(TouchProfileCatalog::require)
    val library = ui.touchProfiles

    // Ordered so the LAST control added is the primary one: the inspector names
    // it, and alignment guides are computed from it.
    var selection by remember(profileId) { mutableStateOf<Set<String>>(emptySet()) }
    var primaryId by remember(profileId) { mutableStateOf<String?>(null) }
    var editGroup by rememberSaveable(profileId) { mutableStateOf(true) }
    /** Explicit multi-select, so long-press stays free for the toolbar handle. */
    var multiSelect by rememberSaveable(profileId) { mutableStateOf(false) }
    /**
     * The authored layout the draft started from.
     *
     * Kept explicitly rather than re-read from the UI state, because "has the
     * user changed anything" and "has the ACTIVE PROFILE changed underneath the
     * editor" are different questions and the answer to the first must not
     * change just because the second happened.
     */
    val authored = profile?.let { ui.touchLayoutDocument ?: TouchLayoutEditor.authoredDefault(it) }
    var baseline by remember(profileId) { mutableStateOf(authored) }
    /**
     * Undo/redo for this editor session.
     *
     * A stack of whole documents rather than of invertible commands: every
     * editor operation is already a pure function from one document to the next,
     * so a revision stack cannot desynchronize from what it is undoing and needs
     * no inverse written for each new operation. Coalescing a drag into one
     * entry is then a question of WHEN a revision is pushed, which the gesture
     * boundaries already answer.
     *
     * Seeded with an empty document when no personality is confirmed yet. That
     * value is never edited — the editor block below runs only once a real
     * document exists, and the effect that adopts one resets the history — it
     * exists so the type has no nullable state to reason about.
     */
    val history = remember(profileId) {
        TouchEditorHistory(authored ?: EMPTY_LAYOUT_DOCUMENT)
    }
    var draft by remember(profileId) { mutableStateOf(authored) }
    /**
     * Whether undo and redo have anything to do.
     *
     * Mirrored into composition state rather than read off [history] directly:
     * the history is a mutable object with a stable identity, so nothing about
     * pushing a revision would tell Compose to redraw the two buttons that
     * report it. Every path that touches the history goes through
     * [syncHistoryState], so the mirror cannot drift.
     */
    var canUndo by remember(profileId) { mutableStateOf(false) }
    var canRedo by remember(profileId) { mutableStateOf(false) }
    var profilesOpen by remember { mutableStateOf(false) }
    var addControlOpen by remember { mutableStateOf(false) }
    var inspectorOpen by remember { mutableStateOf(false) }
    var nameRequest by remember { mutableStateOf<TouchProfileNameRequest?>(null) }
    var pendingConfirm by remember { mutableStateOf<TouchEditorConfirm?>(null) }
    var deletedNotice by remember { mutableStateOf<TouchDeleteNotice?>(null) }
    /** True while a finger is actually moving or resizing something. */
    var manipulating by remember { mutableStateOf(false) }
    /**
     * The document as it was when the live gesture began.
     *
     * A continuous drag, pinch or rotation is ONE undo entry, so the working
     * document is mutated freely while a finger is down and a single revision is
     * pushed when it lifts. Pushing per pointer frame would fill the history
     * with sixty indistinguishable steps and make undo useless exactly when it
     * is most wanted.
     *
     * Declared beside [manipulating] because the effect that cleans up an
     * interrupted gesture has to clear both, and that effect runs long before
     * the gesture handlers are built.
     */
    var gestureStart by remember { mutableStateOf<TouchLayoutDocument?>(null) }
    var gestureLabel by remember { mutableStateOf("Move") }
    /** True while a rotation is currently held on a snap target; buzz on acquisition only. */
    var rotationSnapped by remember { mutableStateOf(false) }
    /**
     * The angle the live gesture has described, before snapping.
     *
     * The gesture's own memory, kept here because the DOCUMENT cannot hold it:
     * a snapped rotation is deliberately not what the fingers asked for, so the
     * stored angle is the wrong thing to add the next frame's delta to. Seeded
     * from the reference control when a rotation starts and cleared with the
     * rest of the gesture state; see [TouchLayoutEditor.snappedRotationDelta].
     */
    var rotationIntent by remember { mutableStateOf<Float?>(null) }
    val dirty = editing && draft != baseline

    fun syncHistoryState() {
        canUndo = history.canUndo
        canRedo = history.canRedo
    }

    /** Adopt a new revision and keep the undo affordances in step with it. */
    fun commit(next: TouchLayoutDocument, label: String) {
        if (next == history.current) return
        history.push(next, label)
        draft = next
        syncHistoryState()
    }

    /** Replace the working document outright; the history it had no longer applies. */
    fun resetDraft() {
        val document = baseline
        draft = document
        if (document != null) history.reset(document)
        syncHistoryState()
        selection = emptySet()
        primaryId = null
        deletedNotice = null
    }

    LaunchedEffect(profileId) {
        editing = false
        previewing = false
        resetDraft()
    }
    // A profile switch, a save or an import replaces the authored layout. Adopt
    // it whenever the draft has nothing of its own to lose; a dirty draft is
    // left alone until the user has answered for it, because silently replacing
    // an edit in progress is how an editor loses somebody's work.
    LaunchedEffect(authored) {
        if (!editing || draft == baseline) {
            draft = authored
            authored?.let(history::reset)
            syncHistoryState()
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
            // The published LEVEL, so a partially pulled trigger is drawn at the
            // depth the console is actually being told, detent cap included.
            analogTriggers = snapshot.analogTriggers,
            // And which way that level grows, decided by the engine off the axis
            // it froze for the gesture in progress.
            analogTriggerFills = snapshot.analogTriggerFills,
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

    /**
     * The three modes the surface can be in, and what each one owns.
     *
     * ```text
     * Gameplay    touches produce controller input from the SAVED layout
     * Edit        touches manipulate scene objects; no controller output at all
     * Preview     touches produce controller input from the WORKING draft
     * ```
     *
     * Kept as two derived booleans rather than one flag because the questions
     * differ: "is the draft the thing on screen" is true in Edit and Preview,
     * while "may a touch reach the console" is true in Gameplay and Preview.
     * Conflating them is precisely how an edit drag becomes an A press.
     */
    val showingDraft = editing
    val playable = !editing || previewing

    val composition = remember(profile, ui.touchLayoutDocument, draft, showingDraft) {
        profile?.let {
            TouchLayoutComposer.compose(it, if (showingDraft) draft else ui.touchLayoutDocument)
        }
    }
    val attemptedResolved = remember(
        area, left, top, right, bottom, density.density, composition, showingDraft,
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
                if (showingDraft) TouchLayoutAuditMode.UserDraft else TouchLayoutAuditMode.Runtime,
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
    val runtimeFallback = !showingDraft && ui.touchLayoutDocument != null &&
        !attemptedResolved.fits && defaultResolved.fits
    val resolved = if (runtimeFallback) defaultResolved else attemptedResolved
    val layoutWarning = ui.touchLayoutWarning ?: composition?.warning ?: if (runtimeFallback) {
        "Your saved layout was unsafe here, so the immutable default is active: ${attemptedResolved.problem}"
    } else null

    // Geometry changed: every retained contact position was measured against the
    // previous rectangle and means nothing now.
    var installedProfileId by remember { mutableStateOf<TouchProfileId?>(null) }
    LaunchedEffect(resolved, profileId, playable) {
        // Installing the layout ALSO releases every held contact, which is what
        // makes each mode transition safe in both directions: leaving preview
        // cannot leave a button down, and entering it starts from neutral.
        if (playable) {
            val reason = if (installedProfileId != null && installedProfileId != profileId) {
                TouchReleaseReason.PersonalityChanged
            } else {
                TouchReleaseReason.GeometryInvalidated
            }
            gamepad.setLayout(resolved, reason)
            installedProfileId = profileId
        } else {
            gamepad.release(TouchReleaseReason.EditorEntered)
        }
        visual.value = TouchVisualState(
            enabled = playable && profile != null && ui.bridge.phase == BridgeLinkPhase.Playing,
        )
    }

    // Every way an editor gesture can end WITHOUT its own end callback: the
    // window resized, the mode changed, preview opened. A gesture torn down mid
    // flight would otherwise leave the chrome permanently faded and, worse,
    // leave `gestureStart` holding a document from before the interruption --
    // so the NEXT drag would coalesce into one undo step reaching back across
    // it. Clearing all three here is what makes a torn-down gesture equivalent
    // to a cancelled one.
    LaunchedEffect(editing, previewing, resolved.region) {
        manipulating = false
        gestureStart = null
        rotationSnapped = false
        rotationIntent = null
    }

    // Platform gesture timing, not invented constants: the same numbers every
    // other double tap on this device uses, including whatever the user's
    // accessibility settings have done to them. Set here rather than in the view
    // model because this is the layer that has a view configuration at all.
    LaunchedEffect(viewConfiguration, density) {
        gamepad.setConfig(
            gamepad.config.copy(
                latch = gamepad.config.latch.copy(
                    doubleTapWindowNanos = viewConfiguration.doubleTapTimeoutMillis * NANOS_PER_MILLI,
                    minTapGapNanos = viewConfiguration.doubleTapMinTimeMillis * NANOS_PER_MILLI,
                    maxTapDurationNanos = viewConfiguration.longPressTimeoutMillis * NANOS_PER_MILLI,
                ),
                // The device's own drag slop, for the same reason as the timings
                // above: starting a trigger pull should take exactly as much
                // movement as starting any other drag on this device. Reported in
                // pixels, stored in logical units, because the portable layer's
                // distances are physical ones.
                trigger = gamepad.config.trigger.copy(
                    dragSlopUnits = viewConfiguration.touchSlop / density.density,
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
        if (previewing) {
            // Back out of a test run into the editor, not out of the editor.
            previewing = false
            return@BackHandler
        }
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
    val editingCanvas = editing && !previewing
    val effectiveTargets = remember(draft, selection, editGroup) {
        draft?.let { TouchLayoutEditor.expand(it, selection, editGroup) }.orEmpty()
    }
    val alignment = remember(settings.editorGrid, settings.editorSnap) {
        TouchAlignmentSettings(grid = settings.editorGrid, snap = settings.editorSnap)
    }
    val gridLines = remember(resolved.region, alignment, editingCanvas) {
        if (editingCanvas) TouchEditorAlignment.gridLines(resolved.region, alignment) else emptyList()
    }
    val guides = remember(resolved, effectiveTargets, primaryId, alignment, editingCanvas) {
        if (editingCanvas) {
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
                selection = when {
                    id == null -> emptySet()
                    // Explicit multi-select: tapping toggles membership rather
                    // than replacing the selection, so long-press stays free for
                    // the toolbar handle and nothing is hidden behind a gesture.
                    multiSelect && id in selection -> selection - id
                    multiSelect -> selection + id
                    else -> setOf(id)
                }
                primaryId = if (id != null && id in selection) id else selection.lastOrNull()
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
                if (gestureStart == null) gestureStart = draft
            },
            onGestureEnd = {
                manipulating = false
                rotationSnapped = false
                // The next gesture reseeds from whatever it is then pointed at.
                rotationIntent = null
                val before = gestureStart
                gestureStart = null
                val after = draft
                // One revision for the whole gesture.
                //
                // A drag mutates `draft` directly and never touches the history,
                // so `history.current` is still the document the gesture started
                // from: pushing the endpoint here is all that is needed, and
                // `commit` drops it if the gesture ended where it began. It must
                // NOT reset the history first — reset clears the undo stack, so
                // doing that here destroyed every earlier step the moment the
                // user dragged anything.
                if (before != null && after != null && after != before) {
                    commit(after, gestureLabel)
                }
            },
            onMove = { layout, deltaX, deltaY ->
                val current = draft
                val active = primaryId
                if (current != null && active != null) {
                    gestureLabel = "Move"
                    val adjusted = TouchEditorAlignment.snap(
                        layout = layout,
                        selection = effectiveTargets,
                        primaryId = active,
                        delta = TouchEditorDelta(deltaX, deltaY),
                        settings = alignment,
                    )
                    draft = TouchLayoutEditor.move(
                        current,
                        layout,
                        selection,
                        adjusted.x,
                        adjusted.y,
                        editGroup,
                    )
                }
            },
            onTransform = { factor, degrees ->
                manipulating = true
                if (gestureStart == null) gestureStart = draft
                val current = draft
                if (current != null && selection.isNotEmpty()) {
                    gestureLabel = if (degrees != 0f) "Rotate" else "Resize"
                    var next = current
                    if (factor != 1f) {
                        next = TouchLayoutEditor.scaleBy(
                            next, layoutState.value, selection, factor, editGroup,
                        )
                    }
                    if (degrees != 0f) {
                        // Magnetic, not discrete: every angle in between is kept
                        // and only the useful ones pull. Computed from the one
                        // reference control and applied to the whole selection,
                        // or a cluster's members would all snap onto the same
                        // angle and stop being a composition.
                        //
                        // The raw angle accumulates HERE rather than being
                        // re-derived from the snapped result, which is what lets
                        // a control leave the detent it started in.
                        val intent = (
                            rotationIntent
                                ?: primaryId?.let { next.instance(it) }?.rotationDegrees
                                ?: 0f
                            ) + degrees
                        rotationIntent = intent
                        val applied = TouchLayoutEditor.snappedRotationDelta(
                            next, primaryId, degrees, intent,
                        )
                        if (applied != degrees && !rotationSnapped) {
                            rotationSnapped = true
                            haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        } else if (applied == degrees) {
                            rotationSnapped = false
                        }
                        next = TouchLayoutEditor.rotateBy(
                            next, layoutState.value, selection, applied, editGroup,
                        )
                    }
                    draft = next
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

        val unusable = profile == null || !resolved.fits && (!editingCanvas || resolved.regionTooSmall)
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
                        } else if (editingCanvas) {
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
                if (editingCanvas) {
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
                personality = ui.snapshot.personality.current,
                busy = ui.busy,
                profileId = profileId,
                profileName = profile?.displayName,
                library = library,
                layoutWarning = layoutWarning,
                onSettings = viewModel::setTouchSettings,
                onPersonality = { menuOpen = false; viewModel.switchTouchPersonality(it) },
                onEditLayout = {
                    profile?.let {
                        resetDraft()
                        editing = true
                        previewing = false
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

        if (editing && profile != null && draft != null && library != null &&
            !resolved.regionTooSmall
        ) {
            val working = requireNotNull(draft)
            val activeProfile = library.selected
            val landscape = resolved.region.width >= resolved.region.height

            /** Commit the draft; the library decides whether that needs a new profile. */
            fun persist(targetId: String, newName: String) {
                val findings = TouchLayoutAudit.audit(
                    requireNotNull(composition).layout,
                    attemptedResolved.controls,
                    attemptedResolved.region,
                    profile,
                    TouchLayoutAuditMode.UserDraft,
                )
                if (findings.none { it.blocking } && attemptedResolved.fits) {
                    viewModel.saveTouchLayout(working, targetId, newName)
                    editing = false
                    previewing = false
                }
            }

            fun requestSave() {
                if (activeProfile.isFactory && working != TouchLayoutEditor.authoredDefault(profile)) {
                    // The shipped layout is never written. Name the copy instead
                    // of failing, so the edit that has just been made survives.
                    nameRequest = TouchProfileNameRequest.SaveAsNew(
                        TouchProfileLibraryEditor.DEFAULT_NEW_PROFILE_NAME,
                    )
                } else {
                    persist(activeProfile.id, TouchProfileLibraryEditor.DEFAULT_NEW_PROFILE_NAME)
                }
            }

            /** Apply one immediate (non-gesture) operation as its own undo step. */
            fun apply(label: String, transform: (TouchLayoutDocument) -> TouchLayoutDocument) {
                commit(transform(requireNotNull(draft)), label)
            }

            val editorActions = object : TouchEditorActions {
                override fun setEditGroup(value: Boolean) { editGroup = value }

                override fun setMultiSelect(value: Boolean) {
                    multiSelect = value
                    // Leaving multi-select keeps only the control the user last
                    // touched, so the inspector never names a set that is no
                    // longer being edited as a set.
                    if (!value) {
                        primaryId?.let { selection = setOf(it) } ?: run { selection = emptySet() }
                    }
                }

                override fun setGrid(value: Boolean) {
                    viewModel.setTouchSettings(settings.copy(editorGrid = value))
                }

                override fun setSnap(value: Boolean) {
                    viewModel.setTouchSettings(settings.copy(editorSnap = value))
                }

                override fun setToolbar(placement: TouchToolbarPlacement) {
                    viewModel.setTouchSettings(settings.withEditorToolbar(landscape, placement))
                }

                override fun nudgeScale(factor: Float) {
                    if (selection.isEmpty()) return
                    apply("Resize") {
                        TouchLayoutEditor.scaleBy(it, resolved, selection, factor, editGroup)
                    }
                }

                override fun nudgeRotation(degrees: Float) {
                    if (selection.isEmpty()) return
                    apply("Rotate") {
                        TouchLayoutEditor.rotateBy(it, resolved, selection, degrees, editGroup)
                    }
                }

                override fun resetRotation() {
                    if (selection.isEmpty()) return
                    apply("Reset orientation") {
                        TouchLayoutEditor.resetRotation(it, selection, editGroup)
                    }
                }

                override fun duplicate() {
                    if (selection.isEmpty()) return
                    val result = TouchLayoutEditor.duplicate(working, selection, editGroup)
                    if (!result.changed) return
                    commit(result.document, "Duplicate")
                    selection = result.created.toSet()
                    primaryId = result.created.lastOrNull()
                }

                override fun delete() {
                    if (selection.isEmpty()) return
                    // The EFFECTIVE targets, not the tapped ids. With whole-group
                    // editing on, one tap deletes a whole cluster, and counting
                    // the taps had a destructive action under-report itself:
                    // four face buttons vanished and the notice said "A deleted".
                    val removed = TouchLayoutEditor.expand(working, selection, editGroup)
                    val result = TouchLayoutEditor.delete(working, selection, editGroup)
                    if (!result.changed) return
                    // The document BEFORE the delete is what Undo restores, and
                    // it restores every field -- transform, rotation, group, z
                    // order, latch -- because a revision is the whole scene.
                    deletedNotice = TouchDeleteNotice(
                        message = if (removed.size == 1) {
                            val id = removed.single()
                            val name = working.instance(id)
                                ?.let { profile.catalogEntry(it.catalogId) }
                                ?.let { controlTitle(profile, it, id) }
                            if (name != null) "$name deleted" else "Control deleted"
                        } else {
                            "${removed.size} controls deleted"
                        },
                        restore = working,
                    )
                    commit(result.document, "Delete")
                    selection = emptySet()
                    primaryId = null
                }

                override fun group() {
                    val result = TouchLayoutEditor.group(working, selection)
                    if (result.changed) commit(result.document, "Group")
                }

                override fun ungroup() {
                    val result = TouchLayoutEditor.ungroup(working, effectiveTargets)
                    if (result.changed) commit(result.document, "Ungroup")
                }

                override fun bringForward() {
                    if (selection.isEmpty()) return
                    apply("Bring forward") {
                        TouchLayoutEditor.bringForward(it, selection, editGroup)
                    }
                }

                override fun sendBackward() {
                    if (selection.isEmpty()) return
                    apply("Send backward") {
                        TouchLayoutEditor.sendBackward(it, selection, editGroup)
                    }
                }

                /**
                 * Step to a neighbouring revision, and drop anything selected
                 * that no longer exists there.
                 *
                 * Undoing an Add would otherwise leave the inspector acting on
                 * an instance that is not in the layout — every operation would
                 * silently do nothing, which looks exactly like a frozen editor.
                 */
                private fun step(next: TouchLayoutDocument?) {
                    val document = next ?: return
                    draft = document
                    syncHistoryState()
                    selection = selection.filterTo(mutableSetOf()) {
                        document.instance(it) != null
                    }
                    primaryId = primaryId?.takeIf { document.instance(it) != null }
                    deletedNotice = null
                }

                override fun undo() = step(history.undo())

                override fun redo() = step(history.redo())

                override fun setLatch(latch: Boolean?) {
                    if (selection.isEmpty()) return
                    apply("Hold setting") {
                        TouchLayoutEditor.setLatch(it, profile, selection, latch, editGroup)
                    }
                }

                override fun resetSelection() {
                    if (selection.isEmpty()) return
                    apply("Reset control") {
                        TouchLayoutEditor.reset(it, profile, selection, editGroup)
                    }
                }

                override fun resetProfile() {
                    commit(TouchLayoutEditor.resetAll(profile), "Reset layout")
                    selection = emptySet()
                    primaryId = null
                }

                override fun openProfiles() { profilesOpen = true }

                override fun openAddControl() { addControlOpen = true }

                override fun openInspector() { inspectorOpen = primaryId != null }

                override fun preview() {
                    // Editor-owned contacts end with the gesture surface; the
                    // layout install on the way in neutralizes anything else.
                    selection = emptySet()
                    primaryId = null
                    manipulating = false
                    previewing = true
                }

                override fun save() = requestSave()

                override fun exit() {
                    if (dirty) pendingConfirm = TouchEditorConfirm.Exit else editing = false
                }
            }

            if (!previewing) {
                TouchEditorChrome(
                    state = TouchEditorUiState(
                        profile = profile,
                        document = working,
                        selection = selection,
                        effectiveTargets = effectiveTargets,
                        primaryId = primaryId,
                        placement = settings.editorToolbar(landscape),
                        editGroup = editGroup,
                        multiSelect = multiSelect,
                        grid = settings.editorGrid,
                        snap = settings.editorSnap,
                        profileName = activeProfile.name,
                        canSave = attemptedResolved.fits,
                        canUndo = canUndo,
                        canRedo = canRedo,
                        // Read off the SAME resolved layout the canvas paints
                        // red and the audit refuses, so the three cannot
                        // disagree about whether this control fits.
                        selectionProblem = primaryId?.let(attemptedResolved::problemFor),
                        dirty = dirty,
                        manipulating = manipulating,
                    ),
                    region = resolved.region,
                    actions = editorActions,
                )

                deletedNotice?.let { notice ->
                    TouchUndoSnackbar(
                        message = notice.message,
                        // Both are bottom-anchored, so a bottom-docked toolbar
                        // and this notice land on top of each other -- and the
                        // buttons it covers are Delete, Undo and Redo, which is
                        // exactly what a user deleting several controls in a row
                        // reaches for next.
                        clearBottomToolbar = settings.editorToolbar(landscape) ==
                            TouchToolbarPlacement.Docked(TouchToolbarEdge.Bottom),
                        onUndo = {
                            commit(notice.restore, "Undo delete")
                            deletedNotice = null
                        },
                        onDismiss = { deletedNotice = null },
                    )
                }
            } else {
                TouchPreviewBanner(onDone = { previewing = false })
            }

            if (addControlOpen) {
                TouchAddControlDialog(
                    profile = profile,
                    document = working,
                    onAdd = { catalogId ->
                        val result = TouchLayoutEditor.add(
                            working,
                            profile,
                            catalogId,
                            // The visible centre is the fallback placement, used
                            // whenever the authored home is already occupied.
                            0.5f,
                            0.5f,
                        )
                        if (result.changed) {
                            commit(result.document, "Add control")
                            selection = result.created.toSet()
                            primaryId = result.created.lastOrNull()
                        }
                    },
                    onDismiss = { addControlOpen = false },
                )
            }

            if (inspectorOpen) {
                primaryId?.let { id ->
                    TouchControlInspectorDialog(
                        profile = profile,
                        document = working,
                        instanceId = id,
                        onApply = { x, y, scale, rotation ->
                            var next = TouchLayoutEditor.place(working, setOf(id), x, y)
                            next = TouchLayoutEditor.setScale(next, setOf(id), scale, editGroup = false)
                            next = TouchLayoutEditor.setRotation(next, setOf(id), rotation)
                            commit(next, "Precise edit")
                        },
                        onDismiss = { inspectorOpen = false },
                    )
                }
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
                            persist(TouchProfileLibrary.FACTORY_PROFILE_ID, name)
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
        modifier = modifier.padding(SPACE_M).widthIn(max = NOTICE_MAX_WIDTH),
        // A container colour rather than a translucent surface: over a black
        // background or a dark photograph, "surface at 86%" is indistinguishable
        // from nothing at all, and an unreadable status is worse than none.
        color = if (failed) MaterialTheme.colorScheme.errorContainer
        else MaterialTheme.colorScheme.secondaryContainer,
        contentColor = if (failed) MaterialTheme.colorScheme.onErrorContainer
        else MaterialTheme.colorScheme.onSecondaryContainer,
        shape = MaterialTheme.shapes.large,
        tonalElevation = ELEVATION,
    ) {
        Row(
            Modifier
                // A trailing button brings its own inset, so the end padding is
                // smaller when there is one. Without it the strip would be
                // visibly heavier on the right than on the left.
                .padding(start = SPACE_XL, end = if (working) SPACE_XL else SPACE_S)
                .semantics { liveRegion = LiveRegionMode.Polite },
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(SPACE_M),
        ) {
            when {
                working -> CircularProgressIndicator(
                    Modifier.size(NOTICE_ICON),
                    strokeWidth = 2.dp,
                    color = LocalContentColor.current,
                )
                failed -> Icon(Icons.Default.Warning, null, Modifier.size(NOTICE_ICON))
                else -> Icon(Icons.Default.LinkOff, null, Modifier.size(NOTICE_ICON))
            }
            Text(text, style = MaterialTheme.typography.labelLarge)
            // A link that is not going to fix itself must be actionable from
            // here. Leaving the only recovery outside the mode would mean the
            // answer to "it says it cannot connect" is "leave and come back".
            if (!working) {
                TextButton(onClick = onRetry, modifier = Modifier.heightIn(min = TOUCH_TARGET)) {
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
    personality: Personality,
    busy: Boolean,
    profileId: TouchProfileId?,
    profileName: String?,
    library: TouchProfileLibrary?,
    layoutWarning: String?,
    onSettings: (TouchGamepadSettings) -> Unit,
    onPersonality: (Personality) -> Unit,
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
                Modifier.padding(SPACE_XL).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(SPACE_L),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "Touch Gamepad",
                        Modifier.weight(1f),
                        style = MaterialTheme.typography.titleLarge,
                    )
                    IconButton(onClick = onClose, modifier = Modifier.size(TOUCH_TARGET)) {
                        Icon(Icons.Default.Close, "Close the Touch Gamepad menu")
                    }
                }

                /**
                 * The emulated controller itself, changed through the real
                 * adapter lifecycle.
                 *
                 * This replaces the old Nintendo/Xbox face-layout toggle, which
                 * asked which LETTERS to draw on one personality's diamond. That
                 * question stopped being a product-level one once the surface
                 * could present four genuine controllers: the letters follow the
                 * controller, and asking for them separately made it possible to
                 * label a Joy-Con with an Xbox diamond.
                 *
                 * The heading is the only text. Four named chips with one
                 * selected do not need a sentence saying they choose a
                 * controller; what they DO need is the re-enumeration warning
                 * below, which is a consequence the chips cannot show.
                 */
                Text("Controller", style = MaterialTheme.typography.titleSmall)
                // BOTH arrangements. A flow row spaces its wrapped LINES only if
                // asked to, so with horizontal spacing alone the fourth chip
                // sat flush against the first row and the list read as three
                // controllers and an afterthought.
                FlowRow(
                    horizontalArrangement = Arrangement.spacedBy(SPACE_M),
                    verticalArrangement = Arrangement.spacedBy(SPACE_M),
                ) {
                    TouchProfileSelector.gameplayPersonalities.forEach { option ->
                        FilterChip(
                            selected = personality == option,
                            onClick = { if (personality != option) onPersonality(option) },
                            label = { Text(option.title) },
                            enabled = !busy,
                            modifier = Modifier.heightIn(min = TOUCH_TARGET),
                        )
                    }
                }
                // Kept: a personality switch re-enumerates USB and replaces the
                // layout, which is a consequence the chips above cannot show and
                // the user would otherwise meet as a surprise.
                Text(
                    "Switching re-enumerates USB and loads that controller's own layout.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                layoutWarning?.let {
                    Text(
                        it,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }

                library?.let {
                    Text("Layout", style = MaterialTheme.typography.titleSmall)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(SPACE_M)) {
                    Button(
                        onClick = onEditLayout,
                        modifier = Modifier.weight(1f).heightIn(min = TOUCH_TARGET),
                    ) { Text("Edit") }
                    OutlinedButton(
                        onClick = onProfiles,
                        modifier = Modifier.weight(1f).heightIn(min = TOUCH_TARGET),
                        enabled = library != null,
                    ) { Text(library?.selected?.name ?: "Profiles") }
                }
                OutlinedButton(
                    onClick = onRestoreDefaults,
                    modifier = Modifier.fillMaxWidth().heightIn(min = TOUCH_TARGET),
                    enabled = library?.selected?.isFactory == false,
                ) { Text("Use the default") }

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

                // Kept, and it is the longest text on this surface for a reason:
                // the gesture is three parts and cannot be guessed. Everything
                // else here names itself.
                SettingSwitch(
                    // Named for the outcome, not the gesture: the padlock badge
                    // is the word the rest of the surface already uses for it.
                    title = "Lock a button held",
                    detail = "Double-tap a button, hold the second press until it ticks, then " +
                        "slide away. Tap a held button to re-press it; press and hold to let go.",
                    checked = settings.doubleTapHold,
                    onChange = { onSettings(settings.copy(doubleTapHold = it)) },
                )

                SettingSwitch(
                    title = "Touch feedback",
                    detail = "Local buzz only; console rumble is separate.",
                    checked = settings.hapticsEnabled,
                    onChange = { onSettings(settings.copy(hapticsEnabled = it)) },
                )

                Row(horizontalArrangement = Arrangement.spacedBy(SPACE_M)) {
                    OutlinedButton(
                        onClick = onPickBackground,
                        modifier = Modifier.weight(1f).heightIn(min = TOUCH_TARGET),
                    ) { Text("Background") }
                    if (settings.backgroundImage != null) {
                        OutlinedButton(
                            onClick = onClearBackground,
                            modifier = Modifier.weight(1f).heightIn(min = TOUCH_TARGET),
                        ) { Text("Remove") }
                    }
                }

                Button(
                    onClick = onExit,
                    modifier = Modifier.fillMaxWidth().heightIn(min = TOUCH_TARGET),
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                ) { Text("Exit Touch Gamepad") }
            }
        }
    }
}

/**
 * A titled switch with one line of detail.
 *
 * One composable so the two settings that need it reserve the same target,
 * indent the same amount and keep the same gap from the switch — the sort of
 * thing that drifts the moment each is laid out by hand.
 */
@Composable
private fun SettingSwitch(
    title: String,
    detail: String,
    checked: Boolean,
    onChange: (Boolean) -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = TOUCH_TARGET),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = SPACE_L)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(
                detail,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(checked = checked, onCheckedChange = onChange)
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
    /** Zoom factor and rotation delta from one two-finger frame, applied together. */
    val onTransform: (Float, Float) -> Unit,
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
                // Scale AND turn from the same frame, using the toolkit's own
                // multi-touch arithmetic rather than a hand-rolled angle: two
                // fingers converging produce a degenerate centroid, and every
                // custom version of this eventually divides by it.
                val zoom = event.calculateZoom()
                val rotation = event.calculateRotation()
                if (!countChanged) {
                    val scaled = if (zoom.isFinite() && zoom > 0f) zoom else 1f
                    val turned = if (rotation.isFinite()) rotation else 0f
                    if (scaled != 1f || turned != 0f) {
                        gestures.value.onTransform(scaled, turned)
                    }
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
        if (current == null) {
            best = control
            bestDistance = control.normalizedDistance(x, y)
            return@forEach
        }
        // Priority, then z-order, then centrality: exactly the router's rule.
        // Editing and playing must agree about what is under a thumb, and once
        // controls may be freely stacked, the one drawn in front is the one the
        // user means in both modes.
        val order = compareValuesBy(control, current, { it.spec.priority }, { it.spec.zIndex })
        val distance = control.normalizedDistance(x, y)
        if (order > 0 || order == 0 && distance < bestDistance) {
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

/**
 * A just-deleted selection, and the document that brings it back.
 *
 * The whole PRE-DELETE document rather than the removed instances: restoring it
 * returns every field each control had — transform, rotation, group membership,
 * z-order, hold setting — because a revision is the whole scene rather than a
 * patch that has to remember what it touched.
 */
private data class TouchDeleteNotice(
    val message: String,
    val restore: TouchLayoutDocument,
)

/**
 * Transient confirmation with one action, over the layout.
 *
 * A snackbar rather than a confirmation dialog, deliberately: deleting a control
 * in a layout editor is an ordinary, frequent, obviously reversible act, and
 * asking "are you sure?" every time trains the user to dismiss without reading.
 *
 * Built as a surface rather than with Material's [Snackbar], which lays itself
 * out at the width it is OFFERED: over a full-screen layout that is the whole
 * window, so "A deleted UNDO" arrived as a bar most of the screen wide with the
 * two words marooned at one end. The colours are still the snackbar's, because
 * this is still a snackbar; only the measuring is ours, and it is the same
 * measuring [TouchPreviewBanner] already uses.
 */
@Composable
private fun BoxScope.TouchUndoSnackbar(
    message: String,
    clearBottomToolbar: Boolean,
    onUndo: () -> Unit,
    onDismiss: () -> Unit,
) {
    // Times out on its own so it cannot sit over the layout being edited.
    LaunchedEffect(message) {
        delay(UNDO_SNACKBAR_MILLIS)
        onDismiss()
    }
    Surface(
        modifier = Modifier
            .align(Alignment.BottomCenter)
            .windowInsetsPadding(WindowInsets.safeContent)
            .padding(
                start = SPACE_XL,
                end = SPACE_XL,
                top = SPACE_XL,
                bottom = if (clearBottomToolbar) TOOLBAR_BAND else SPACE_XL,
            )
            // A ceiling, not a width: a long message wraps instead of running
            // to the screen edges, and a short one stays short.
            .widthIn(max = NOTICE_MAX_WIDTH),
        shape = MaterialTheme.shapes.large,
        // The editor's own chrome colours, like the toolbar it sits beside. The
        // Material snackbar's inverse surface is a near-white slab here, which
        // is the one piece of chrome in this mode that would not look like the
        // rest of it.
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        contentColor = MaterialTheme.colorScheme.onSurface,
        tonalElevation = ELEVATION,
        shadowElevation = ELEVATION,
    ) {
        Row(
            Modifier
                .padding(start = SPACE_XL, end = SPACE_S)
                .semantics { liveRegion = LiveRegionMode.Polite },
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(SPACE_M),
        ) {
            Text(message, style = MaterialTheme.typography.bodyMedium)
            TextButton(onClick = onUndo, modifier = Modifier.heightIn(min = TOUCH_TARGET)) {
                Text("Undo")
            }
        }
    }
}

/**
 * The only editor chrome that survives into Preview.
 *
 * Everything else is gone — no toolbar, no outlines, no guides — because the
 * point of a test run is to see the layout as it will be played. One button
 * back, so the mode is never a place the user can get stuck.
 */
@Composable
private fun BoxScope.TouchPreviewBanner(onDone: () -> Unit) {
    Surface(
        modifier = Modifier
            .align(Alignment.TopCenter)
            .windowInsetsPadding(WindowInsets.safeContent)
            .padding(SPACE_M),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.primaryContainer,
        contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
        tonalElevation = ELEVATION,
    ) {
        Row(
            Modifier.padding(start = SPACE_XL, end = SPACE_S),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(SPACE_M),
        ) {
            Text("Testing", style = MaterialTheme.typography.labelLarge)
            TextButton(onClick = onDone, modifier = Modifier.heightIn(min = TOUCH_TARGET)) {
                Text("Back to editing")
            }
        }
    }
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
            error = scheme.error,
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

/** Long enough to notice and act on, short enough not to sit over the layout. */
private const val UNDO_SNACKBAR_MILLIS = 6_000L

/**
 * The same spacing scale the editor chrome uses, so the menu, the notices and
 * the toolbar share one rhythm rather than three sets of hand-picked numbers.
 */
private val SPACE_S = 4.dp
private val SPACE_M = 8.dp
private val SPACE_L = 12.dp
private val SPACE_XL = 16.dp

/** The 48dp accessibility floor, named once. */
private val TOUCH_TARGET = 48.dp

/** One raised surface height for every piece of editor chrome. */
private val ELEVATION = 6.dp

/**
 * The widest a transient notice may grow before its text wraps.
 *
 * A CEILING and never a width: these strips size to what they say. A fixed or
 * offered width is how "A deleted UNDO" ends up spanning a 1920-pixel window.
 */
private val NOTICE_MAX_WIDTH = 420.dp

/** The glyph in a notice strip: smaller than a button icon, larger than text. */
private val NOTICE_ICON = 16.dp

/**
 * Height a bottom-docked toolbar occupies: one button row plus its own padding
 * and the gap a notice keeps from it. What the undo notice steps over.
 */
private val TOOLBAR_BAND = 72.dp

/**
 * A placeholder document for the moment before a personality is confirmed.
 *
 * Never edited and never composed: the editor opens only once a real document
 * exists. It is here so the undo history has no nullable state, which is worth
 * more than avoiding one unused value.
 */
private val EMPTY_LAYOUT_DOCUMENT = TouchLayoutDocument(
    profileId = TouchProfileId.Pro2,
    templateId = "",
    basedOnRevision = 1,
)

private const val CONTROLLER_DESCRIPTION =
    "On-screen controller. Sticks, D-pad, face buttons, shoulders and triggers are " +
        "positional touch controls; swipe inward from either screen edge to open the menu."
