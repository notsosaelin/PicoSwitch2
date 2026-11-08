package dev.picoswitch.companion.ui

import dev.picoswitch.management.audio.CompanionAudioPhase
import dev.picoswitch.management.audio.CompanionAudioView
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HeadsetTileTest {
    @Test
    fun aPlayingHeadsetIsLiveAndItsMicOnlyMutes() {
        val live = HeadsetTile.of(CompanionAudioView(CompanionAudioPhase.Playing, microphone = true))
        assertTrue(live.live)
        assertEquals(HeadsetTile.Mic.Live, live.mic)
        assertEquals("", live.line)
        val muted = HeadsetTile.of(CompanionAudioView(CompanionAudioPhase.Playing, microphone = true, muted = true))
        assertEquals(HeadsetTile.Mic.Muted, muted.mic)
        assertEquals("", muted.line)
    }

    @Test
    fun noMicrophoneIsStillAHeadsetJustHeadphones() {
        val view = HeadsetTile.of(
            CompanionAudioView(CompanionAudioPhase.Playing, microphone = false, microphoneDetail = "No microphone"),
        )
        assertTrue(view.live)
        assertEquals(HeadsetTile.Mic.None, view.mic)
        assertTrue(view.line.startsWith("Headphones only"))
    }

    @Test
    fun theConsoleWaitingForInputSaysSo() {
        val view = HeadsetTile.of(CompanionAudioView(CompanionAudioPhase.Playing, awaitingConsole = true))
        assertTrue(view.line.contains("press any button", ignoreCase = true))
    }

    @Test
    fun offWithoutControllerLinkCannotBePluggedIn() {
        val view = HeadsetTile.of(CompanionAudioView(CompanionAudioPhase.Off, controllerLink = false))
        assertFalse(view.live)
        assertFalse(view.enabled)
        assertEquals("Start playing to plug it in", view.line)
        assertTrue(HeadsetTile.of(CompanionAudioView(CompanionAudioPhase.Off)).enabled)
    }

    @Test
    fun startingIsBusyAndNotTappable() {
        val view = HeadsetTile.of(CompanionAudioView(CompanionAudioPhase.Starting))
        assertTrue(view.busy)
        assertFalse(view.enabled)
    }
}
