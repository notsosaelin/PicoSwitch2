package dev.picoswitch.companion.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.protocol.BridgeContract
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.data.AmiiboGallery
import dev.picoswitch.companion.model.*

/**
 * Play, the home page (docs/architecture/companion-shell-layout.md, "Play"),
 * laid out like a console home screen rather than a settings form:
 *
 * - the left column in four rows (owner, 2026-11-08: the controller card was
 *   mostly empty, and the console buttons REPLACED Touch Gamepad while
 *   streaming, so there was no way into it once play started):
 *   1. the HEADSET strip: one tap plugs it in (LIVE), one tap pulls it out; the
 *      microphone comes with it and only mutes;
 *   2. the CONTROLLER row: input -> output (the controller this device reads ->
 *      what the console sees, with its Player N), the state in a few words, and
 *      one quick-settings menu (which controller, which emulated controller);
 *   3. the CONSOLE BUTTONS: Home, Capture, C as tiles, always in place, held
 *      while playing and disabled otherwise;
 *   4. the ACTIONS: Start / Stop, Touch Gamepad, Wake or the adapter's next step;
 * - the AMIIBO tile (right, full height): the figure the adapter holds.
 *
 * Everything fits the page without scrolling on a landscape handheld; only a
 * window too narrow or too short for side-by-side tiles stacks and scrolls.
 * Every target is a focusable tile or button, so a gamepad reaches all of it
 * with the D-pad and A (MainActivity, GamepadNavigation).
 */
@Composable
fun PlayScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onPrepare: () -> Unit,
    onOpenTouchGamepad: () -> Unit,
    onConnectAdapter: () -> Unit,
) {
    val playing = ui.bridge.phase == BridgeLinkPhase.Playing
    val hasAdapter = ui.adapterRelationship != null
    val host = if (ui.bridge.phase == BridgeLinkPhase.Ready) viewModel.knownControllerHost() else null
    val action = PlayAction.of(
        phase = ui.bridge.phase,
        hasAdapter = hasAdapter,
        hasController = ui.selectedSourceDescriptor != null,
        hostKnown = host != null,
        bridgeMessage = ui.bridge.message,
    )

    val onHero: () -> Unit = {
        when (action.kind) {
            PlayAction.Kind.Start -> onPrepare()
            PlayAction.Kind.Reconnect -> host?.let(viewModel::connectControllerHost)
            PlayAction.Kind.Stop -> viewModel.stopControllerBridge()
            PlayAction.Kind.Busy -> Unit
        }
    }

    BoxWithConstraints(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        val short = LocalShortWindow.current
        val gap = if (short) LayoutTokens.Space2 else LayoutTokens.Space3
        val hub = maxWidth >= HUB_MIN_WIDTH && maxHeight >= HUB_MIN_HEIGHT
        val actions: @Composable (Modifier) -> Unit = { modifier ->
            ActionRow(ui, viewModel, action, onHero, hasAdapter, onOpenTouchGamepad, onConnectAdapter, modifier)
        }
        if (hub) {
            val padTop = if (short) LayoutTokens.Space1 else LayoutTokens.PagePaddingTop
            val padBottom = if (short) LayoutTokens.Space2 else LayoutTokens.PagePaddingBottom
            // The controller row and the console tiles share the spare height
            // (1.4 : 1); the tiles stop at 140 so a tall window gives the rest
            // to the controller row, which centres its content.
            val spare = maxHeight - padTop - padBottom - HEADSET_STRIP_HEIGHT - LayoutTokens.ActionHeight - gap * 3
            val consoleHeight = (spare / 2.4f).coerceIn(72.dp, 140.dp)
            Row(
                Modifier
                    .widthIn(max = LayoutTokens.WidePageMaxWidth)
                    .fillMaxSize()
                    .padding(horizontal = if (short) LayoutTokens.Space4 else LayoutTokens.PagePaddingSide)
                    .padding(top = padTop, bottom = padBottom),
                horizontalArrangement = Arrangement.spacedBy(gap),
            ) {
                Column(Modifier.weight(1.35f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(gap)) {
                    HeadsetTileCard(viewModel, Modifier.fillMaxWidth().height(HEADSET_STRIP_HEIGHT))
                    ControllerCard(ui, viewModel, action, playing, Modifier.weight(1f).fillMaxWidth())
                    ConsoleTile(viewModel, playing, Modifier.fillMaxWidth().height(consoleHeight))
                    actions(Modifier.fillMaxWidth())
                }
                AmiiboTileCard(ui, viewModel, Modifier.weight(1f).fillMaxHeight())
            }
        } else {
            Column(
                Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = LayoutTokens.NarrowPagePaddingSide)
                    .padding(top = LayoutTokens.PagePaddingTop, bottom = LayoutTokens.PagePaddingBottom),
                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
            ) {
                actions(Modifier.fillMaxWidth())
                ControllerCard(ui, viewModel, action, playing, Modifier.fillMaxWidth())
                ConsoleTile(viewModel, playing, Modifier.fillMaxWidth().height(112.dp))
                HeadsetTileCard(viewModel, Modifier.fillMaxWidth().height(STACKED_TILE_HEIGHT))
                AmiiboTileCard(ui, viewModel, Modifier.fillMaxWidth().height(STACKED_AMIIBO_HEIGHT))
            }
        }
    }
}

private val HUB_MIN_WIDTH = 600.dp
private val HUB_MIN_HEIGHT = 280.dp
private val STACKED_TILE_HEIGHT = 120.dp
private val STACKED_AMIIBO_HEIGHT = 300.dp
private val HEADSET_STRIP_HEIGHT = 96.dp

/**
 * The controller row: input -> output at a glance, the state in a few words,
 * and the quick-settings menu. Centred in whatever height the column gives it.
 */
@Composable
private fun ControllerCard(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    action: PlayAction.View,
    playing: Boolean,
    modifier: Modifier,
) {
    val colors = MaterialTheme.colorScheme
    val short = LocalShortWindow.current
    val glow = if (playing) colors.tertiaryContainer else colors.primaryContainer
    val headline = when (action.kind) {
        PlayAction.Kind.Stop -> "On the console"
        PlayAction.Kind.Busy -> "Connecting…"
        else -> if (action.enabled) "Ready to play" else "Not ready"
    }
    Box(
        modifier
            .clip(RoundedCornerShape(LayoutTokens.CardRadius))
            .background(Brush.linearGradient(listOf(glow, colors.surfaceContainerHigh))),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.fillMaxWidth().padding(if (short) LayoutTokens.Space3 else LayoutTokens.Space4),
            verticalArrangement = Arrangement.spacedBy(if (short) LayoutTokens.Space2 else LayoutTokens.Space3),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    headline,
                    style = if (short) MaterialTheme.typography.headlineSmall else MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                QuickSettings(ui, viewModel)
            }
            InputOutput(ui)
            if (!playing) ActionLine(action, ui)
        }
    }
}

/** Input -> output: the controller this device reads, and what the console sees with its player. */
@Composable
private fun InputOutput(ui: CompanionUiState) {
    val selected = ui.sourceDevices.firstOrNull { it.descriptor == ui.selectedSourceDescriptor }
    val input = ControllerCardStatus.of(selected?.name, selected != null, ui.sourceDevices.size)
    val connected = ui.connection.connected
    val playing = ui.bridge.phase == BridgeLinkPhase.Playing
    val outputName = if (connected) ui.snapshot.personality.current.title else "No adapter"
    val outputLine = if (!connected) {
        AdapterStatus.of(false, ui.connection.phase, ui.adapterRelationship != null).words
    } else {
        ConsoleCues.playerLabel(true, ui.snapshot.input.console) ?: if (playing) "On the console" else "To the console"
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        FlowEnd(Icons.Default.Gamepad, input.name, if (input.tone == StatusTone.Good) null else input.words, accent = false, Modifier.weight(1f))
        Icon(
            Icons.Default.ChevronRight, null,
            Modifier.padding(horizontal = LayoutTokens.Space1),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        FlowEnd(Icons.Default.Gamepad, outputName, outputLine, accent = true, Modifier.weight(1f))
    }
}

@Composable
private fun FlowEnd(icon: ImageVector, name: String, line: String?, accent: Boolean, modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier.size(44.dp).background(
                if (accent) colors.primary.copy(alpha = 0.18f) else colors.surface.copy(alpha = 0.6f),
                CircleShape,
            ),
            contentAlignment = Alignment.Center,
        ) { Icon(icon, null, tint = if (accent) colors.primary else colors.onSurfaceVariant) }
        Spacer(Modifier.width(LayoutTokens.Space3))
        Column {
            Text(name, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            line?.let {
                Text(it, style = MaterialTheme.typography.bodySmall, color = colors.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

/**
 * Quick settings: which controller this device reads (and refresh), and which
 * controller the console sees. The full forms stay on the Adapter page.
 */
@Composable
private fun QuickSettings(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var open by remember { mutableStateOf(false) }
    val personalities = ui.snapshot.personality.available.ifEmpty {
        listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight)
    }
    val canSwitch = ui.connection.connected && !ui.busy &&
        ui.snapshot.capabilities.personality != CapabilityState.Unsupported
    Box {
        FilledTonalIconButton(
            onClick = { open = true },
            modifier = Modifier.size(44.dp).semantics { contentDescription = "Controller quick settings" },
        ) { Icon(Icons.Default.Settings, null) }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            DropdownMenuItem(text = { Text("Controller", style = MaterialTheme.typography.labelLarge) }, onClick = {}, enabled = false)
            ui.sourceDevices.forEach { source ->
                DropdownMenuItem(
                    text = { Text(source.name) },
                    leadingIcon = { RadioButton(selected = source.descriptor == ui.selectedSourceDescriptor, onClick = null) },
                    onClick = {
                        open = false
                        viewModel.selectSource(source.descriptor)
                    },
                )
            }
            DropdownMenuItem(
                text = { Text("Look for controllers") },
                leadingIcon = { Icon(Icons.Default.Refresh, null) },
                onClick = {
                    open = false
                    viewModel.refreshSources()
                },
            )
            HorizontalDivider()
            DropdownMenuItem(text = { Text("The console sees", style = MaterialTheme.typography.labelLarge) }, onClick = {}, enabled = false)
            personalities.forEach { personality ->
                DropdownMenuItem(
                    text = { Text(personality.title) },
                    leadingIcon = { RadioButton(selected = ui.connection.connected && personality == ui.snapshot.personality.current, onClick = null) },
                    enabled = canSwitch,
                    onClick = {
                        open = false
                        if (personality != ui.snapshot.personality.current) viewModel.switchPersonality(personality)
                    },
                )
            }
        }
    }
}

/** Home, Capture, C: always in place, so starting never rearranges the page; live only while playing. */
@Composable
private fun ConsoleTile(viewModel: CompanionViewModel, playing: Boolean, modifier: Modifier) {
    Row(
        modifier
            .clip(RoundedCornerShape(LayoutTokens.CardRadius))
            .background(MaterialTheme.colorScheme.surfaceContainerHigh)
            .padding(LayoutTokens.Space3),
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
    ) {
        // Hold, not click: the console tells a tap from a hold (Capture screenshots
        // on release but records on a long hold). Pressed through Controller Link.
        ActionButton(Icons.Default.Home, "Home", Modifier.weight(1f).fillMaxHeight(), enabled = playing, stacked = true,
            onHold = { viewModel.setConsoleButton(ControllerButton.Home, it) })
        ActionButton(Icons.Default.PhotoCamera, "Capture", Modifier.weight(1f).fillMaxHeight(), enabled = playing, stacked = true,
            onHold = { viewModel.setConsoleButton(ControllerButton.Capture, it) })
        ActionButton(Icons.AutoMirrored.Filled.Chat, "C", Modifier.weight(1f).fillMaxHeight(), enabled = playing, stacked = true,
            onHold = { viewModel.setConsoleButton(ControllerButton.C, it) })
    }
}

/** Start / Stop, Touch Gamepad (in every state), and Wake or the adapter's next step. */
@Composable
private fun ActionRow(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    action: PlayAction.View,
    onHero: () -> Unit,
    hasAdapter: Boolean,
    onOpenTouchGamepad: () -> Unit,
    onConnectAdapter: () -> Unit,
    modifier: Modifier,
) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
        HeroButton(action, onHero, Modifier.weight(1f))
        // Not gated on a physical input: the screen itself is a complete controller.
        QuickAction(Icons.Default.TouchApp, "Touch Gamepad", enabled = hasAdapter, onClick = onOpenTouchGamepad)
        if (ui.connection.connected) {
            QuickAction(
                Icons.Default.PowerSettingsNew, "Wake",
                enabled = !ui.busy && ui.snapshot.capabilities.wake != CapabilityState.Unsupported,
                onClick = viewModel::wake,
            )
        } else {
            val connects = AdapterStatus.nextStepConnects(false, ui.connection.phase, hasAdapter)
            QuickAction(
                Icons.Default.Cable,
                AdapterStatus.nextStep(false, ui.connection.phase, hasAdapter),
                enabled = !AdapterStatus.inProgress(ui.connection.phase) && !ui.busy,
                onClick = { if (connects) onConnectAdapter() else viewModel.navigate(AppSection.Adapter) },
            )
        }
    }
}

/**
 * The headset, as one thing that is plugged in or not (see [HeadsetTile]).
 * Plugging in asks for the microphone permission once; without it the headset
 * is headphones, which is what a headset without a mic is.
 */
@Composable
private fun HeadsetTileCard(viewModel: CompanionViewModel, modifier: Modifier) {
    val audio by viewModel.headset.collectAsState()
    val view = HeadsetTile.of(audio)
    val context = LocalContext.current
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        viewModel.setHeadset(on = true, microphonePermitted = granted)
    }
    val colors = MaterialTheme.colorScheme
    HomeTile(
        onClick = {
            when {
                audio.on -> viewModel.setHeadset(on = false, microphonePermitted = false)
                context.canRecord() -> viewModel.setHeadset(on = true, microphonePermitted = true)
                else -> permission.launch(Manifest.permission.RECORD_AUDIO)
            }
        },
        modifier = modifier,
        enabled = view.enabled,
        container = if (view.live) colors.secondaryContainer else colors.surfaceContainerHigh,
        content = if (view.live) colors.onSecondaryContainer else colors.onSurface,
        description = "Headset. ${if (view.live) "Live" else "Off"}. ${view.line}",
    ) {
        Row(Modifier.fillMaxSize(), verticalAlignment = Alignment.CenterVertically) {
            TileIcon(if (view.mic == HeadsetTile.Mic.None) Icons.Default.Headphones else Icons.Default.HeadsetMic, view.live)
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    // The title gives way, never the badge: a squeezed badge
                    // wrapped to "LIV / E" on the Odin's font scale.
                    Text(
                        view.title,
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f, fill = false),
                    )
                    if (view.live) {
                        Spacer(Modifier.width(LayoutTokens.Space2))
                        LiveBadge("LIVE", LocalContentColor.current)
                    }
                }
                if (view.line.isNotEmpty()) {
                    Text(
                        view.line,
                        style = MaterialTheme.typography.bodyMedium,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            when {
                view.busy -> CircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 3.dp)
                view.mic != HeadsetTile.Mic.None -> {
                    val muted = view.mic == HeadsetTile.Mic.Muted
                    FilledTonalIconToggleButton(
                        checked = muted,
                        onCheckedChange = viewModel::setHeadsetMuted,
                        modifier = Modifier.size(52.dp),
                    ) { Icon(if (muted) Icons.Default.MicOff else Icons.Default.Mic, if (muted) "Unmute" else "Mute") }
                }
            }
        }
    }
}

/**
 * The figure the adapter is holding, as art and a name -- never as a UID. A
 * tap opens the library; Present / Eject is the one action worth a shortcut.
 */
@Composable
private fun AmiiboTileCard(ui: CompanionUiState, viewModel: CompanionViewModel, modifier: Modifier) {
    val status = ui.snapshot.amiibo
    val resident = ui.connection.connected && (status.loaded || status.v3Loaded)
    val catalog = ui.adapterAmiiboCatalog
    val backup = remember(ui.library, status.uid, resident) {
        AmiiboGallery.residentId(ui.library, status.uid, resident)?.let { id -> ui.library.firstOrNull { it.id == id } }
    }
    val name = catalogTitle(catalog, backup?.displayName ?: "Amiibo")
    // Where the library opens: this exact tag when the phone has it, else the
    // same figure (another dump of it), so the card lands on what the adapter
    // holds rather than on the first figure in the library.
    val libraryTarget = backup?.id ?: if (resident && status.figureId.isNotBlank()) {
        ui.library.firstOrNull { it.figureId.equals(status.figureId, ignoreCase = true) }?.id
    } else {
        null
    }
    val canPresent = resident && !ui.busy && ui.snapshot.personality.current == Personality.Pro2 &&
        ui.snapshot.capabilities.amiibo != CapabilityState.Unsupported
    // The console has an NFC scan running: the game is asking for a figure
    // right now. The tile says so and its action is the one to press.
    val wanted = ConsoleCues.amiiboWanted(ui.connection.connected, ui.snapshot.input.console) && !status.presented
    val colors = MaterialTheme.colorScheme
    HomeTile(
        onClick = { viewModel.openAmiiboLibrary(libraryTarget) },
        modifier = modifier,
        container = if (wanted) colors.tertiaryContainer else colors.surfaceContainerHigh,
        content = if (wanted) colors.onTertiaryContainer else colors.onSurface,
        description = buildString {
            if (wanted) append("The game is waiting for an amiibo. ")
            append(if (resident) "Amiibo: $name. Open library." else "No Amiibo loaded. Open library.")
        },
    ) {
        Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally) {
            if (wanted) {
                LiveBadge("GAME WANTS AN AMIIBO", LocalContentColor.current)
                Spacer(Modifier.height(LayoutTokens.Space2))
            }
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                if (resident) {
                    AmiiboArtwork(
                        catalog?.imageUrl.orEmpty(),
                        name,
                        Modifier.fillMaxHeight().aspectRatio(1f, matchHeightConstraintsFirst = true)
                            .clip(RoundedCornerShape(LayoutTokens.CardRadius)),
                    )
                } else {
                    Box(
                        Modifier.size(96.dp).background(colors.surfaceVariant, CircleShape),
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(Icons.Default.Contactless, null, Modifier.size(48.dp), tint = colors.onSurfaceVariant)
                    }
                }
            }
            Spacer(Modifier.height(LayoutTokens.Space2))
            Text(
                if (resident) name else "No Amiibo",
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            val subtitle = when {
                !ui.connection.connected -> "Adapter not connected"
                !resident && wanted -> "Choose one from the library"
                !resident -> ""
                else -> catalogSubtitle(catalog).ifBlank { if (backup != null) "Saved on this phone" else "Not in your library" }
            }
            if (subtitle.isNotEmpty()) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (wanted) LocalContentColor.current else colors.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(LayoutTokens.Space3))
            // A full-size action, the same as every other on Play: the small
            // tonal button that used to sit beside the name was squeezed to
            // nothing on a narrow tile.
            if (resident) {
                ActionButton(
                    icon = if (status.presented) Icons.Default.Eject else Icons.Default.Contactless,
                    label = if (status.presented) "Eject" else "Present",
                    modifier = Modifier.fillMaxWidth(),
                    tone = if (status.presented) ActionTone.Tonal else ActionTone.Primary,
                    enabled = canPresent,
                    onClick = { viewModel.setPresented(!status.presented) },
                )
            } else {
                ActionButton(
                    icon = Icons.Default.CollectionsBookmark,
                    label = "Library",
                    modifier = Modifier.fillMaxWidth(),
                    tone = if (wanted) ActionTone.Primary else ActionTone.Tonal,
                    onClick = { viewModel.openAmiiboLibrary(libraryTarget) },
                )
            }
        }
    }
}

@Composable
private fun TileIcon(icon: ImageVector, live: Boolean) {
    val colors = MaterialTheme.colorScheme
    Box(
        Modifier.size(56.dp).background(if (live) colors.primary else colors.surfaceVariant, CircleShape),
        contentAlignment = Alignment.Center,
    ) { Icon(icon, null, Modifier.size(28.dp), tint = if (live) colors.onPrimary else colors.onSurfaceVariant) }
}

private fun Context.canRecord(): Boolean =
    ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

/** The one line that says what is in the way, and a contract-skew warning when there is one. */
@Composable
private fun ActionLine(action: PlayAction.View, ui: CompanionUiState) {
    if (action.line.isNotBlank()) {
        Text(
            action.line,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
    }
    // Firmware/app bridge-contract skew: battery, motion and rumble missing
    // while buttons work. Silent on the compatible path.
    if (BridgeContract.warrantsWarning(ui.bridgeCompatibility)) {
        InlineNotice(
            ui.bridgeCompatibility.summary,
            Modifier.widthIn(max = 520.dp),
            icon = Icons.Default.Warning,
            tone = ChipTone.Error,
        )
    }
}

/**
 * Start / Stop: the same size as every other action, distinguished by colour
 * alone. Takes focus when Play opens under a gamepad, so A is always "play" on
 * the home screen.
 */
@Composable
private fun HeroButton(action: PlayAction.View, onClick: () -> Unit, modifier: Modifier) {
    val focus = remember { FocusRequester() }
    val keys = keyNavigation()
    LaunchedEffect(keys) { if (keys) runCatching { focus.requestFocus() } }
    ActionButton(
        if (action.kind == PlayAction.Kind.Stop) Icons.Default.Stop else Icons.Default.PlayArrow,
        action.label,
        modifier.focusRequester(focus),
        tone = if (action.kind == PlayAction.Kind.Stop) ActionTone.Danger else ActionTone.Primary,
        enabled = action.enabled,
        busy = action.kind == PlayAction.Kind.Busy,
        onClick = onClick,
    )
}

@Composable
private fun RowScope.QuickAction(icon: ImageVector, label: String, enabled: Boolean, onClick: () -> Unit) {
    ActionButton(icon, label, Modifier.weight(1f), enabled = enabled, onClick = onClick)
}

/**
 * Live normalized controller state.
 *
 * A troubleshooting instrument rather than a product feature, so it renders in
 * Diagnostics; it lives here because the state it reads belongs to Play's subject.
 */
@Composable
internal fun LiveInputBlock(state: dev.picoswitch.bridge.core.ControllerState) {
    Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
        AxisMeter("LX", state.leftX, Modifier.weight(1f))
        AxisMeter("LY", state.leftY, Modifier.weight(1f))
        AxisMeter("RX", state.rightX, Modifier.weight(1f))
        AxisMeter("RY", state.rightY, Modifier.weight(1f))
    }
    // Four retained directions, not the wire hat code: the UI shows the
    // normalized model, and the hat is a protocol detail the encoder owns.
    val dpad = listOfNotNull(
        "Up".takeIf { state.dpadUp }, "Right".takeIf { state.dpadRight },
        "Down".takeIf { state.dpadDown }, "Left".takeIf { state.dpadLeft },
    )
    LabelValueRow("Triggers", "L2 ${state.leftTrigger} · R2 ${state.rightTrigger}", monospace = true)
    LabelValueRow("D-pad", dpad.ifEmpty { listOf("centered") }.joinToString(" + "))
    LabelValueRow(
        "Buttons",
        if (state.buttons.isEmpty()) "None held" else state.buttons.joinToString(" · ") { it.name },
    )
}

@Composable
private fun AxisMeter(label: String, value: Int, modifier: Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        LinearProgressIndicator({ value / 255f }, Modifier.fillMaxWidth().height(6.dp))
        Spacer(Modifier.height(LayoutTokens.Space1))
        Text("$label $value", style = MaterialTheme.typography.labelSmall)
    }
}
