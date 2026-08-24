package dev.picoswitch.companion

import android.content.res.Configuration
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.toPixelMap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onRoot
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import dev.picoswitch.companion.ui.ThemePreferenceStore
import dev.picoswitch.companion.ui.resolveColorScheme
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/**
 * The application, not the window background, fills the system-bar regions.
 *
 * This is the regression guard for the white strips: the app is edge-to-edge, so
 * the status and navigation bars are transparent glass and whatever the app
 * paints underneath them IS their colour. When nothing was painted there, the
 * platform theme's near-white `windowBackground` (#FAFAFA) showed through and a
 * dark app sat between two white bands.
 *
 * The assertion samples the app's own window surface inside each system-bar
 * inset and requires the resolved theme background. It is deliberately
 * independent of which theme is resolved: it derives the expectation the same
 * way the app does, so it holds in light mode, in dark mode, and under a forced
 * appearance, and it fails if the app ever stops drawing there.
 *
 * A device with no system-bar insets in this window (immersive, or a form factor
 * with no bars) has nothing to assert, and the test says so rather than passing
 * vacuously.
 */
class SystemBarBackgroundTest {
    @get:Rule val rule = createAndroidComposeRule<MainActivity>()

    @Test fun systemBarRegionsUseTheApplicationBackground() {
        val activity = rule.activity
        val systemDark = (activity.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        val expected = hex(ThemePreferenceStore(activity).load().resolveColorScheme(systemDark).background.toArgb())

        val bars = rule.runOnIdle {
            ViewCompat.getRootWindowInsets(activity.window.decorView)
                ?.getInsets(WindowInsetsCompat.Type.systemBars())
        }
        assertTrue("No window insets available to sample", bars != null)
        val top = bars!!.top
        val bottom = bars.bottom
        assertTrue("This window exposes no system-bar region to test", top > 0 || bottom > 0)

        val pixels = rule.onRoot().captureToImage().toPixelMap()
        // A quarter, a half and three quarters across: a single centre sample
        // would miss a strip that is only wrong at one end.
        val columns = listOf(pixels.width / 4, pixels.width / 2, pixels.width * 3 / 4)
        for (x in columns) {
            if (top > 0) {
                assertEquals("status-bar region at x=$x", expected, hex(pixels[x, top / 2].toArgb()))
            }
            if (bottom > 0) {
                assertEquals(
                    "navigation-bar region at x=$x",
                    expected,
                    hex(pixels[x, pixels.height - bottom / 2 - 1].toArgb()),
                )
            }
        }
    }

    /** Colours are compared as hex so a failure names the two colours. */
    private fun hex(argb: Int): String = "#%08X".format(argb)
}
