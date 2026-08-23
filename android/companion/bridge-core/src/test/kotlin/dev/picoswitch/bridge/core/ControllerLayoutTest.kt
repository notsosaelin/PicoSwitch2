package dev.picoswitch.bridge.core

import org.junit.Assert.assertEquals
import org.junit.Test

class ControllerLayoutTest {
    @Test fun `Nintendo layout swaps only the four face buttons`() {
        assertEquals(ControllerButton.B, ControllerLayoutResolver.mapFaceButton(ControllerButton.A, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.A, ControllerLayoutResolver.mapFaceButton(ControllerButton.B, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.Y, ControllerLayoutResolver.mapFaceButton(ControllerButton.X, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.X, ControllerLayoutResolver.mapFaceButton(ControllerButton.Y, ControllerFaceLayout.Nintendo))
        assertEquals(ControllerButton.L1, ControllerLayoutResolver.mapFaceButton(ControllerButton.L1, ControllerFaceLayout.Nintendo))
    }

    @Test fun `Xbox and Auto fallback retain Android positional order`() {
        ControllerButton.entries.forEach { button ->
            assertEquals(button, ControllerLayoutResolver.mapFaceButton(button, ControllerFaceLayout.Xbox))
        }
        val unknown = ControllerSourceIdentity("source", "Generic Gamepad", 0x1234, 0x5678)
        assertEquals(ControllerFaceLayout.Xbox, ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, unknown).layout)
    }

    @Test fun `Auto recognizes both audited Nintendo-labeled handheld families`() {
        val thorOdinMode = ControllerSourceIdentity("thor-1", "Odin Controller", 0x2020, 0x0111)
        val thorXboxMode = ControllerSourceIdentity("thor-2", "Xbox Wireless Controller", 0x2020, 0x0112)
        val retroid = ControllerSourceIdentity("retroid", "Retroid Pocket Controller", 0x2022, 0x3001)
        listOf(thorOdinMode, thorXboxMode, retroid).forEach { source ->
            assertEquals(ControllerFaceLayout.Nintendo, ControllerLayoutResolver.resolve(ControllerFaceLayout.Auto, source).layout)
        }
    }

    @Test fun `manual selection always overrides inferred layout`() {
        val known = ControllerSourceIdentity("thor", "Odin Controller", 0x2020, 0x0111)
        assertEquals(ControllerFaceLayout.Xbox, ControllerLayoutResolver.resolve(ControllerFaceLayout.Xbox, known).layout)
        assertEquals(ControllerFaceLayout.Nintendo, ControllerLayoutResolver.resolve(ControllerFaceLayout.Nintendo, null).layout)
    }

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
                    ControllerLayoutResolver.mapFaceButton(position.positional, layout).name,
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
