package dev.picoswitch.companion.ui

import android.app.Activity
import android.content.Context
import android.content.SharedPreferences
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import androidx.core.view.WindowCompat

/**
 * The one place a spacing, size or breakpoint number is decided.
 *
 * A literal dp in a screen is treated as suspicious: the earlier layouts each
 * carried their own card padding and row heights, so cards on adjacent screens
 * did not line up and a correction had to be made once per screen. Anything
 * reused by more than one composable belongs here.
 */
object LayoutTokens {
    val Space1 = 4.dp
    val Space2 = 8.dp
    val Space3 = 12.dp
    val Space4 = 16.dp
    val Space5 = 24.dp
    val Space6 = 32.dp
    val CardRadius = 20.dp
    val ControlRadius = 14.dp
    val CardPadding = 16.dp

    /** Material's minimum comfortable touch target; every tappable row honours it. */
    val TouchHeight = 48.dp
    /** Read-only rows carry no touch target, so they may be tighter than [TouchHeight]. */
    val CompactRowHeight = 32.dp
    val IconSize = 20.dp
    val LabelWidth = 96.dp
    val DisabledAlpha = 0.38f

    val ContentMaxWidth = 1240.dp
    val DetailWidth = 360.dp
    val EmptyStateWidth = 340.dp
    /**
     * Amiibo grid cell sizing. The minimum keeps the artwork recognisable on a
     * narrow phone; the adaptive column count then uses whatever extra width a
     * handheld or tablet has instead of stretching a fixed number of columns.
     */
    val AmiiboCellMinWidth = 132.dp
    val AmiiboArtHeight = 84.dp
    /**
     * Width the Amiibo toolbar needs before filter, sort and scan can stay as
     * their own buttons: six 48 dp targets plus a readable title. Below it they
     * collapse into the overflow menu.
     */
    val AmiiboToolbarWideWidth = 480.dp
    /** Three appearance tiles need this much width before they beat three rows. */
    val ColorTileRowMinWidth = 320.dp
    /** Beyond this a dialog stops reading as a focused decision on a wide handheld. */
    val DialogMaxWidth = 520.dp
    /**
     * A scrolling list inside a dialog. Bounded so the dialog cannot grow past
     * the buttons in landscape, where the whole window is barely taller than
     * this.
     */
    val DialogListMaxHeight = 280.dp
    /**
     * Space a segmented button needs beyond its label text: the button's own
     * horizontal inset plus the selected segment's leading check icon.
     */
    val SegmentPadding = 40.dp

    val NavigationBreakpoint = 720.dp
    /**
     * Width one navigation-bar label needs at font scale 1.0, including the
     * item's own padding. Compared against the per-item share of the bar to
     * decide whether labels fit or the bar drops to icon-only.
     */
    val NavLabelWidth = 78.dp
    // After the rail and page gutters, a 960dp-class landscape handheld still has room for
    // two intentional columns. Detail panes keep their own 360dp lower bound.
    val TwoPaneBreakpoint = 760.dp
    /** Width buckets consumed through [WindowSize]; never branch on device names. */
    val MediumBreakpoint = 600.dp
    val ExpandedBreakpoint = 840.dp
    /**
     * Under this height the page uses its tighter vertical rhythm. Landscape
     * handhelds and large font scales both land here, and both are cases where
     * generous section gaps cost a whole extra screen of scrolling.
     */
    val ShortWindowHeight = 560.dp
}

/** The complete appearance choice stored by the app. Firmware/controller colors are separate. */
data class ThemeSelection(
    val mode: ThemeMode = ThemeMode.System,
    val palette: AccentPalette = AccentPalette.Standard,
)

enum class ThemeMode(val key: String, val title: String, val description: String) {
    System("system", "System", "Follow Android's light or dark setting"),
    Light("light", "Light", "Bright surfaces for daylight use"),
    Dark("dark", "Dark", "Dim surfaces while retaining readable contrast"),
    OledBlack("oled_black", "OLED black", "True black backgrounds for OLED displays"),
    ;

    companion object {
        fun fromKey(key: String?): ThemeMode = entries.firstOrNull { it.key == key } ?: System
    }
}

/**
 * UI-only accents inspired by the colors documented for Joy-Con hardware. These never change
 * the adapter's body/lightbar/Joy-Con identity configuration.
 */
enum class AccentPalette(
    val key: String,
    val title: String,
    val description: String,
    val leftSwatch: Color,
    val rightSwatch: Color,
) {
    Standard(
        key = "standard",
        title = "PicoSwitch",
        description = "Cyan and rose application accents",
        leftSwatch = Color(0xFF77D8FF),
        rightSwatch = Color(0xFFFFB1C3),
    ),
    JoyCon1Inspired(
        key = "joycon1_inspired",
        title = "Joy-Con 1 inspired",
        description = "Blue and red accents inspired by the original pair",
        leftSwatch = Color(0xFF00B4E6),
        rightSwatch = Color(0xFFFF3C28),
    ),
    JoyCon2Inspired(
        key = "joycon2_inspired",
        title = "Joy-Con 2 inspired",
        description = "Verified light-blue and coral accent hues",
        leftSwatch = Color(0xFF9BE1E6),
        rightSwatch = Color(0xFFFF8C5F),
    ),
    ;

    companion object {
        fun fromKey(key: String?): AccentPalette = entries.firstOrNull { it.key == key } ?: Standard
    }
}

/** Pure codec keeps preference migration deterministic and easy to test without an Android device. */
object ThemePreferenceCodec {
    fun decode(modeKey: String?, paletteKey: String?): ThemeSelection = ThemeSelection(
        mode = ThemeMode.fromKey(modeKey),
        palette = AccentPalette.fromKey(paletteKey),
    )

    fun encode(selection: ThemeSelection): Map<String, String> = mapOf(
        ThemePreferenceStore.MODE_KEY to selection.mode.key,
        ThemePreferenceStore.PALETTE_KEY to selection.palette.key,
    )
}

class ThemePreferenceStore(context: Context) {
    private val preferences: SharedPreferences = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)

    fun load(): ThemeSelection = ThemePreferenceCodec.decode(
        preferences.getString(MODE_KEY, null),
        preferences.getString(PALETTE_KEY, null),
    )

    fun save(selection: ThemeSelection) {
        val encoded = ThemePreferenceCodec.encode(selection)
        preferences.edit {
            putString(MODE_KEY, encoded.getValue(MODE_KEY))
            putString(PALETTE_KEY, encoded.getValue(PALETTE_KEY))
        }
    }

    companion object {
        internal const val FILE_NAME = "appearance"
        internal const val MODE_KEY = "theme_mode"
        internal const val PALETTE_KEY = "accent_palette"
    }
}

private data class AccentTokens(
    val lightPrimary: Color,
    val lightOnPrimary: Color,
    val lightPrimaryContainer: Color,
    val lightOnPrimaryContainer: Color,
    val lightSecondary: Color,
    val lightOnSecondary: Color,
    val lightSecondaryContainer: Color,
    val lightOnSecondaryContainer: Color,
    val darkPrimary: Color,
    val darkOnPrimary: Color,
    val darkPrimaryContainer: Color,
    val darkOnPrimaryContainer: Color,
    val darkSecondary: Color,
    val darkOnSecondary: Color,
    val darkSecondaryContainer: Color,
    val darkOnSecondaryContainer: Color,
)

private fun AccentPalette.tokens(): AccentTokens = when (this) {
    AccentPalette.Standard -> AccentTokens(
        lightPrimary = Color(0xFF006687), lightOnPrimary = Color.White,
        lightPrimaryContainer = Color(0xFFC0E8FF), lightOnPrimaryContainer = Color(0xFF001F2A),
        lightSecondary = Color(0xFF93405A), lightOnSecondary = Color.White,
        lightSecondaryContainer = Color(0xFFFFD9E1), lightOnSecondaryContainer = Color(0xFF3B071A),
        darkPrimary = Color(0xFF77D8FF), darkOnPrimary = Color(0xFF003548),
        darkPrimaryContainer = Color(0xFF004D66), darkOnPrimaryContainer = Color(0xFFBFEAFF),
        darkSecondary = Color(0xFFFFB1C3), darkOnSecondary = Color(0xFF5E1028),
        darkSecondaryContainer = Color(0xFF7A273E), darkOnSecondaryContainer = Color(0xFFFFD9E1),
    )
    AccentPalette.JoyCon1Inspired -> AccentTokens(
        lightPrimary = Color(0xFF00627D), lightOnPrimary = Color.White,
        lightPrimaryContainer = Color(0xFFB8EBFF), lightOnPrimaryContainer = Color(0xFF001F29),
        lightSecondary = Color(0xFFA52B22), lightOnSecondary = Color.White,
        lightSecondaryContainer = Color(0xFFFFDAD5), lightOnSecondaryContainer = Color(0xFF3B0805),
        darkPrimary = Color(0xFF8FDBFF), darkOnPrimary = Color(0xFF003548),
        darkPrimaryContainer = Color(0xFF004D66), darkOnPrimaryContainer = Color(0xFFB8EBFF),
        darkSecondary = Color(0xFFFF8A80), darkOnSecondary = Color(0xFF57110B),
        darkSecondaryContainer = Color(0xFF7A1E17), darkOnSecondaryContainer = Color(0xFFFFDAD5),
    )
    AccentPalette.JoyCon2Inspired -> AccentTokens(
        // The source swatches are the genuine Joy-Con 2 values recorded by the repository;
        // these darker light-theme roles preserve readable text on white controls.
        lightPrimary = Color(0xFF006A72), lightOnPrimary = Color.White,
        lightPrimaryContainer = Color(0xFFB7F0F3), lightOnPrimaryContainer = Color(0xFF002022),
        lightSecondary = Color(0xFFA43D1E), lightOnSecondary = Color.White,
        lightSecondaryContainer = Color(0xFFFFDCCF), lightOnSecondaryContainer = Color(0xFF3B0C00),
        darkPrimary = Color(0xFF9BE1E6), darkOnPrimary = Color(0xFF00363A),
        darkPrimaryContainer = Color(0xFF164E52), darkOnPrimaryContainer = Color(0xFFB7F0F3),
        darkSecondary = Color(0xFFFFB59B), darkOnSecondary = Color(0xFF571A08),
        darkSecondaryContainer = Color(0xFF7A2D16), darkOnSecondaryContainer = Color(0xFFFFDCCF),
    )
}

private fun lightColors(palette: AccentPalette): ColorScheme {
    val a = palette.tokens()
    return lightColorScheme(
        primary = a.lightPrimary,
        onPrimary = a.lightOnPrimary,
        primaryContainer = a.lightPrimaryContainer,
        onPrimaryContainer = a.lightOnPrimaryContainer,
        secondary = a.lightSecondary,
        onSecondary = a.lightOnSecondary,
        secondaryContainer = a.lightSecondaryContainer,
        onSecondaryContainer = a.lightOnSecondaryContainer,
        tertiary = Color(0xFF4C5D91),
        onTertiary = Color.White,
        tertiaryContainer = Color(0xFFDCE2FF),
        onTertiaryContainer = Color(0xFF061743),
        background = Color(0xFFF7F9FF),
        onBackground = Color(0xFF191B20),
        surface = Color(0xFFFCF8FF),
        onSurface = Color(0xFF191B20),
        surfaceVariant = Color(0xFFE3E8F1),
        onSurfaceVariant = Color(0xFF41474D),
        outline = Color(0xFF71787E),
        error = Color(0xFFBA1A1A),
        onError = Color.White,
        errorContainer = Color(0xFFFFDAD6),
        onErrorContainer = Color(0xFF410002),
        inverseSurface = Color(0xFF2E3035),
        inverseOnSurface = Color(0xFFF0F0F7),
        inversePrimary = Color(0xFF77D8FF),
    )
}

private fun darkColors(palette: AccentPalette, oled: Boolean): ColorScheme {
    val a = palette.tokens()
    return darkColorScheme(
        primary = a.darkPrimary,
        onPrimary = a.darkOnPrimary,
        primaryContainer = a.darkPrimaryContainer,
        onPrimaryContainer = a.darkOnPrimaryContainer,
        secondary = a.darkSecondary,
        onSecondary = a.darkOnSecondary,
        secondaryContainer = a.darkSecondaryContainer,
        onSecondaryContainer = a.darkOnSecondaryContainer,
        tertiary = Color(0xFFB9C4FF),
        onTertiary = Color(0xFF1B2B61),
        tertiaryContainer = Color(0xFF333F76),
        onTertiaryContainer = Color(0xFFDCE2FF),
        background = if (oled) Color.Black else Color(0xFF10131A),
        onBackground = Color(0xFFE2E2E9),
        surface = if (oled) Color.Black else Color(0xFF171B23),
        onSurface = Color(0xFFE2E2E9),
        surfaceVariant = if (oled) Color(0xFF1A1A1A) else Color(0xFF242A35),
        onSurfaceVariant = Color(0xFFBEC7D3),
        outline = Color(0xFF87919D),
        error = Color(0xFFFFB4AB),
        onError = Color(0xFF690005),
        errorContainer = Color(0xFF93000A),
        onErrorContainer = Color(0xFFFFDAD6),
        inverseSurface = Color(0xFFE2E2E9),
        inverseOnSurface = Color(0xFF2E3035),
        inversePrimary = Color(0xFF006687),
    )
}

@Composable
fun CompanionTheme(
    selection: ThemeSelection = ThemeSelection(),
    content: @Composable () -> Unit,
) {
    val systemDark = isSystemInDarkTheme()
    val dark = when (selection.mode) {
        ThemeMode.System -> systemDark
        ThemeMode.Light -> false
        ThemeMode.Dark, ThemeMode.OledBlack -> true
    }
    val colors = when {
        selection.mode == ThemeMode.OledBlack -> darkColors(selection.palette, oled = true)
        dark -> darkColors(selection.palette, oled = false)
        else -> lightColors(selection.palette)
    }
    val view = LocalView.current
    val context = LocalContext.current
    if (!view.isInEditMode) {
        SideEffect {
            (context as? Activity)?.window?.let { window ->
                window.statusBarColor = colors.background.toArgb()
                window.navigationBarColor = colors.background.toArgb()
                WindowCompat.getInsetsController(window, view).apply {
                    isAppearanceLightStatusBars = !dark
                    isAppearanceLightNavigationBars = !dark
                }
            }
        }
    }
    MaterialTheme(
        colorScheme = colors,
        shapes = Shapes(
            extraSmall = RoundedCornerShape(8.dp), small = RoundedCornerShape(12.dp),
            medium = RoundedCornerShape(LayoutTokens.ControlRadius), large = RoundedCornerShape(LayoutTokens.CardRadius),
        ),
        content = content,
    )
}
