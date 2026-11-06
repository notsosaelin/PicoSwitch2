package dev.picoswitch.bridge.core

import org.junit.Assert.assertEquals
import org.junit.Test

class ControllerLayoutTest {

    // ------------------------------------------------- the on-screen controller

    @Test fun `a Nintendo presentation swaps only the four on-screen face slots`() {
        assertEquals(ControllerButton.B, ControllerLayoutResolver.mapTouchFacePosition(ControllerButton.A, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.A, ControllerLayoutResolver.mapTouchFacePosition(ControllerButton.B, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.Y, ControllerLayoutResolver.mapTouchFacePosition(ControllerButton.X, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.X, ControllerLayoutResolver.mapTouchFacePosition(ControllerButton.Y, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.L1, ControllerLayoutResolver.mapTouchFacePosition(ControllerButton.L1, ControllerFaceLayout.Nintendo))
    }

    @Test fun `an Xbox presentation sends the on-screen slot's own letter`() {
        ControllerButton.entries.forEach { button ->
            assertEquals(button, ControllerLayoutResolver.mapTouchFacePosition(button, ControllerFaceLayout.Xbox))
        }
    }

    // ------------------------------------------------ the built-in physical pad

    /**
     * Regression, 2026-08-24. Controller Link face buttons came out inverted on
     * console because physical keys and on-screen slots shared one mapper. The
     * console-facing rule for a physical key is the same for both device families
     * and is what these two tests state: the face button you press lands on the
     * face button in the same place on the Switch.
     *
     * A Nintendo-labelled handheld reports the PRINTED letter — the button
     * printed `A` sends `KEYCODE_BUTTON_A` — so it is already logical and must
     * pass through untouched.
     */
    @Test fun `a Nintendo-labelled source's face keys are already logical`() {
        listOf(
            ControllerButton.A, ControllerButton.B, ControllerButton.X, ControllerButton.Y,
            ControllerButton.L1, ControllerButton.Start,
        ).forEach { key ->
            assertEquals(key, ControllerLayoutResolver.mapPhysicalFaceKey(key, ControllerFaceLayout.Nintendo))
        }
    }

    /**
     * A positional source names the BOTTOM button `A`, but the console's bottom
     * button is B. Its face keys are swapped into logical order; nothing else is.
     */
    @Test fun `a positional source's face keys are swapped into logical order`() {
        assertEquals(ControllerButton.B, ControllerLayoutResolver.mapPhysicalFaceKey(ControllerButton.A, ControllerFaceLayout.Xbox))
        assertEquals(ControllerButton.A, ControllerLayoutResolver.mapPhysicalFaceKey(ControllerButton.B, ControllerFaceLayout.Xbox))
        assertEquals(ControllerButton.Y, ControllerLayoutResolver.mapPhysicalFaceKey(ControllerButton.X, ControllerFaceLayout.Xbox))
        assertEquals(ControllerButton.X, ControllerLayoutResolver.mapPhysicalFaceKey(ControllerButton.Y, ControllerFaceLayout.Xbox))
        listOf(
            ControllerButton.L1, ControllerButton.R2, ControllerButton.Select,
            ControllerButton.Start, ControllerButton.LeftStick, ControllerButton.Home,
            ControllerButton.Capture, ControllerButton.C,
        ).forEach { key ->
            assertEquals(key, ControllerLayoutResolver.mapPhysicalFaceKey(key, ControllerFaceLayout.Xbox))
        }
    }

    /**
     * The trap this locks out: the two mappers look interchangeable, and merging
     * them silently inverts whichever origin was not being tested. Under either
     * layout they must disagree on every face button — that disagreement IS the
     * correction, and if it ever disappears one of the two origins is broken.
     */
    @Test fun `the physical and on-screen mappers are opposites for every layout`() {
        val faces = listOf(ControllerButton.A, ControllerButton.B, ControllerButton.X, ControllerButton.Y)
        listOf(ControllerFaceLayout.Nintendo, ControllerFaceLayout.Xbox).forEach { layout ->
            faces.forEach { button ->
                val touch = ControllerLayoutResolver.mapTouchFacePosition(button, layout)
                val physical = ControllerLayoutResolver.mapPhysicalFaceKey(button, layout)
                assertEquals(
                    "$layout $button: the two origins must not resolve alike",
                    false, touch == physical,
                )
            }
        }
    }

    // ---------------------------------------------------------- source identity

    @Test fun `Auto falls back to positional order for an unknown source`() {
        val unknown = ControllerSourceIdentity("source", "Generic Gamepad", 0x1234, 0x5678)
        assertEquals(ControllerFaceLayout.Xbox, ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, unknown).layout)
    }

    @Test fun `Auto recognizes the audited legend-reporting handheld identities`() {
        val aynNintendoMode = ControllerSourceIdentity("ayn-1", "Odin Controller", 0x2020, 0x0111)
        val retroid = ControllerSourceIdentity("retroid", "Retroid Pocket Controller", 0x2022, 0x3001)
        listOf(aynNintendoMode, retroid).forEach { source ->
            assertEquals(ControllerFaceLayout.Nintendo, ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, source).layout)
        }
    }

    /**
     * Regression, 2026-08-24, from a live Odin 2 Mini. AYN's button-layout toggle
     * swaps the DEVICE IDENTITY — `0x0111 "Odin Controller"` becomes
     * `0x0112 "Xbox Wireless Controller"` — and with it which key code each
     * physical button sends. Auto used to claim both PIDs as Nintendo, which
     * inverts every face button whenever the handheld is left in Xbox mode.
     *
     * Same plastic, same vendor, opposite answer. Do not re-merge these.
     */
    @Test fun `Auto treats the same handheld in Xbox mode as a positional source`() {
        val aynXboxMode = ControllerSourceIdentity("ayn-2", "Xbox Wireless Controller", 0x2020, 0x0112)
        assertEquals(
            ControllerFaceLayout.Xbox,
            ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, aynXboxMode).layout,
        )
    }

    @Test fun `manual selection always overrides inferred layout`() {
        val known = ControllerSourceIdentity("thor", "Odin Controller", 0x2020, 0x0111)
        assertEquals(ControllerFaceLayout.Xbox, ControllerLayoutResolver.resolve(ControllerFaceLayout.Xbox, known).layout)
        assertEquals(ControllerFaceLayout.Nintendo, ControllerLayoutResolver.resolve(ControllerFaceLayout.Nintendo, null).layout)
    }

    // ------------------------------------------------------------------- labels

    /**
     * A drawn legend and the bit that is sent must come from the same decision.
     * A renderer with its own `when(layout)` block produces a button that says A
     * and sends B, and nothing catches it until someone presses it on a console.
     */
    @Test fun `a face position's drawn label is the button that position sends`() {
        listOf(ControllerFaceLayout.Nintendo, ControllerFaceLayout.Xbox).forEach { layout ->
            FaceButtonPosition.entries.forEach { position ->
                assertEquals(
                    "$layout $position",
                    ControllerLayoutResolver.mapTouchFacePosition(position.positional, layout).name,
                    ControllerLayoutResolver.faceLabel(position, layout),
                )
            }
        }
    }

    @Test fun `face positions carry the platform-standard positional order`() {
        assertEquals(ControllerButton.A, FaceButtonPosition.South.positional)
        assertEquals(ControllerButton.B, FaceButtonPosition.East.positional)
        assertEquals(ControllerButton.X, FaceButtonPosition.West.positional)
        assertEquals(ControllerButton.Y, FaceButtonPosition.North.positional)
    }

    @Test fun `Nintendo labels read like a Switch controller`() {
        assertEquals("B", ControllerLayoutResolver.faceLabel(FaceButtonPosition.South, ControllerFaceLayout.Nintendo))
        assertEquals("A", ControllerLayoutResolver.faceLabel(FaceButtonPosition.East, ControllerFaceLayout.Nintendo))
        assertEquals("Y", ControllerLayoutResolver.faceLabel(FaceButtonPosition.West, ControllerFaceLayout.Nintendo))
        assertEquals("X", ControllerLayoutResolver.faceLabel(FaceButtonPosition.North, ControllerFaceLayout.Nintendo))
    }
}
