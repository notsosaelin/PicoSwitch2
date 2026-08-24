package dev.picoswitch.companion.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
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

    /**
     * The system-bar icon polarity and the colour scheme are both derived from
     * [resolveDark], so a forced theme cannot leave dark icons on a dark bar.
     * Forcing dark on a light device is the case that used to be wrong.
     */
    @Test
    fun `forced modes ignore the system setting`() {
        assertEquals(true, ThemeSelection(ThemeMode.Dark).resolveDark(systemDark = false))
        assertEquals(true, ThemeSelection(ThemeMode.OledBlack).resolveDark(systemDark = false))
        assertEquals(false, ThemeSelection(ThemeMode.Light).resolveDark(systemDark = true))
        assertEquals(true, ThemeSelection(ThemeMode.System).resolveDark(systemDark = true))
        assertEquals(false, ThemeSelection(ThemeMode.System).resolveDark(systemDark = false))
    }

    /**
     * The window background painted behind the transparent system bars is
     * `ColorScheme.background`. Whatever [resolveDark] says must therefore be
     * matched by a background of that polarity for EVERY palette -- otherwise a
     * bar region would disagree with its own icons.
     */
    @Test
    fun `background polarity matches the resolved appearance`() {
        for (palette in AccentPalette.entries) {
            for (mode in ThemeMode.entries) {
                for (systemDark in listOf(false, true)) {
                    val selection = ThemeSelection(mode, palette)
                    val background = selection.resolveColorScheme(systemDark).background
                    val luminance = 0.2126f * background.red + 0.7152f * background.green + 0.0722f * background.blue
                    if (selection.resolveDark(systemDark)) {
                        assertTrue("$mode/$palette dark background too bright: $background", luminance < 0.2f)
                    } else {
                        assertTrue("$mode/$palette light background too dark: $background", luminance > 0.8f)
                    }
                }
            }
        }
    }

    /** OLED black is a dark mode whose background is genuinely black, not merely dim. */
    @Test
    fun `oled black resolves a true black background`() {
        assertEquals(Color.Black, ThemeSelection(ThemeMode.OledBlack).resolveColorScheme(systemDark = false).background)
    }
}
