package dev.picoswitch.companion.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.text.TextAutoSize
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.TextUnit
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.picoswitch.companion.model.KbmBinding
import dev.picoswitch.companion.model.KbmDestination
import dev.picoswitch.companion.model.KbmSource
import dev.picoswitch.management.KbmSourceKind
import dev.picoswitch.companion.model.KeyCap
import dev.picoswitch.companion.model.KeyboardCluster
import dev.picoswitch.companion.model.KeyboardGeometry
import dev.picoswitch.companion.model.label
import dev.picoswitch.companion.model.title

/** The drawn keyboard's size for a given space, so a card can hug it. */
internal object DrawnKeyboardSize {
    private val totalUnits: Double
        get() = KeyboardGeometry.clusters.sumOf { it.columns } + ClusterGapUnits * (KeyboardGeometry.clusters.size - 1)
    private val totalRows: Double get() = KeyboardGeometry.clusters.maxOf { it.rows }

    /** The key unit [DrawnKeyboard] picks for a space of [width] x [height]. */
    fun unitFor(width: Dp, height: Dp): Dp =
        minOf(width / totalUnits.toFloat(), height / totalRows.toFloat()).coerceIn(MinKeyUnit, MaxKeyUnit)

    fun width(unit: Dp): Dp = unit * totalUnits.toFloat()
    fun height(unit: Dp): Dp = unit * totalRows.toFloat()
}

/**
 * The keyboard, drawn: the Android counterpart of the desktop companions'
 * key grid (Windows `KeyboardMousePage`, Linux `KeyboardMousePage.BuildKeyboard`),
 * from the same geometry ([KeyboardGeometry], parity-tested against the C#).
 *
 * Tapping a key opens the binding editor; long-pressing a bound key clears it
 * (the desktop's middle-click, through the same edit). The binding is printed
 * ON the key, and a changed key is filled with the accent and says "changed"
 * to a screen reader, because colour alone carries nothing to one.
 *
 * Sized by measurement, not by device: the key unit is the largest that fits
 * BOTH the width and the height offered, up to [MaxKeyUnit], so the whole
 * keyboard is on screen without scrolling. Only below [MinKeyUnit] does it keep
 * its size and scroll instead — a phone in portrait cannot show 104 legible
 * keys, and shrinking them further makes every key a mis-tap. The height is
 * honoured only when the caller gives a bounded one (a weighted region, not a
 * scroll).
 */
@Composable
internal fun DrawnKeyboard(
    bindings: List<KbmBinding>,
    enabled: Boolean,
    onEdit: (KbmBinding) -> Unit,
    onClear: (KbmBinding) -> Unit,
) {
    val byUsage = bindings.associateBy { it.source.code }

    BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.TopCenter) {
        val unit = DrawnKeyboardSize.unitFor(maxWidth, if (constraints.hasBoundedHeight) maxHeight else Dp.Infinity)

        Row(
            Modifier
                .horizontalScroll(rememberScrollState())
                .verticalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(unit * ClusterGapUnits.toFloat()),
        ) {
            KeyboardGeometry.clusters.forEach { cluster ->
                ClusterCanvas(cluster, unit, byUsage, enabled, onEdit, onClear)
            }
        }
    }
}

/** The ISO and Japanese keys: bindable, sized to their content, never given a false position. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun OtherKeys(
    bindings: List<KbmBinding>,
    enabled: Boolean,
    onEdit: (KbmBinding) -> Unit,
    onClear: (KbmBinding) -> Unit,
) {
    val byUsage = bindings.associateBy { it.source.code }
    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(KeyGap),
        verticalArrangement = Arrangement.spacedBy(KeyGap),
    ) {
        KeyboardGeometry.other.keys.forEach { cap ->
            KeyButton(
                cap,
                byUsage[cap.usage] ?: unbound(cap),
                enabled,
                Modifier.widthIn(min = OtherKeyUnit * cap.width.toFloat()).height(OtherKeyUnit - KeyGap),
                onEdit,
                onClear,
            )
        }
    }
}

/**
 * Mouse buttons as tiles in the same style as the keys: tap to change, hold to
 * clear, the binding printed on the tile. Rows of "Left Button → ZR" took a
 * 48 dp row each, five of them more than a landscape handheld's page.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun MouseButtonTiles(
    bindings: List<KbmBinding>,
    enabled: Boolean,
    onEdit: (KbmBinding) -> Unit,
    onClear: (KbmBinding) -> Unit,
) {
    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(KeyGap),
        verticalArrangement = Arrangement.spacedBy(KeyGap),
    ) {
        bindings.forEach { binding ->
            KeyButton(
                KeyCap(binding.source.code, binding.source.label, 0.0, 0.0),
                binding,
                enabled,
                Modifier.widthIn(min = MouseTileWidth).height(MouseTileHeight),
                onEdit,
                onClear,
            )
        }
    }
}

@Composable
private fun ClusterCanvas(
    cluster: KeyboardCluster,
    unit: Dp,
    byUsage: Map<Int, KbmBinding>,
    enabled: Boolean,
    onEdit: (KbmBinding) -> Unit,
    onClear: (KbmBinding) -> Unit,
) {
    Box(Modifier.size(unit * cluster.columns.toFloat(), unit * cluster.rows.toFloat())) {
        cluster.keys.forEach { cap ->
            KeyButton(
                cap,
                byUsage[cap.usage] ?: unbound(cap),
                enabled,
                Modifier
                    .offset(unit * cap.column.toFloat(), unit * cap.row.toFloat())
                    .size(unit * cap.width.toFloat() - KeyGap, unit * cap.height.toFloat() - KeyGap),
                onEdit,
                onClear,
            )
        }
    }
}

/** A key nobody has bound; the editor offers every destination for it, as on desktop. */
private fun unbound(cap: KeyCap): KbmBinding = KbmBinding(
    KbmSource(KbmSourceKind.Key, cap.usage),
    KbmDestination.None,
    custom = false,
)

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun KeyButton(
    cap: KeyCap,
    binding: KbmBinding,
    enabled: Boolean,
    modifier: Modifier,
    onEdit: (KbmBinding) -> Unit,
    onClear: (KbmBinding) -> Unit,
) {
    val bound = binding.destination != KbmDestination.None
    val colors = MaterialTheme.colorScheme
    val container = when {
        binding.custom -> colors.primaryContainer
        bound -> colors.surfaceContainerHighest
        else -> colors.surfaceContainerLow
    }
    val content = if (binding.custom) colors.onPrimaryContainer else colors.onSurface
    val description = if (bound) {
        "${cap.label}, mapped to ${binding.destination.title}" + if (binding.custom) ", changed" else ""
    } else {
        "${cap.label}, not mapped"
    }

    Surface(
        modifier = modifier
            .semantics {
                contentDescription = description
                role = Role.Button
                if (bound && enabled) {
                    customActions = listOf(CustomAccessibilityAction("Clear") { onClear(binding); true })
                }
            }
            .combinedClickable(
                enabled = enabled,
                onClick = { onEdit(binding) },
                // Only a bound key: clearing an unmapped one would dirty the
                // draft with a change that alters nothing.
                onLongClick = if (bound) ({ onClear(binding) }) else null,
            ),
        shape = RoundedCornerShape(KeyRadius),
        color = container,
        contentColor = content,
        border = BorderStroke(1.dp, colors.outlineVariant),
    ) {
        Column(
            Modifier.padding(horizontal = 3.dp, vertical = 1.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // The key's NAME shrinks to fit, never an ellipsis
            // (companion-shell-layout.md): "Ho…" on a one-unit Home key is a key
            // the user cannot identify. The binding under it shrinks too and
            // trims only at the floor, as on Windows: the full name is in the
            // accessible name and the editor. Each line gets half the key, so a
            // small key shrinks both rather than clipping the second.
            Box(Modifier.weight(1f, fill = false), contentAlignment = Alignment.Center) {
                FitText(cap.label, TextStyle(color = content), maxSize = 12.sp)
            }
            if (bound) {
                Box(Modifier.weight(1f, fill = false), contentAlignment = Alignment.Center) {
                    FitText(
                        binding.destination.title,
                        TextStyle(
                            color = if (binding.custom) content else colors.primary,
                            fontWeight = FontWeight.SemiBold,
                        ),
                        maxSize = 10.sp,
                    )
                }
            }
        }
    }
}

/**
 * One line that shrinks (to a floor) until it fits the key, as the desktop keys
 * do. Only at the floor does it trim, which a key NAME does not reach: the
 * longest one-unit names (Pause, Right) fit a [MinKeyUnit] key.
 */
@Composable
private fun FitText(text: String, style: TextStyle, maxSize: TextUnit) {
    BasicText(
        text,
        style = style.copy(textAlign = TextAlign.Center),
        maxLines = 1,
        softWrap = false,
        overflow = TextOverflow.Ellipsis,
        autoSize = TextAutoSize.StepBased(minFontSize = 6.sp, maxFontSize = maxSize, stepSize = 0.5.sp),
    )
}

/**
 * Below this the keyboard keeps its size and scrolls rather than shrinking.
 *
 * Measured, not chosen per device: the keyboard is 24 units wide, and a
 * landscape handheld around 830 dp wide has about 730 dp for it once the page
 * and card padding are taken, so a 28 dp floor keeps every key on such a screen
 * with no scrolling. Below it the keys become mis-taps.
 */
private val MinKeyUnit = 28.dp

/**
 * The ISO and Japanese keys sit in a wrapping row, not on the keyboard, so they
 * keep the shell's control height rather than following the keyboard's unit.
 */
private val OtherKeyUnit = 36.dp

/** Mouse-button tiles: room for "Back (Mouse 4)" over a binding, two lines. */
private val MouseTileWidth = 104.dp
private val MouseTileHeight = 48.dp

/** The desktop companions' fixed key unit (46): the keyboard stops growing there. */
private val MaxKeyUnit = 46.dp

private val KeyGap = 4.dp
private val KeyRadius = 6.dp

/** The space between main block, navigation cluster and numpad, in key units. */
private const val ClusterGapUnits = 0.5
