@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/**
 * The colour of each [StatusTone]. What a state is called and which tone it gets
 * is decided in ShellPresentation.kt; only the colour is decided here, and it is
 * the same RGB the desktop companions use.
 */
fun toneColor(tone: StatusTone): Color = when (tone) {
    StatusTone.Good -> Color(0xFF2EA043)
    StatusTone.Busy -> Color(0xFFD98E04)
    StatusTone.Bad -> Color(0xFFD13438)
    StatusTone.Idle -> Color(0xFF8B8B94)
}

@Composable
fun StatusDot(tone: StatusTone, modifier: Modifier = Modifier, size: Dp = 10.dp) {
    Box(modifier.size(size).background(toneColor(tone), CircleShape))
}

/** A dot and a few words: the shell's one way of stating a state. */
@Composable
fun StatusLine(tone: StatusTone, words: String, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        StatusDot(tone)
        Spacer(Modifier.width(LayoutTokens.Space2))
        Text(
            words,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

/**
 * The page column: centred, capped at [maxWidth], with the shared page padding
 * (narrower on a narrow window).
 *
 * [scroll] is the LAST RESORT for a window too small for any layout
 * (docs/architecture/companion-shell-layout.md, "Fitting the screen"). A page
 * whose content already scrolls (the Amiibo grid) or is split into tabs passes
 * false and gives its own body the fallback instead.
 */
@Composable
fun PageFrame(
    maxWidth: Dp,
    scroll: Boolean = true,
    content: @Composable ColumnScope.() -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        val side = if (this.maxWidth < LayoutTokens.NarrowPageWidth) LayoutTokens.NarrowPagePaddingSide
        else LayoutTokens.PagePaddingSide
        // A short window (a landscape handheld) gets the tight vertical rhythm:
        // the shared 12/24 top/bottom padding is a whole row of controls there.
        val short = LocalShortWindow.current
        Column(
            Modifier
                .fillMaxHeight()
                .widthIn(max = maxWidth + side * 2)
                .fillMaxWidth()
                .then(if (scroll) Modifier.verticalScroll(rememberScrollState()) else Modifier)
                .padding(
                    start = side, end = side,
                    top = if (short) LayoutTokens.Space1 else LayoutTokens.PagePaddingTop,
                    bottom = if (short) LayoutTokens.Space2 else LayoutTokens.PagePaddingBottom,
                ),
            verticalArrangement = Arrangement.spacedBy(if (short) LayoutTokens.Space2 else LayoutTokens.Space3),
            content = content,
        )
    }
}

data class PageTab(val label: String, val icon: ImageVector)

/**
 * Tabs within a page, where the page would otherwise scroll. Sized to their
 * labels (not stretched across the page) so they read as a toolbar, the same as
 * the desktop's selector bar.
 */
@Composable
fun PageTabs(
    tabs: List<PageTab>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    // Drawn here rather than with Material's tab rows: those impose a 90 dp
    // minimum tab width and stack the icon over the label, which made a
    // three-tab bar plus Keyboard & Mouse's open-profile controls too wide for
    // a 960 dp handheld and the tabs twice the control height.
    val colors = MaterialTheme.colorScheme
    Row(modifier.selectableGroup(), horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space1)) {
        tabs.forEachIndexed { index, tab ->
            val on = index == selected
            Column(
                Modifier
                    .clip(RoundedCornerShape(LayoutTokens.Space2))
                    .selectable(selected = on, role = Role.Tab, onClick = { onSelect(index) }),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Row(
                    Modifier
                        .heightIn(min = LayoutTokens.TouchHeight - 3.dp)
                        .padding(horizontal = LayoutTokens.Space3),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(
                        tab.icon, null, Modifier.size(18.dp),
                        tint = if (on) colors.primary else colors.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(
                        tab.label,
                        style = MaterialTheme.typography.labelLarge,
                        color = if (on) colors.onSurface else colors.onSurfaceVariant,
                        maxLines = 1,
                    )
                }
                Box(
                    Modifier
                        .padding(horizontal = LayoutTokens.Space3)
                        .height(3.dp)
                        .width(28.dp)
                        .background(if (on) colors.primary else Color.Transparent, RoundedCornerShape(2.dp)),
                )
            }
        }
    }
}

/**
 * The remaining-space region below a page's header and tabs. Its own scroll is
 * the last-resort fallback for a window too small for the tab's layout.
 */
@Composable
fun ColumnScope.TabBody(content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(if (LocalShortWindow.current) LayoutTokens.Space2 else LayoutTokens.Space3),
        content = content,
    )
}
