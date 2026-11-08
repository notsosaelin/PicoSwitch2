package dev.picoswitch.companion.ui

import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.management.ConsoleView

// The shell's words and states, kept out of the composables so they can be
// tested and so they read the same as the desktop companions. Each object here
// mirrors one in the C# `PicoSwitch.Companion.Services.Presentation` namespace
// (PlayStatus.cs, ShellNavigation.cs, FaceLayoutChoices.cs); the wording is the
// shared contract in docs/architecture/companion-shell-layout.md. Change both
// together.

/** The colour family a status dot belongs to. Mapped to a colour once, in [toneColor]. */
enum class StatusTone { Good, Busy, Bad, Idle }

data class StatusWords(val tone: StatusTone, val words: String)

/**
 * The adapter's state as a dot and a few words, and the one action that moves
 * it forward. Shared by the top bar and Play's adapter card.
 *
 * Android's connection phases are finer than the desktop relationship phases;
 * each maps onto the shared words. "No relationship" is not a phase here but the
 * absence of a remembered adapter, so it is passed in.
 */
object AdapterStatus {
    fun of(connected: Boolean, phase: ConnectionPhase, hasAdapter: Boolean): StatusWords = when {
        connected -> StatusWords(StatusTone.Good, "Connected")
        phase == ConnectionPhase.Scanning -> StatusWords(StatusTone.Busy, "Looking…")
        phase == ConnectionPhase.Associating || phase == ConnectionPhase.Bonding ->
            StatusWords(StatusTone.Busy, "Pairing…")
        phase == ConnectionPhase.Connecting || phase == ConnectionPhase.Reconnecting ->
            StatusWords(StatusTone.Busy, "Connecting…")
        // Android-only: switching between two remembered adapters.
        phase == ConnectionPhase.Disconnecting -> StatusWords(StatusTone.Busy, "Switching…")
        phase == ConnectionPhase.RepairRequired -> StatusWords(StatusTone.Bad, "Needs repair")
        phase == ConnectionPhase.Failed -> StatusWords(StatusTone.Bad, "Could not connect")
        !hasAdapter -> StatusWords(StatusTone.Idle, "Not paired")
        else -> StatusWords(StatusTone.Idle, "Not connected")
    }

    fun inProgress(phase: ConnectionPhase): Boolean = when (phase) {
        ConnectionPhase.Scanning, ConnectionPhase.Associating, ConnectionPhase.Bonding,
        ConnectionPhase.Connecting, ConnectionPhase.Reconnecting, ConnectionPhase.Disconnecting -> true
        else -> false
    }

    /**
     * The adapter card's button: it names the next step, so the card is never a
     * dead end. "Manage", "Pair adapter" and "Repair" open the Adapter page;
     * "Connect" connects in place.
     */
    fun nextStep(connected: Boolean, phase: ConnectionPhase, hasAdapter: Boolean): String = when {
        connected -> "Manage"
        phase == ConnectionPhase.RepairRequired -> "Repair"
        inProgress(phase) -> "Connecting…"
        !hasAdapter -> "Pair adapter"
        else -> "Connect"
    }

    /** Whether [nextStep] connects in place rather than opening the Adapter page. */
    fun nextStepConnects(connected: Boolean, phase: ConnectionPhase, hasAdapter: Boolean): Boolean =
        !connected && hasAdapter && phase != ConnectionPhase.RepairRequired && !inProgress(phase)
}

/** Play's controller card: which controller, and a few words of state. */
object ControllerCardStatus {
    data class View(val name: String, val tone: StatusTone, val words: String)

    fun of(selectedName: String?, selectedCanDrive: Boolean, sourceCount: Int): View = when {
        selectedName != null && selectedCanDrive -> View(selectedName, StatusTone.Good, "Ready")
        selectedName != null -> View(selectedName, StatusTone.Busy, "Can't be read")
        sourceCount == 0 -> View("No controller", StatusTone.Idle, "Plug one in")
        else -> View("No controller chosen", StatusTone.Idle, "Choose one below")
    }
}

/**
 * Play's primary action and the one line under it.
 *
 * Android's Controller Link has its own lifecycle (register as a HID device,
 * then connect to the adapter), so the view is derived here from the bridge
 * phase instead of the desktop's ControllerLinkView; the words match.
 */
object PlayAction {
    enum class Kind { Start, Reconnect, Stop, Busy }

    data class View(val kind: Kind, val label: String, val enabled: Boolean, val line: String)

    fun of(
        phase: BridgeLinkPhase,
        hasAdapter: Boolean,
        hasController: Boolean,
        hostKnown: Boolean,
        bridgeMessage: String?,
    ): View = when (phase) {
        BridgeLinkPhase.Playing ->
            View(Kind.Stop, "Stop", true, "Keep this screen open while playing.")
        BridgeLinkPhase.Preparing, BridgeLinkPhase.Registering, BridgeLinkPhase.Connecting ->
            View(Kind.Busy, "Starting…", false, bridgeMessage.orEmpty())
        BridgeLinkPhase.Ready -> View(
            Kind.Reconnect, "Start playing", hostKnown,
            if (hostKnown) "" else "The saved adapter pairing is unavailable. Reconnect or pair the adapter again.",
        )
        BridgeLinkPhase.Idle, BridgeLinkPhase.Unsupported, BridgeLinkPhase.Failed -> {
            val blocked = when {
                !hasAdapter -> "Pair an adapter first."
                !hasController -> "Choose a controller, or use the Touch Gamepad."
                else -> null
            }
            View(
                Kind.Start, "Start playing", blocked == null,
                blocked ?: if (phase == BridgeLinkPhase.Idle) "" else bridgeMessage.orEmpty(),
            )
        }
    }
}

/**
 * What the console has told the adapter, as Play shows it (input sources
 * reply, "console"). Shown only while the adapter is connected: the last
 * snapshot of a lost adapter says nothing about the console now.
 */
object ConsoleCues {
    /** "Player 2" on the controller card, or null when the console assigned none. */
    fun playerLabel(connected: Boolean, console: ConsoleView): String? =
        if (connected && console.player in 1..4) "Player ${console.player}" else null

    /** The game is waiting for an amiibo: the Amiibo tile asks for one. */
    fun amiiboWanted(connected: Boolean, console: ConsoleView): Boolean =
        connected && console.amiiboWanted
}

/**
 * Where the navigation sits. The player's choice (Settings → Navigation);
 * Automatic puts it at the side of a landscape window, where height is the
 * scarce dimension, and at the bottom otherwise.
 */
enum class NavPlacement(val key: String, val title: String) {
    Auto("auto", "Automatic"),
    Left("left", "Left"),
    Right("right", "Right"),
    Top("top", "Top"),
    Bottom("bottom", "Bottom"),
    ;

    /** At the side: a column of icons. At the top or bottom: a row with names. */
    val vertical: Boolean get() = this == Left || this == Right

    companion object {
        fun fromKey(key: String?): NavPlacement = entries.firstOrNull { it.key == key } ?: Auto

        fun resolve(chosen: NavPlacement, landscape: Boolean): NavPlacement = when {
            chosen != Auto -> chosen
            landscape -> Left
            else -> Bottom
        }
    }
}

/** Settings' "Face buttons" row: the choices, in order, and what Automatic is doing. */
object FaceLayoutChoices {
    val options: List<Pair<ControllerFaceLayout, String>> = listOf(
        ControllerFaceLayout.Auto to "Automatic",
        ControllerFaceLayout.Nintendo to "Match the letters (A sends A)",
        ControllerFaceLayout.Xbox to "Match the positions (bottom sends bottom)",
    )

    fun label(layout: ControllerFaceLayout): String =
        options.firstOrNull { it.first == layout }?.second ?: layout.title

    /**
     * What Automatic is doing, and nothing for a manual choice (the choice
     * already says it). Why Automatic resolved that way is a tooltip on the
     * desktops and is not on screen here: as a line it made the row a paragraph.
     */
    fun reason(chosen: ControllerFaceLayout, resolved: ControllerFaceLayout): String =
        if (chosen == ControllerFaceLayout.Auto) {
            if (resolved == ControllerFaceLayout.Nintendo) "Matching the letters." else "Matching the positions."
        } else {
            ""
        }
}
