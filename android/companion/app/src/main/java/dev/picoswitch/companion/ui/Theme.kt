package dev.picoswitch.companion.ui

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

object LayoutTokens {
    val Space1 = 4.dp
    val Space2 = 8.dp
    val Space3 = 12.dp
    val Space4 = 16.dp
    val Space5 = 24.dp
    val Space6 = 32.dp
    val CardRadius = 20.dp
    val ControlRadius = 14.dp
    val TouchHeight = 48.dp
    val ContentMaxWidth = 1240.dp
    val DetailWidth = 360.dp
    val NavigationBreakpoint = 720.dp
    // After the rail and page gutters, a 960dp-class landscape handheld still has room for
    // two intentional columns. Detail panes keep their own 360dp lower bound.
    val TwoPaneBreakpoint = 760.dp
}

private val DarkColors = darkColorScheme(
    primary = Color(0xFF77D8FF), onPrimary = Color(0xFF003548), primaryContainer = Color(0xFF004D66),
    secondary = Color(0xFFFFB1C3), onSecondary = Color(0xFF5E1028), secondaryContainer = Color(0xFF7A273E),
    tertiary = Color(0xFFB9C4FF), background = Color(0xFF10131A), surface = Color(0xFF171B23),
    surfaceVariant = Color(0xFF242A35), outline = Color(0xFF87919D), error = Color(0xFFFFB4AB),
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF006687), onPrimary = Color.White, primaryContainer = Color(0xFFC0E8FF),
    secondary = Color(0xFF93405A), onSecondary = Color.White, secondaryContainer = Color(0xFFFFD9E1),
    tertiary = Color(0xFF4C5D91), background = Color(0xFFF7F9FF), surface = Color(0xFFFCF8FF),
    surfaceVariant = Color(0xFFE3E8F1), outline = Color(0xFF737C87),
)

@Composable
fun CompanionTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (androidx.compose.foundation.isSystemInDarkTheme()) DarkColors else LightColors,
        shapes = Shapes(
            extraSmall = RoundedCornerShape(8.dp), small = RoundedCornerShape(12.dp),
            medium = RoundedCornerShape(LayoutTokens.ControlRadius), large = RoundedCornerShape(LayoutTokens.CardRadius),
        ),
        content = content,
    )
}
