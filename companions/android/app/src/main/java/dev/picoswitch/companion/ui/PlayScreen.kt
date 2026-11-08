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
 * - the HERO (left, largest): which controller this device is reading, the
 *   state in one word, and the one big Start / Stop; underneath, the console
 *   buttons while playing and the secondary actions otherwise;
 * - the HEADSET tile: one tap plugs it in (LIVE), one tap pulls it out; the
 *   microphone comes with it and only mutes;
 * - the AMIIBO tile: the figure the adapter is holding, as art and a name.
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
        val hero: @Composable (Modifier) -> Unit = { modifier ->
            HeroCard(ui, viewModel, action, onHero, playing, hasAdapter, onOpenTouchGamepad, onConnectAdapter, modifier)
        }
        if (hub) {
            Row(
                Modifier
                    .widthIn(max = LayoutTokens.WidePageMaxWidth)
                    .fillMaxSize()
                    .padding(horizontal = if (short) LayoutTokens.Space4 else LayoutTokens.PagePaddingSide)
                    .padding(
                        top = if (short) LayoutTokens.Space1 else LayoutTokens.PagePaddingTop,
                        bottom = if (short) LayoutTokens.Space2 else LayoutTokens.PagePaddingBottom,
                    ),
                horizontalArrangement = Arrangement.spacedBy(gap),
            ) {
                // Left: the headset as a slim strip, the controller card taking
                // the rest. Right: the Amiibo, full height, so its art is the
                // picture on the home screen (owner, 2026-11-08).
                Column(Modifier.weight(1.35f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(gap)) {
                    HeadsetTileCard(viewModel, Modifier.fillMaxWidth().height(HEADSET_STRIP_HEIGHT))
                    hero(Modifier.weight(1f).fillMaxWidth())
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
                hero(Modifier.fillMaxWidth().heightIn(min = 300.dp))
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

/** The big card: the controller, its state, Start / Stop, and what goes with it. */
@Composable
private fun HeroCard(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    action: PlayAction.View,
    onHero: () -> Unit,
    playing: Boolean,
    hasAdapter: Boolean,
    onOpenTouchGamepad: () -> Unit,
    onConnectAdapter: () -> Unit,
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
    ) {
        Column(
            Modifier.fillMaxSize().padding(if (short) LayoutTokens.Space3 else LayoutTokens.Space4),
            verticalArrangement = Arrangement.spacedBy(if (short) LayoutTokens.Space1 else LayoutTokens.Space2),
        ) {
            ControllerHeader(ui, viewModel)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    headline,
                    style = if (short) MaterialTheme.typography.headlineSmall else MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
                // The number the console gave this controller (its player
                // LEDs), so the player knows which slot they are in.
                ConsoleCues.playerLabel(ui.connection.connected, ui.snapshot.input.console)?.let { label ->
                    Spacer(Modifier.width(LayoutTokens.Space3))
                    PlayerBadge(label)
                }
            }
            if (!playing) ActionLine(action, ui)
            // The actions sit at the bottom edge, where the thumbs are.
            Spacer(Modifier.weight(1f))
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
            ) {
                if (playing) {
                    ConsoleButtons(viewModel)
                } else {
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
            HeroButton(action, onHero)
        }
    }
}

/** Which controller this device is reading. A choice opens as a menu, so the header stays one line. */
@Composable
private fun ControllerHeader(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val selected = ui.sourceDevices.firstOrNull { it.descriptor == ui.selectedSourceDescriptor }
    val view = ControllerCardStatus.of(selected?.name, selected != null, ui.sourceDevices.size)
    val choose = ui.sourceChoiceRequired || (selected == null && ui.sourceDevices.isNotEmpty())
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier.size(40.dp).background(MaterialTheme.colorScheme.surface.copy(alpha = 0.6f), CircleShape),
            contentAlignment = Alignment.Center,
        ) { Icon(Icons.Default.Gamepad, null, tint = MaterialTheme.colorScheme.primary) }
        Spacer(Modifier.width(LayoutTokens.Space3))
        Column(Modifier.weight(1f)) {
            Text(
                view.name,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (view.tone != StatusTone.Good) StatusLine(view.tone, view.words)
        }
        Box {
            TextButton(onClick = { if (choose || ui.sourceDevices.size > 1) open = true else viewModel.refreshSources() }) {
                Icon(
                    if (choose || ui.sourceDevices.size > 1) Icons.Default.SwapHoriz else Icons.Default.Refresh,
                    null, Modifier.size(18.dp),
                )
                Spacer(Modifier.width(LayoutTokens.Space1))
                Text(if (choose || ui.sourceDevices.size > 1) "Change" else "Refresh")
            }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
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
                    text = { Text("Refresh") },
                    leadingIcon = { Icon(Icons.Default.Refresh, null) },
                    onClick = {
                        open = false
                        viewModel.refreshSources()
                    },
                )
            }
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

/** A quiet, static pill: the console's player number, not news. */
@Composable
private fun PlayerBadge(label: String) {
    Surface(
        shape = CircleShape,
        color = MaterialTheme.colorScheme.surface.copy(alpha = 0.6f),
        contentColor = MaterialTheme.colorScheme.onSurface,
    ) {
        Text(
            label,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelLarge,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            softWrap = false,
        )
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

/** Console buttons only while streaming: they are pressed through Controller Link, so at any other time they would go nowhere. */
@Composable
private fun RowScope.ConsoleButtons(viewModel: CompanionViewModel) {
    // Hold, not click: the console tells a tap from a hold (Capture screenshots
    // on release but records on a long hold).
    ActionButton(Icons.Default.Home, "Home", Modifier.weight(1f), onHold = { viewModel.setConsoleButton(ControllerButton.Home, it) })
    ActionButton(Icons.Default.PhotoCamera, "Capture", Modifier.weight(1f), onHold = { viewModel.setConsoleButton(ControllerButton.Capture, it) })
    ActionButton(Icons.AutoMirrored.Filled.Chat, "C", Modifier.weight(1f), onHold = { viewModel.setConsoleButton(ControllerButton.C, it) })
}

/**
 * Start / Stop: the same size as every other action, distinguished by colour
 * alone. Takes focus when Play opens under a gamepad, so A is always "play" on
 * the home screen.
 */
@Composable
private fun HeroButton(action: PlayAction.View, onClick: () -> Unit) {
    val focus = remember { FocusRequester() }
    val keys = keyNavigation()
    LaunchedEffect(keys) { if (keys) runCatching { focus.requestFocus() } }
    ActionButton(
        if (action.kind == PlayAction.Kind.Stop) Icons.Default.Stop else Icons.Default.PlayArrow,
        action.label,
        Modifier.fillMaxWidth().focusRequester(focus),
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
