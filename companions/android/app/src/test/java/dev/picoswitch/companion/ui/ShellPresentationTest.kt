package dev.picoswitch.companion.ui

import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.management.ConsoleView
import org.junit.Assert.assertNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The shell's words are a cross-platform contract (the C# twin is
 * PlayStatusTests / ShellNavigationTests in PicoSwitch.Companion.Services.Tests), so
 * the exact strings are asserted, not just their presence.
 */
class ShellPresentationTest {
    @Test
    fun `console cues need a connected adapter and a real player number`() {
        val view = ConsoleView(player = 3, amiiboWanted = true)
        assertEquals("Player 3", ConsoleCues.playerLabel(connected = true, view))
        assertTrue(ConsoleCues.amiiboWanted(connected = true, view))
        // A lost adapter's last snapshot says nothing about the console now.
        assertNull(ConsoleCues.playerLabel(connected = false, view))
        assertFalse(ConsoleCues.amiiboWanted(connected = false, view))
        // Older firmware: no console view, no cues.
        assertNull(ConsoleCues.playerLabel(connected = true, ConsoleView()))
        assertFalse(ConsoleCues.amiiboWanted(connected = true, ConsoleView()))
    }

    @Test
    fun `adapter words match the desktop companions`() {
        assertEquals(StatusWords(StatusTone.Good, "Connected"), AdapterStatus.of(true, ConnectionPhase.Connected, true))
        assertEquals(StatusWords(StatusTone.Idle, "Not paired"), AdapterStatus.of(false, ConnectionPhase.Idle, false))
        assertEquals(StatusWords(StatusTone.Idle, "Not connected"), AdapterStatus.of(false, ConnectionPhase.Idle, true))
        assertEquals(StatusWords(StatusTone.Busy, "Looking…"), AdapterStatus.of(false, ConnectionPhase.Scanning, true))
        assertEquals(StatusWords(StatusTone.Busy, "Pairing…"), AdapterStatus.of(false, ConnectionPhase.Bonding, false))
        assertEquals(StatusWords(StatusTone.Busy, "Connecting…"), AdapterStatus.of(false, ConnectionPhase.Connecting, true))
        assertEquals(StatusWords(StatusTone.Bad, "Could not connect"), AdapterStatus.of(false, ConnectionPhase.Failed, true))
        assertEquals(StatusWords(StatusTone.Bad, "Needs repair"), AdapterStatus.of(false, ConnectionPhase.RepairRequired, true))
    }

    @Test
    fun `the next step is never a dead end`() {
        assertEquals("Manage", AdapterStatus.nextStep(true, ConnectionPhase.Connected, true))
        assertEquals("Pair adapter", AdapterStatus.nextStep(false, ConnectionPhase.Idle, false))
        assertEquals("Repair", AdapterStatus.nextStep(false, ConnectionPhase.RepairRequired, true))
        assertEquals("Connecting…", AdapterStatus.nextStep(false, ConnectionPhase.Connecting, true))
        assertEquals("Connect", AdapterStatus.nextStep(false, ConnectionPhase.Failed, true))

        assertTrue(AdapterStatus.nextStepConnects(false, ConnectionPhase.Idle, true))
        assertFalse(AdapterStatus.nextStepConnects(false, ConnectionPhase.Idle, false))
        assertFalse(AdapterStatus.nextStepConnects(false, ConnectionPhase.RepairRequired, true))
        assertFalse(AdapterStatus.nextStepConnects(true, ConnectionPhase.Connected, true))
    }

    @Test
    fun `controller card words match the desktop companions`() {
        assertEquals(ControllerCardStatus.View("Pad", StatusTone.Good, "Ready"), ControllerCardStatus.of("Pad", true, 1))
        assertEquals(ControllerCardStatus.View("Pad", StatusTone.Busy, "Can't be read"), ControllerCardStatus.of("Pad", false, 1))
        assertEquals(ControllerCardStatus.View("No controller", StatusTone.Idle, "Plug one in"), ControllerCardStatus.of(null, false, 0))
        assertEquals(
            ControllerCardStatus.View("No controller chosen", StatusTone.Idle, "Choose one below"),
            ControllerCardStatus.of(null, false, 2),
        )
    }

    @Test
    fun `play says what is in the way and nothing when the button says it all`() {
        val ready = PlayAction.of(BridgeLinkPhase.Idle, hasAdapter = true, hasController = true, hostKnown = true, bridgeMessage = null)
        assertEquals(PlayAction.Kind.Start, ready.kind)
        assertTrue(ready.enabled)
        assertEquals("", ready.line)

        val noAdapter = PlayAction.of(BridgeLinkPhase.Idle, hasAdapter = false, hasController = true, hostKnown = false, bridgeMessage = null)
        assertFalse(noAdapter.enabled)
        assertEquals("Pair an adapter first.", noAdapter.line)

        val playing = PlayAction.of(BridgeLinkPhase.Playing, true, true, true, null)
        assertEquals(PlayAction.Kind.Stop, playing.kind)
        assertEquals("Stop", playing.label)

        val starting = PlayAction.of(BridgeLinkPhase.Registering, true, true, true, "Registering")
        assertFalse(starting.enabled)
        assertEquals("Registering", starting.line)
    }

    @Test
    fun `automatic navigation is at the side in landscape and the bottom otherwise`() {
        assertEquals(NavPlacement.Left, NavPlacement.resolve(NavPlacement.Auto, landscape = true))
        assertEquals(NavPlacement.Bottom, NavPlacement.resolve(NavPlacement.Auto, landscape = false))
        NavPlacement.entries.filter { it != NavPlacement.Auto }.forEach { chosen ->
            assertEquals(chosen, NavPlacement.resolve(chosen, landscape = true))
            assertEquals(chosen, NavPlacement.resolve(chosen, landscape = false))
        }
        assertTrue(NavPlacement.Left.vertical && NavPlacement.Right.vertical)
        assertFalse(NavPlacement.Top.vertical || NavPlacement.Bottom.vertical)
        assertEquals(NavPlacement.Auto, NavPlacement.fromKey("diagonal"))
    }

    @Test
    fun `the navigation placement survives the preference codec`() {
        val chosen = ThemeSelection(navigation = NavPlacement.Top)
        val encoded = ThemePreferenceCodec.encode(chosen)
        assertEquals(
            chosen,
            ThemePreferenceCodec.decode(
                encoded[ThemePreferenceStore.MODE_KEY],
                encoded[ThemePreferenceStore.PALETTE_KEY],
                encoded[ThemePreferenceStore.NAVIGATION_KEY],
            ),
        )
        // An install from before the setting existed has no key: Automatic.
        assertEquals(NavPlacement.Auto, ThemePreferenceCodec.decode("light", "standard").navigation)
    }

    @Test
    fun `face layout choices read the same as the desktop`() {
        assertEquals(
            listOf("Automatic", "Match the letters (A sends A)", "Match the positions (bottom sends bottom)"),
            FaceLayoutChoices.options.map { it.second },
        )
        assertEquals(
            "Matching the letters.",
            FaceLayoutChoices.reason(ControllerFaceLayout.Auto, ControllerFaceLayout.Nintendo),
        )
        assertEquals(
            "Matching the positions.",
            FaceLayoutChoices.reason(ControllerFaceLayout.Auto, ControllerFaceLayout.Xbox),
        )
        // A manual choice needs no line: the choice itself is on screen.
        assertEquals("", FaceLayoutChoices.reason(ControllerFaceLayout.Xbox, ControllerFaceLayout.Xbox))
    }
}
