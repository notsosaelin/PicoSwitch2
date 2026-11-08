package dev.picoswitch.companion.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.focusGroup
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.FocusDirection
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import kotlinx.coroutines.delay
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/**
 * The app's navigation, at whichever edge the player chose
 * (docs/architecture/companion-shell-layout.md, "Navigation").
 *
 * Always a slide-out, never docked (owner, 2026-11-08: "the dock should
 * always just be a slide out thing", like a console's game-mode menu). Nothing
 * is left on screen while it is away: no handle, no arrow. It comes in
 * - with a controller, by moving focus off the page toward its edge, or by Back;
 * - by touch, by Back (the edge swipe, the back button);
 * and leaves by Back, by a tap on the scrim, by focus moving back to the
 * page, or by choosing a page. Starting or stopping Controller Link never
 * moves it: an earlier version docked it whenever touch drove the app and
 * not playing, so every Start / Stop made it jump in or out.
 *
 * Nothing here animates a size, and the page never moves: the navigation
 * slides over it by translation only. The first rail's judder came from
 * animating its width, which re-measured the whole page on every frame.
 */
@Composable
internal fun NavShell(
    ui: CompanionUiState,
    placement: NavPlacement,
    onSelect: (AppSection) -> Unit,
    /** Told whenever the navigation opens or closes (it decides who owns the pad while streaming). */
    onShownChange: (Boolean) -> Unit = {},
    page: @Composable (Modifier) -> Unit,
) {
    val vertical = placement.vertical
    val thickness = if (vertical) LayoutTokens.RailWidth else LayoutTokens.BarHeight
    val px = with(LocalDensity.current) { thickness.toPx() }
    // Which way the navigation travels to leave the screen.
    val away = if (placement == NavPlacement.Left || placement == NavPlacement.Top) -1f else 1f
    val focusManager = LocalFocusManager.current

    // ONE EXPLICIT STATE, CHANGED ONLY BY INTENT. It used to follow whether the
    // navigation held focus, so any focus the page dropped (a list item
    // recomposed away, a page swapped under the cursor) hid it, and the next
    // D-pad press from "nothing focused" searched from the screen's edge and
    // popped it open again — the "unpops itself randomly" of 2026-11-08.
    // Now it opens when focus enters it or Back asks, and closes only on Back,
    // a scrim tap, D-pad back toward the page, or choosing a page.
    var shown by remember { mutableStateOf(false) }
    LaunchedEffect(shown) { onShownChange(shown) }
    // Set by the intentional ways out, so a focus loss that is none of them can
    // be told apart and repaired instead of closing anything.
    var leaving by remember { mutableStateOf(false) }
    var navHasFocus by remember { mutableStateOf(false) }
    // Bumped to ask for focus back on the page (after a page change, after the
    // navigation closes, or when the page dropped it).
    var pageFocusTicket by remember { mutableIntStateOf(0) }
    val pageFocus = remember { FocusRequester() }
    // The D-pad key that points at the navigation's edge.
    val towardNavKey = when (placement) {
        NavPlacement.Right -> Key.DirectionRight
        NavPlacement.Top -> Key.DirectionUp
        NavPlacement.Bottom -> Key.DirectionDown
        else -> Key.DirectionLeft
    }
    val towardNav = when (placement) {
        NavPlacement.Right -> FocusDirection.Right
        NavPlacement.Top -> FocusDirection.Up
        NavPlacement.Bottom -> FocusDirection.Down
        else -> FocusDirection.Left
    }
    val towardPage = when (placement) {
        NavPlacement.Right -> FocusDirection.Left
        NavPlacement.Top -> FocusDirection.Down
        NavPlacement.Bottom -> FocusDirection.Up
        else -> FocusDirection.Right
    }

    val navShift = remember(placement) { Animatable(if (shown) 0f else away * px) }
    LaunchedEffect(shown, placement) { navShift.animateTo(if (shown) 0f else away * px, motionSpec()) }
    val scrim by animateFloatAsState(if (shown) 0.32f else 0f, motionSpec(), label = "nav-scrim")

    val requesters = remember { AppSection.entries.associateWith { FocusRequester() } }
    val selected = ui.section.takeIf { ui.overlay == AppOverlay.None }
    val entry = requesters.getValue(selected ?: AppSection.Play)
    val keys = keyNavigation()

    // Sends the navigation away; under a controller focus goes to the page,
    // so it is never left pointing at nothing.
    fun dismiss() {
        leaving = true
        shown = false
        if (keys) pageFocusTicket++
    }

    // Focus requests wait for the page change to settle (the outgoing page
    // fades for 120 ms), so they land in the page being shown, not the old one.
    LaunchedEffect(pageFocusTicket) {
        if (pageFocusTicket == 0) return@LaunchedEffect
        delay(PAGE_SETTLE_MS)
        if (!shown) runCatching { pageFocus.requestFocus() }
    }
    // A new page under a controller gets focus too, however it was reached
    // (L1 / R1, a link on another page).
    LaunchedEffect(ui.section) { if (keys && !shown) pageFocusTicket++ }

    // Back shows and hides the navigation on every page (owner, 2026-11-08).
    // Registered before the pages' own handlers, so a page's Back (a pane, a
    // dialog, an overlay) still wins; Back never leaves the app from inside
    // the shell and never jumps to Play.
    BackHandler(enabled = !shown && ui.overlay == AppOverlay.None) {
        leaving = false
        shown = true
        if (keys) runCatching { entry.requestFocus() }
    }
    BackHandler(enabled = shown) { dismiss() }

    Box(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
        page(
            Modifier.fillMaxSize()
                .focusRequester(pageFocus)
                .onFocusChanged { state ->
                    // The page dropped focus and nothing else took it (not the
                    // navigation, not an intentional close): give it back to the
                    // page, after a frame so a move into the navigation lands first.
                    if (!state.hasFocus && keys && !shown) pageFocusTicket++
                }
                // A CLOSED NAVIGATION IS NOT IN THE FOCUS SEARCH (its items
                // cannot take focus, NavItem). It used to be: off screen by
                // translation only, it was still laid out at the screen's edge,
                // so the first D-pad press with nothing focused (after a tap,
                // after details closed) searched from that edge and landed in
                // it. Now it opens only on purpose: a D-pad press toward its
                // edge that has nowhere left to go inside the page.
                .onKeyEvent { event ->
                    if (event.type != KeyEventType.KeyDown || event.key != towardNavKey || shown) {
                        false
                    } else if (focusManager.moveFocus(towardNav)) {
                        true
                    } else {
                        leaving = false
                        shown = true
                        runCatching { entry.requestFocus() }
                        true
                    }
                }
                .focusGroup(),
        )
        if (scrim > 0f) {
            Box(
                Modifier.fillMaxSize()
                    .background(MaterialTheme.colorScheme.scrim.copy(alpha = scrim))
                    .pointerInput(Unit) { detectTapGestures { dismiss() } },
            )
        }
        val edge = when (placement) {
            NavPlacement.Right -> Alignment.CenterEnd
            NavPlacement.Top -> Alignment.TopCenter
            NavPlacement.Bottom -> Alignment.BottomCenter
            else -> Alignment.CenterStart
        }
        val navModifier = Modifier
            .align(edge)
            .then(if (vertical) Modifier.width(thickness).fillMaxHeight() else Modifier.height(thickness).fillMaxWidth())
            .graphicsLayer { if (vertical) translationX = navShift.value else translationY = navShift.value }
            .shadow(if (shown) 12.dp else 0.dp)
            .background(MaterialTheme.colorScheme.surfaceContainerLow)
            .onFocusChanged {
                navHasFocus = it.hasFocus
                if (it.hasFocus) {
                    leaving = false
                    shown = true
                } else if (shown && !leaving && keys) {
                    // Lost without a way out being taken: take it back.
                    runCatching { entry.requestFocus() }
                }
            }
            .focusProperties {
                // Arriving from the page lands on the page being shown, never on
                // whichever item happened to be nearest the focused control.
                onEnter = { runCatching { entry.requestFocus() } }
                // Only "back toward the page" leaves. Past either end, or toward
                // the screen edge, focus stays put instead of escaping to
                // whatever is nearest.
                onExit = {
                    if (requestedFocusDirection == towardPage) {
                        leaving = true
                        shown = false
                    } else if (requestedFocusDirection in BLOCKED_EXITS) {
                        cancelFocusChange()
                    }
                }
            }
            .focusGroup()
        CompositionLocalProvider(LocalNavReachable provides { shown }) {
        NavContent(
            ui = ui,
            vertical = vertical,
            selected = selected,
            requesters = requesters,
            onSelect = { section ->
                onSelect(section)
                dismiss()
            },
            modifier = navModifier,
        )
        }
    }
}

/** Whether the navigation's items can take focus right now: only while it is out. */
private val LocalNavReachable = compositionLocalOf<() -> Boolean> { { true } }

/** The outgoing page fades for 120 ms; focus waits for it to go. */
private const val PAGE_SETTLE_MS = 140L

private val BLOCKED_EXITS = setOf(FocusDirection.Up, FocusDirection.Down, FocusDirection.Left, FocusDirection.Right)

@Composable
private fun NavContent(
    ui: CompanionUiState,
    vertical: Boolean,
    selected: AppSection?,
    requesters: Map<AppSection, FocusRequester>,
    onSelect: (AppSection) -> Unit,
    modifier: Modifier,
) {
    val pages = AppSection.entries.filter { it.inDock }
    if (vertical) {
        Column(
            modifier.padding(vertical = LayoutTokens.Space2),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            AdapterDot(ui)
            Spacer(Modifier.weight(1f))
            pages.forEach { section ->
                NavItem(section, null, section == selected, requesters.getValue(section)) { onSelect(section) }
                Spacer(Modifier.height(LayoutTokens.Space2))
            }
            Spacer(Modifier.weight(1f))
            NavItem(AppSection.Settings, null, selected == AppSection.Settings, requesters.getValue(AppSection.Settings)) {
                onSelect(AppSection.Settings)
            }
        }
    } else {
        BoxWithConstraints(modifier) {
            // Full names at the top or bottom (owner, 2026-11-08). A window
            // too narrow for all of them falls back to one-word names, never
            // to an ellipsis.
            val full = maxWidth >= LayoutTokens.NavFullNamesMin
            Row(
                Modifier.fillMaxSize().padding(horizontal = LayoutTokens.Space3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                AdapterDot(ui)
                Spacer(Modifier.weight(1f))
                pages.forEach { section ->
                    NavItem(
                        section,
                        if (full) section.label else section.shortLabel,
                        section == selected,
                        requesters.getValue(section),
                    ) { onSelect(section) }
                    Spacer(Modifier.width(LayoutTokens.Space1))
                }
                Spacer(Modifier.weight(1f))
                NavItem(AppSection.Settings, null, selected == AppSection.Settings, requesters.getValue(AppSection.Settings)) {
                    onSelect(AppSection.Settings)
                }
            }
        }
    }
}

/**
 * One destination. Icon only at the side; icon and name at the top or bottom.
 * Under a controller the focused item lifts like the tiles on Play, so the
 * cursor reads the same everywhere in the app.
 */
@Composable
private fun NavItem(
    section: AppSection,
    label: String?,
    selected: Boolean,
    requester: FocusRequester,
    onClick: () -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    val reachable = LocalNavReachable.current
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val lifted = focused && keyNavigation()
    val scale by animateFloatAsState(
        if (lifted) 1.08f else 1f,
        spring(dampingRatio = 0.7f, stiffness = Spring.StiffnessMediumLow),
        label = "nav-scale",
    )
    val outline by animateColorAsState(if (lifted) colors.primary else Color.Transparent, tween(120), label = "nav-outline")
    Surface(
        onClick = onClick,
        shape = CircleShape,
        color = if (selected) colors.secondaryContainer else Color.Transparent,
        contentColor = if (selected) colors.onSecondaryContainer else colors.onSurfaceVariant,
        border = BorderStroke(2.dp, outline),
        interactionSource = interaction,
        modifier = Modifier
            // Read when focus is searched, so it is current the moment the
            // navigation opens (Back, or the D-pad at the page's edge).
            .focusProperties { canFocus = reachable() }
            .focusRequester(requester)
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
            }
            .semantics { contentDescription = section.label },
    ) {
        Row(
            Modifier
                .height(LayoutTokens.NavItemHeight)
                .widthIn(min = LayoutTokens.NavItemHeight)
                .padding(horizontal = if (label == null) 0.dp else LayoutTokens.Space3),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center,
        ) {
            Icon(section.icon, null, Modifier.size(LayoutTokens.IconSize))
            if (label != null) {
                Spacer(Modifier.width(LayoutTokens.Space2))
                Text(
                    label,
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                    maxLines = 1,
                    softWrap = false,
                )
            }
        }
    }
}

/**
 * The app's one connection indicator: a dot, with a spinner while connecting.
 * An indicator only, never a control: it is not focusable, so the D-pad never
 * stops on it (owner, 2026-11-08). The Adapter page is the item below it.
 */
@Composable
private fun AdapterDot(ui: CompanionUiState) {
    val status = AdapterStatus.of(ui.connection.connected, ui.connection.phase, ui.adapterRelationship != null)
    val name = ui.adapterRelationship?.displayName ?: ui.connection.deviceName ?: "No adapter"
    Box(
        Modifier.size(LayoutTokens.NavItemHeight).semantics(mergeDescendants = true) {
            contentDescription = "$name, ${status.words}"
            liveRegion = LiveRegionMode.Polite
        },
        contentAlignment = Alignment.Center,
    ) {
        if (AdapterStatus.inProgress(ui.connection.phase)) {
            CircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 2.dp)
        }
        StatusDot(status.tone, size = 14.dp)
    }
}

internal val AppSection.icon: ImageVector
    get() = when (this) {
        AppSection.Play -> Icons.Default.SportsEsports
        AppSection.Keyboard -> Icons.Default.Keyboard
        AppSection.Amiibo -> Icons.Default.Contactless
        AppSection.Customization -> Icons.Default.Palette
        AppSection.Adapter -> Icons.Default.Cable
        AppSection.Settings -> Icons.Default.Settings
    }

/** One word, for a top / bottom bar too narrow for the full names. */
private val AppSection.shortLabel: String
    get() = when (this) {
        AppSection.Keyboard -> "Keys"
        AppSection.Customization -> "Style"
        else -> label
    }
