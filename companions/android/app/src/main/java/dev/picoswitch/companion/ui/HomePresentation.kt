package dev.picoswitch.companion.ui

import dev.picoswitch.management.audio.CompanionAudioPhase
import dev.picoswitch.management.audio.CompanionAudioView

/**
 * What Play's headset tile says, decided without Compose so the wording rules
 * are testable.
 *
 * The headset is ONE thing the player plugs in, like a real one: it is either
 * plugged in (LIVE) or not. The microphone comes with it whenever this device
 * can record; there is no second switch, because changing the accessory on a
 * playing session is a re-plug that interrupts the console's audio (see
 * CompanionViewModel.setHeadset). Mute is the only live microphone control.
 */
object HeadsetTile {
    enum class Mic { None, Live, Muted }

    data class View(
        val title: String,
        val line: String,
        val live: Boolean,
        val busy: Boolean,
        val enabled: Boolean,
        val mic: Mic,
    )

    fun of(view: CompanionAudioView): View {
        val mic = when {
            view.phase != CompanionAudioPhase.Playing || !view.microphone -> Mic.None
            view.muted -> Mic.Muted
            else -> Mic.Live
        }
        return when (view.phase) {
            CompanionAudioPhase.Unavailable ->
                View("Headset", view.detail ?: "Connect the adapter to use it", false, false, false, mic)
            CompanionAudioPhase.Starting -> View("Headset", "Plugging in…", false, true, false, mic)
            CompanionAudioPhase.Playing -> View(
                "Headset",
                when {
                    view.awaitingConsole -> "Press any button so the console picks it up"
                    // The mute button already shows the microphone's state.
                    mic != Mic.None -> ""
                    else -> "Headphones only" + (view.microphoneDetail?.let { " · $it" } ?: "")
                },
                live = true, busy = false, enabled = true, mic = mic,
            )
            else -> View(
                "Headset",
                when {
                    !view.controllerLink -> "Start playing to plug it in"
                    view.phase == CompanionAudioPhase.Off -> "Tap to plug in"
                    else -> view.detail ?: "Tap to plug in"
                },
                live = false, busy = false, enabled = view.canToggle, mic = mic,
            )
        }
    }
}
