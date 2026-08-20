@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import android.content.ClipData
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.ClipEntry
import androidx.compose.ui.platform.LocalClipboard
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch

/**
 * The companion's small shared visual vocabulary.
 *
 * Every recurring product surface -- a titled block of related controls, a
 * settings row, a label/value pair, a status indicator, a confirmation -- is
 * built here exactly once. The reason is mechanical rather than aesthetic: the
 * previous screens each re-derived their own card padding, row height and
 * dialog shape, so a spacing correction had to be repeated per screen and was
 * silently missed on the ones nobody re-opened. Nothing in this file knows
 * about the management protocol; it is presentation only.
 */

// ---------------------------------------------------------------------------
// Window size
// ---------------------------------------------------------------------------

/**
 * Available width, bucketed the way the layouts actually branch.
 *
 * Deliberately derived from the measured content width rather than the device
 * or its orientation: the app has to look intentional on square displays and
 * on wide gaming handhelds, neither of which is described by "phone" or
 * "tablet". [WindowSize.of] is pure so layout decisions stay unit-testable.
 */
enum class WindowSize {
    Compact, Medium, Expanded;

    val atLeastMedium: Boolean get() = this != Compact
    val isExpanded: Boolean get() = this == Expanded

    companion object {
        fun of(width: Dp): WindowSize = when {
            width >= LayoutTokens.ExpandedBreakpoint -> Expanded
            width >= LayoutTokens.MediumBreakpoint -> Medium
            else -> Compact
        }
    }
}

/**
 * True when the window is short enough that generous vertical rhythm costs more
 * than it gives -- landscape handhelds, split screen, and large font scales.
 */
val LocalShortWindow = staticCompositionLocalOf { false }

/**
 * Should a stack of section cards be laid out in two columns?
 *
 * Decided from the measured content width rather than from a size bucket,
 * because the bucket describes the window and this describes the space left
 * after the navigation rail and page gutters. A landscape handheld sits right
 * where those two disagree: the window is Expanded, the content column is not,
 * and reading the bucket left a 900dp-wide display running one column of very
 * wide rows.
 *
 * The threshold is two readable card columns; below it one column is better
 * than two cramped ones.
 */
fun twoColumnLayout(width: Dp): Boolean = width >= LayoutTokens.TwoPaneBreakpoint

@Composable
fun ProvideShortWindow(height: Dp, content: @Composable () -> Unit) {
    CompositionLocalProvider(LocalShortWindow provides (height < LayoutTokens.ShortWindowHeight), content = content)
}

// ---------------------------------------------------------------------------
// Screen scaffolding
// ---------------------------------------------------------------------------

/**
 * The standard page frame: an optional title, then scrolling content with one
 * consistent gap between sections.
 *
 * [actions] sits on the title's baseline row so a page-level action never has
 * to invent its own placement, which is what produced the mismatched toolbars
 * on the Amiibo and Settings pages.
 */
@Composable
fun ScreenHeader(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String = "",
    actions: @Composable RowScope.() -> Unit = {},
) {
    val short = LocalShortWindow.current
    Row(
        modifier.fillMaxWidth().heightIn(min = LayoutTokens.TouchHeight),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
    ) {
        Column(Modifier.weight(1f)) {
            Text(
                title,
                Modifier.semantics { heading() },
                style = if (short) MaterialTheme.typography.titleLarge else MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.SemiBold,
            )
            if (subtitle.isNotBlank() && !short) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        actions()
    }
}

/** A titled block of related controls. The only card shape the product uses. */
@Composable
fun SectionCard(
    modifier: Modifier = Modifier,
    title: String? = null,
    icon: ImageVector? = null,
    trailing: @Composable (RowScope.() -> Unit)? = null,
    container: Color = MaterialTheme.colorScheme.surfaceVariant,
    contentPadding: Dp = LayoutTokens.CardPadding,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = container)) {
        Column(
            Modifier.fillMaxWidth().padding(contentPadding),
            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
        ) {
            if (title != null) {
                Row(
                    Modifier.fillMaxWidth().semantics { heading() },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (icon != null) {
                        Icon(icon, null, Modifier.size(LayoutTokens.IconSize))
                        Spacer(Modifier.width(LayoutTokens.Space2))
                    }
                    Text(
                        title,
                        Modifier.weight(1f),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    trailing?.invoke(this)
                }
            }
            content()
        }
    }
}

/** A quieter heading for a group inside a [SectionCard]. */
@Composable
fun SubsectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        modifier.semantics { heading() },
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

/**
 * A tappable settings row: title, optional supporting line, optional trailing
 * control.
 *
 * The whole row is the touch target when [onClick] is supplied, so a row is
 * never harder to hit than the control inside it.
 */
@Composable
fun SettingsRow(
    title: String,
    modifier: Modifier = Modifier,
    supporting: String? = null,
    leading: ImageVector? = null,
    enabled: Boolean = true,
    onClick: (() -> Unit)? = null,
    role: Role? = null,
    trailing: @Composable (RowScope.() -> Unit)? = null,
) {
    val alpha = if (enabled) 1f else LayoutTokens.DisabledAlpha
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = LayoutTokens.TouchHeight)
            .then(
                if (onClick != null && enabled) {
                    Modifier.clip(MaterialTheme.shapes.small).clickable(role = role, onClick = onClick)
                } else Modifier,
            )
            .padding(vertical = LayoutTokens.Space1),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
    ) {
        if (leading != null) {
            Icon(
                leading,
                null,
                Modifier.size(LayoutTokens.IconSize),
                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = alpha),
            )
        }
        Column(Modifier.weight(1f)) {
            Text(
                title,
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = alpha),
            )
            if (!supporting.isNullOrBlank()) {
                Text(
                    supporting,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = alpha),
                )
            }
        }
        trailing?.invoke(this)
    }
}

/**
 * An aligned label/value pair.
 *
 * [monospace] is for identifiers -- addresses, build hashes, CRCs -- where
 * proportional digits make two values hard to compare by eye. [onCopy] adds a
 * copy affordance; it is deliberately opt-in so trivial values do not each grow
 * an icon.
 */
@Composable
fun LabelValueRow(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    monospace: Boolean = false,
    copyable: Boolean = false,
    valueColor: Color = MaterialTheme.colorScheme.onSurface,
) {
    val clipboard = LocalClipboard.current
    val scope = rememberCoroutineScope()
    Row(
        modifier.fillMaxWidth().heightIn(min = LayoutTokens.CompactRowHeight),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
    ) {
        Text(
            label,
            Modifier.widthIn(min = LayoutTokens.LabelWidth).weight(0.42f),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            value,
            Modifier.weight(0.58f),
            style = if (monospace) {
                MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace)
            } else MaterialTheme.typography.bodyMedium,
            color = valueColor,
        )
        if (copyable) {
            IconButton(
                onClick = {
                    scope.launch {
                        clipboard.setClipEntry(ClipEntry(ClipData.newPlainText(label, value)))
                    }
                },
                modifier = Modifier.size(LayoutTokens.TouchHeight),
            ) {
                Icon(Icons.Default.ContentCopy, "Copy $label", Modifier.size(18.dp))
            }
        }
    }
}

/**
 * A tight stack of [LabelValueRow]s.
 *
 * A card's section gap is sized for controls; applied between read-only rows it
 * turns a dozen values into a page of scrolling. These rows already carry their
 * own minimum height, so the group supplies only a hairline of separation.
 */
@Composable
fun DetailList(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space1),
        content = content,
    )
}

/**
 * A device presence row: what it is, what it is called, and whether it is here.
 *
 * Presence is carried by the chip's text as well as its colour, because colour
 * alone is not an accessible state indicator.
 */
@Composable
fun DeviceStatusRow(
    role: String,
    name: String?,
    connected: Boolean,
    modifier: Modifier = Modifier,
    connectedLabel: String = "Connected",
    absentLabel: String = "Not connected",
) {
    Row(
        modifier.fillMaxWidth().heightIn(min = LayoutTokens.TouchHeight),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
    ) {
        Text(
            role,
            Modifier.widthIn(min = LayoutTokens.LabelWidth),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            if (connected) name?.takeIf(String::isNotBlank) ?: "Connected device" else "—",
            Modifier.weight(1f),
            style = MaterialTheme.typography.bodyMedium,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        StatusChip(
            if (connected) connectedLabel else absentLabel,
            tone = if (connected) ChipTone.Positive else ChipTone.Neutral,
        )
    }
}

// ---------------------------------------------------------------------------
// Indicators
// ---------------------------------------------------------------------------

enum class ChipTone { Positive, Neutral, Attention, Error }

@Composable
fun StatusChip(label: String, modifier: Modifier = Modifier, tone: ChipTone = ChipTone.Neutral) {
    val (container, content) = when (tone) {
        ChipTone.Positive -> MaterialTheme.colorScheme.primaryContainer to MaterialTheme.colorScheme.onPrimaryContainer
        ChipTone.Neutral -> MaterialTheme.colorScheme.surface to MaterialTheme.colorScheme.onSurfaceVariant
        ChipTone.Attention -> MaterialTheme.colorScheme.tertiaryContainer to MaterialTheme.colorScheme.onTertiaryContainer
        ChipTone.Error -> MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
    }
    Surface(modifier, shape = CircleShape, color = container) {
        Text(
            label,
            Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space1),
            style = MaterialTheme.typography.labelMedium,
            color = content,
            maxLines = 1,
        )
    }
}

/** A compact inline notice. Not an alert -- ordinary contextual information. */
@Composable
fun InlineNotice(
    text: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    tone: ChipTone = ChipTone.Neutral,
) {
    val (container, content) = when (tone) {
        ChipTone.Positive -> MaterialTheme.colorScheme.primaryContainer to MaterialTheme.colorScheme.onPrimaryContainer
        ChipTone.Neutral -> MaterialTheme.colorScheme.surface to MaterialTheme.colorScheme.onSurfaceVariant
        ChipTone.Attention -> MaterialTheme.colorScheme.tertiaryContainer to MaterialTheme.colorScheme.onTertiaryContainer
        ChipTone.Error -> MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
    }
    Surface(modifier.fillMaxWidth(), shape = MaterialTheme.shapes.small, color = container) {
        Row(
            Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        ) {
            if (icon != null) Icon(icon, null, Modifier.size(LayoutTokens.IconSize), tint = content)
            Text(text, style = MaterialTheme.typography.bodySmall, color = content)
        }
    }
}

@Composable
fun EmptyStateBlock(
    icon: ImageVector,
    title: String,
    body: String,
    modifier: Modifier = Modifier,
    action: @Composable (() -> Unit)? = null,
) {
    Box(modifier, contentAlignment = Alignment.Center) {
        Column(
            Modifier.widthIn(max = LayoutTokens.EmptyStateWidth).padding(LayoutTokens.Space5),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        ) {
            Icon(icon, null, Modifier.size(44.dp), tint = MaterialTheme.colorScheme.primary)
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(
                body,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
            action?.invoke()
        }
    }
}

/** A round colour swatch that also announces what it represents. */
@Composable
fun ColorSwatch(color: Color, description: String, modifier: Modifier = Modifier, size: Dp = 28.dp) {
    Box(
        modifier
            .size(size)
            .clip(CircleShape)
            .background(color)
            .semantics { contentDescription = description },
    )
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

/**
 * A single-choice segmented control.
 *
 * Falls back to a wrapping chip row when the options cannot fit as equal
 * segments. The threshold is the measured width of the *longest* label rather
 * than a constant, because a constant is wrong in both directions: it clipped
 * "Keyboard + Mouse" into "Keyboard…" in a two-column landscape, and it would
 * push four short labels into chips on a display where they fit. Measuring also
 * makes the control correct at any font scale for free.
 */
@Composable
fun <T> SegmentedSelector(
    options: List<T>,
    selected: T?,
    label: (T) -> String,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    val measurer = rememberTextMeasurer()
    val labelStyle = MaterialTheme.typography.labelLarge
    val density = LocalDensity.current
    val widest = remember(options, labelStyle, density) {
        options.maxOfOrNull { option ->
            with(density) { measurer.measure(label(option), labelStyle).size.width.toDp() }
        } ?: 0.dp
    }
    BoxWithConstraints(modifier.fillMaxWidth()) {
        val perSegment = maxWidth / options.size.coerceAtLeast(1)
        // The selected segment also carries a check icon, so its label has less
        // room than the others; SegmentPadding covers that plus the button's
        // own horizontal inset.
        if (perSegment >= widest + LayoutTokens.SegmentPadding) {
            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                options.forEachIndexed { index, option ->
                    SegmentedButton(
                        selected = option == selected,
                        onClick = { onSelect(option) },
                        enabled = enabled,
                        shape = SegmentedButtonDefaults.itemShape(index, options.size),
                        label = { Text(label(option), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    )
                }
            }
        } else {
            FlowRowCompat {
                options.forEach { option ->
                    FilterChip(
                        selected = option == selected,
                        onClick = { onSelect(option) },
                        enabled = enabled,
                        label = { Text(label(option)) },
                        leadingIcon = if (option == selected) {
                            { Icon(Icons.Default.Check, null, Modifier.size(16.dp)) }
                        } else null,
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun FlowRowCompat(modifier: Modifier = Modifier, content: @Composable FlowRowScope.() -> Unit) {
    FlowRow(
        modifier,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        content = content,
    )
}

/** An advanced/secondary group that stays out of the way until asked for. */
@Composable
fun ExpandableSection(
    title: String,
    expanded: Boolean,
    onToggle: () -> Unit,
    modifier: Modifier = Modifier,
    summary: String? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(modifier.fillMaxWidth()) {
        SettingsRow(
            title = title,
            supporting = summary,
            onClick = onToggle,
            trailing = {
                Icon(
                    if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                    if (expanded) "Collapse $title" else "Expand $title",
                )
            },
        )
        AnimatedVisibility(
            visible = expanded,
            enter = expandVertically() + fadeIn(),
            exit = shrinkVertically() + fadeOut(),
        ) {
            Column(
                Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2),
                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
                content = content,
            )
        }
    }
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

/**
 * The single dialog shape the product uses.
 *
 * Every popup goes through here so title alignment, side padding, corner
 * radius, button order and the maximum width are decided once. The width bound
 * matters most on wide handhelds, where an unbounded Material dialog stretches
 * across the whole display and stops reading as a focused decision.
 */
@Composable
fun PicoDialog(
    onDismiss: () -> Unit,
    title: String,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    confirmLabel: String? = null,
    onConfirm: (() -> Unit)? = null,
    confirmEnabled: Boolean = true,
    dismissLabel: String = "Cancel",
    destructive: Boolean = false,
    content: @Composable ColumnScope.() -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        modifier = modifier.widthIn(max = LayoutTokens.DialogMaxWidth),
        icon = icon?.let { { Icon(it, null) } },
        title = { Text(title, style = MaterialTheme.typography.headlineSmall) },
        text = {
            Column(
                Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
                content = content,
            )
        },
        confirmButton = {
            if (onConfirm != null && confirmLabel != null) {
                TextButton(
                    onClick = onConfirm,
                    enabled = confirmEnabled,
                    colors = if (destructive) {
                        ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)
                    } else ButtonDefaults.textButtonColors(),
                ) { Text(confirmLabel) }
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(dismissLabel) } },
        shape = MaterialTheme.shapes.large,
    )
}

/** A confirmation whose whole body is one explanatory sentence. */
@Composable
fun ConfirmDialog(
    onDismiss: () -> Unit,
    title: String,
    body: String,
    confirmLabel: String,
    onConfirm: () -> Unit,
    confirmEnabled: Boolean = true,
    destructive: Boolean = false,
    icon: ImageVector? = null,
) {
    PicoDialog(
        onDismiss = onDismiss,
        title = title,
        icon = icon,
        confirmLabel = confirmLabel,
        onConfirm = onConfirm,
        confirmEnabled = confirmEnabled,
        destructive = destructive,
    ) {
        Text(body, style = MaterialTheme.typography.bodyMedium)
    }
}
