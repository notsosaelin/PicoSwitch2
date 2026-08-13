package dev.picoswitch.companion.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test
import androidx.compose.ui.graphics.Color

class ThemeTest {
    @Test
    fun `unknown preference values fail closed to system standard`() {
        assertEquals(ThemeSelection(), ThemePreferenceCodec.decode("future", "future"))
    }

    @Test
    fun `selection encodes and decodes without losing choices`() {
        val selection = ThemeSelection(ThemeMode.OledBlack, AccentPalette.JoyCon2Inspired)
        val encoded = ThemePreferenceCodec.encode(selection)
        assertEquals(selection, ThemePreferenceCodec.decode(encoded[ThemePreferenceStore.MODE_KEY], encoded[ThemePreferenceStore.PALETTE_KEY]))
    }

    @Test
    fun `palette options retain distinct inspired swatches`() {
        assertNotEquals(AccentPalette.JoyCon1Inspired.leftSwatch, AccentPalette.JoyCon2Inspired.leftSwatch)
        assertNotEquals(AccentPalette.JoyCon1Inspired.rightSwatch, AccentPalette.JoyCon2Inspired.rightSwatch)
        assertEquals(Color(0xFF9BE1E6), AccentPalette.JoyCon2Inspired.leftSwatch)
        assertEquals(Color(0xFFFF8C5F), AccentPalette.JoyCon2Inspired.rightSwatch)
    }
}
