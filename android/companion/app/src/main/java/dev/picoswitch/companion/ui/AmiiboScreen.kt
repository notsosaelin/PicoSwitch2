@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import android.graphics.BitmapFactory
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.foundation.Image
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.background
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.automirrored.filled.Sort
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.data.AmiiboCard
import dev.picoswitch.companion.data.AmiiboCategory
import dev.picoswitch.companion.data.AmiiboGallery
import dev.picoswitch.companion.data.AmiiboInspection
import dev.picoswitch.companion.data.AmiiboInteraction
import dev.picoswitch.companion.data.AmiiboInteractionState
import dev.picoswitch.companion.data.AmiiboGalleryFilters
import dev.picoswitch.companion.data.AmiiboGalleryOptions
import dev.picoswitch.companion.data.AmiiboSort
import dev.picoswitch.companion.data.AmiiboViewMode
import dev.picoswitch.companion.model.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

/**
 * The Amiibo library.
 *
 * Artwork first: this page is a shelf of figures, so the grid is the page and
 * everything else is contextual. Search is an action rather than a permanent
 * field, library maintenance lives in Amiibo Settings, and per-item operations
 * open from the item rather than sitting on every card.
 */
@Composable
fun AmiiboScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onImport: () -> Unit,
    onImportKeys: () -> Unit,
    onScan: () -> Unit,
) {
    // The query lives in the ViewModel, not here. As Compose locals these reset
    // whenever the screen left composition, and — the reason that matters — the
    // card list would be rebuilt from state a selection could reach, which is
    // exactly how the Windows page lost its scroll position on every click.
    val filters = ui.amiiboFilters
    val query = filters.search
    var searchOpen by rememberSaveable { mutableStateOf(false) }

    // WHETHER details are open is domain state, shared with Windows and pinned
    // by tests. Only WHERE they appear is a layout question, and this is the
    // answer to it — hoisted out of BoxWithConstraints so the sheet, which is
    // rendered outside it, can ask.
    var twoColumnDetail by remember { mutableStateOf(false) }
    val paneOpen = ui.amiiboInteraction.inspectorOpen
    val inspected = ui.library.firstOrNull { it.id == ui.amiiboInteraction.inspectedId }

    // Back cancels a selection, then closes the details surface, and only then
    // leaves the screen. Enabled only when there is something to dismiss, so it
    // is never swallowed on an ordinary browsing screen.
    BackHandler(enabled = ui.amiiboInteraction.selecting || paneOpen) {
        viewModel.backFromAmiibo()
    }

    val adapter = ui.snapshot.amiibo
    val adapterLoaded = adapter.loaded || adapter.v3Loaded
    val selected = ui.library.firstOrNull { it.id == ui.selectedAmiiboId }
    val adapterMatchesSelected = selected != null &&
        AmiiboGallery.residentOn(selected, adapter.uid, adapterLoaded)
    val adapterOnly = adapterLoaded && !adapterMatchesSelected

    // Indexed by FIGURE id once, not scanned per item. amiiboCatalogEntries is
    // keyed by library item id, so looking a figure up by scanning its values
    // was a full catalog walk for each of a thousand-plus rows.
    val byFigureId = remember(ui.amiiboCatalogEntries) {
        ui.amiiboCatalogEntries.values.associateBy { it.id.uppercase() }
    }
    val catalogFor: (String) -> AmiiboCatalogEntry? = { byFigureId[it.uppercase()] }

    // ONE projection, shared by all three views. Keyed on the library, the
    // catalog and the QUERY — not on the selection, so selecting cannot rebuild
    // the list and cannot move the scroll position.
    //
    // The catalog IS a key: it arrives asynchronously, long after the first
    // projection, and it carries the title and artwork every card shows. A
    // signature that omitted it would leave a library of placeholders that never
    // resolved. The Windows companion shipped exactly that bug.
    val cards = remember(ui.library, ui.amiiboCatalogEntries, filters.queryIdentity, adapterLoaded) {
        AmiiboGallery.build(
            ui.library,
            catalogFor,
            AmiiboGallery.residentId(ui.library, adapter.uid, adapterLoaded),
            filters,
        )
    }
    val options = remember(ui.library, ui.amiiboCatalogEntries) {
        AmiiboGallery.options(ui.library, catalogFor)
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)

        // Unfolding or rotating between the two treatments must not leave the
        // same figure described twice, once over the other.
        LaunchedEffect(twoColumn) { twoColumnDetail = twoColumn }

        Column(Modifier.fillMaxSize()) {
            AmiiboToolbar(
                ui = ui,
                viewModel = viewModel,
                count = ui.library.size,
                matches = cards.size,
                filters = filters,
                options = options,
                searchOpen = searchOpen,
                onSearchOpen = { searchOpen = it; if (!it) viewModel.setAmiiboSearch("") },
                onImport = onImport,
                onScan = onScan,
            )
            Spacer(Modifier.height(LayoutTokens.Space2))

            ui.libraryWarnings.firstOrNull()?.let {
                InlineNotice(it, icon = Icons.Default.Warning, tone = ChipTone.Error)
                Spacer(Modifier.height(LayoutTokens.Space2))
            }
            // The scan flow's own progress. Silent until a scan is armed, so an
            // NFC-less device never sees a permanent explanation of a feature
            // it does not have.
            if (ui.nfcScan.phase != NfcScanPhase.Unavailable && ui.nfcScan.phase != NfcScanPhase.Idle) {
                InlineNotice(
                    ui.nfcScan.message.ifBlank { "Hold the tag against the phone." },
                    icon = Icons.Default.Contactless,
                    tone = if (ui.nfcScan.phase == NfcScanPhase.Rejected) ChipTone.Error else ChipTone.Positive,
                )
                Spacer(Modifier.height(LayoutTokens.Space2))
            }

            // The bulk bar, present only while a selection exists. That is what
            // makes selection mode unmistakable instead of something the user
            // has to infer from tick marks.
            AmiiboSelectionBar(ui, viewModel, cards)

            Box(Modifier.weight(1f).fillMaxWidth()) {
                // THE BROWSER OWNS THE CANVAS UNTIL SOMEONE ASKS TO INSPECT.
                // The pane used to be permanent at this width, reserving space
                // to describe whatever happened to be highlighted; now width
                // decides only WHERE details appear, never whether they do.
                if (twoColumn && paneOpen) {
                    Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
                        // No sheet here: the pane beside the grid is already
                        // showing this figure, and stacking a modal copy of it
                        // over the top would hide the browser it sits next to.
                        AmiiboBrowser(ui, viewModel, cards, Modifier.weight(1f).fillMaxHeight())
                        Column(
                            Modifier.width(LayoutTokens.DetailWidth).fillMaxHeight()
                                .verticalScroll(rememberScrollState()),
                            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
                        ) {
                            if (adapterOnly) AdapterAmiiboCard(ui, viewModel)
                            AmiiboDetailPanel(
                                inspected, ui, viewModel, onImportKeys,
                                onClose = viewModel::closeAmiiboDetails,
                            )
                        }
                    }
                } else {
                    Column(Modifier.fillMaxSize()) {
                        if (adapterOnly) {
                            AdapterAmiiboCard(ui, viewModel)
                            Spacer(Modifier.height(LayoutTokens.Space2))
                        }
                        AmiiboBrowser(ui, viewModel, cards, Modifier.weight(1f).fillMaxWidth())
                    }
                }
            }
        }
    }

    // Compact layouts get the details as a sheet so the browser stays the page.
    if (!twoColumnDetail && inspected != null) {
        ModalBottomSheet(onDismissRequest = viewModel::closeAmiiboDetails) {
            Column(
                Modifier.fillMaxWidth().padding(horizontal = LayoutTokens.Space4)
                    .padding(bottom = LayoutTokens.Space5)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
            ) {
                // No close button: a sheet is dismissed by swiping it away or by
                // Back, both of which are native and both of which are wired.
                AmiiboDetailPanel(inspected, ui, viewModel, onImportKeys, framed = false)
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

/**
 * Title, search, and the library actions.
 *
 * Search expands in place instead of occupying a permanent full-width field:
 * the field was the tallest element above the grid and was empty almost all of
 * the time.
 *
 * The action set collapses by measured width. Six icon buttons plus a title do
 * not fit a 411 dp phone, and the failure mode was not a clipped icon -- the
 * title column was squeezed until "Amiibo" wrapped one letter per line. Below
 * the threshold, filter, sort and scan move into the overflow menu that already
 * had to exist for Amiibo settings.
 */
@Composable
private fun AmiiboToolbar(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    count: Int,
    matches: Int,
    filters: AmiiboGalleryFilters,
    options: AmiiboGalleryOptions,
    searchOpen: Boolean,
    onSearchOpen: (Boolean) -> Unit,
    onImport: () -> Unit,
    onScan: () -> Unit,
) {
    val query = filters.search
    val onQuery: (String) -> Unit = viewModel::setAmiiboSearch
    var sortOpen by remember { mutableStateOf(false) }
    var filterOpen by remember { mutableStateOf(false) }
    var viewOpen by remember { mutableStateOf(false) }
    var overflowOpen by remember { mutableStateOf(false) }
    val focusRequester = remember { FocusRequester() }
    // An Android build with no usable NFC reader hides the action entirely
    // rather than carrying a permanent explanation of an absent capability.
    val nfcAvailable = ui.nfcScan.phase != NfcScanPhase.Unavailable

    LaunchedEffect(searchOpen) { if (searchOpen) runCatching { focusRequester.requestFocus() } }

    BoxWithConstraints(Modifier.fillMaxWidth()) {
        val roomy = maxWidth >= LayoutTokens.AmiiboToolbarWideWidth
        Column {
            Row(
                Modifier.fillMaxWidth().heightIn(min = LayoutTokens.TouchHeight),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space1),
            ) {
                if (searchOpen) {
                    OutlinedTextField(
                        value = query,
                        onValueChange = onQuery,
                        modifier = Modifier.weight(1f).focusRequester(focusRequester),
                        singleLine = true,
                        placeholder = { Text("Find an Amiibo") },
                        leadingIcon = { Icon(Icons.Default.Search, null) },
                        trailingIcon = {
                            IconButton(onClick = { onSearchOpen(false) }) {
                                Icon(Icons.Default.Close, "Close search")
                            }
                        },
                    )
                } else {
                    Column(Modifier.weight(1f)) {
                        Text(
                            AppSection.Amiibo.title,
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Text(
                            if (!filters.any) "$count saved" else "$matches of $count",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    IconButton(onClick = { onSearchOpen(true) }) {
                        Icon(Icons.Default.Search, "Search library")
                    }
                    // View mode sits with search rather than in the overflow:
                    // it is the control someone reaches for most after finding
                    // nothing looks the way they want.
                    Box {
                        IconButton(onClick = { viewOpen = true }) {
                            Icon(filters.view.icon(), "Change how the library is shown")
                        }
                        ViewMenu(viewOpen, { viewOpen = false }, filters.view, viewModel::setAmiiboView)
                    }
                    if (roomy) {
                        Box {
                            IconButton(
                                onClick = { filterOpen = true },
                                enabled = options.gameSeries.isNotEmpty() ||
                                    options.amiiboSeries.isNotEmpty() || options.types.isNotEmpty(),
                            ) {
                                Icon(
                                    if (filters.any) Icons.Default.FilterAlt else Icons.Default.FilterList,
                                    "Filter the library",
                                )
                            }
                            FilterMenu(filterOpen, { filterOpen = false }, filters, options, viewModel)
                        }
                        Box {
                            IconButton(onClick = { sortOpen = true }) {
                                Icon(Icons.AutoMirrored.Filled.Sort, "Sort library")
                            }
                            SortMenu(sortOpen, { sortOpen = false }, filters, viewModel)
                        }
                        if (nfcAvailable) {
                            IconButton(onClick = onScan, enabled = !ui.busy) {
                                Icon(Icons.Default.Contactless, "Scan a tag with this phone")
                            }
                        }
                    }
                    FilledTonalIconButton(onClick = onImport, enabled = !ui.busy) {
                        Icon(Icons.Default.Add, "Import an Amiibo backup")
                    }
                    Box {
                        IconButton(onClick = { overflowOpen = true }) {
                            Icon(Icons.Default.MoreVert, "More library actions")
                        }
                        DropdownMenu(expanded = overflowOpen, onDismissRequest = { overflowOpen = false }) {
                            if (!roomy) {
                                DropdownMenuItem(
                                    text = { Text("Sort: ${filters.sort.label()}") },
                                    leadingIcon = { Icon(Icons.AutoMirrored.Filled.Sort, null) },
                                    onClick = { overflowOpen = false; sortOpen = true },
                                )
                                DropdownMenuItem(
                                    text = { Text(if (filters.any) "Filters applied" else "Filter the library") },
                                    leadingIcon = { Icon(Icons.Default.FilterList, null) },
                                    enabled = options.gameSeries.isNotEmpty() ||
                                        options.amiiboSeries.isNotEmpty() || options.types.isNotEmpty(),
                                    onClick = { overflowOpen = false; filterOpen = true },
                                )
                                if (nfcAvailable) {
                                    DropdownMenuItem(
                                        text = { Text("Scan a tag") },
                                        leadingIcon = { Icon(Icons.Default.Contactless, null) },
                                        enabled = !ui.busy,
                                        onClick = { overflowOpen = false; onScan() },
                                    )
                                }
                                HorizontalDivider()
                            }
                            DropdownMenuItem(
                                text = { Text("Amiibo settings") },
                                leadingIcon = { Icon(Icons.Default.Settings, null) },
                                onClick = { overflowOpen = false; viewModel.openOverlay(AppOverlay.AmiiboSettings) },
                            )
                        }
                        // Anchored here so the collapsed menus open near the
                        // overflow button they were reached through.
                        if (!roomy) {
                            SortMenu(sortOpen, { sortOpen = false }, filters, viewModel)
                            FilterMenu(filterOpen, { filterOpen = false }, filters, options, viewModel)
                        }
                    }
                }
            }
            // Active filters as removable chips. A filter you cannot see is a
            // library that looks mysteriously incomplete.
            if (filters.any && !searchOpen) {
                Spacer(Modifier.height(LayoutTokens.Space1))
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space1)) {
                    listOfNotNull(
                        filters.gameSeries.takeIf { it.isNotBlank() }
                            ?.let { it to { viewModel.setAmiiboGameSeries("") } },
                        filters.amiiboSeries.takeIf { it.isNotBlank() }
                            ?.let { it to { viewModel.setAmiiboSeries("") } },
                        filters.type.takeIf { it.isNotBlank() }
                            ?.let { it to { viewModel.setAmiiboType("") } },
                    ).forEach { (label, clear) ->
                        FilterChip(
                            selected = true,
                            onClick = clear,
                            label = { Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            trailingIcon = {
                                Icon(Icons.Default.Close, "Clear filter", Modifier.size(16.dp))
                            },
                        )
                    }
                }
            }
        }
    }
}

/**
 * Sort mode, and the direction alongside it.
 *
 * Direction belongs with the selector rather than as a separate control: it is
 * meaningless on its own, and a stray "reverse" button with no visible sort is
 * a puzzle.
 */
@Composable
private fun SortMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    filters: AmiiboGalleryFilters,
    viewModel: CompanionViewModel,
) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        AmiiboSort.entries.forEach { sort ->
            DropdownMenuItem(
                text = { Text(sort.label()) },
                onClick = { viewModel.setAmiiboSort(sort); onDismiss() },
                leadingIcon = if (sort == filters.sort) ({ Icon(Icons.Default.Check, null) }) else null,
            )
        }
        HorizontalDivider()
        DropdownMenuItem(
            text = { Text(if (filters.descending) "Descending" else "Ascending") },
            leadingIcon = { Icon(Icons.AutoMirrored.Filled.Sort, null) },
            onClick = { viewModel.toggleAmiiboSortDirection(); onDismiss() },
        )
    }
}

/**
 * The three filters, each offering only values the user's library contains.
 *
 * A menu listing every Amiibo series in existence when the library holds three
 * figures is a worse control than one listing those three's series.
 */
@Composable
private fun FilterMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    filters: AmiiboGalleryFilters,
    options: AmiiboGalleryOptions,
    viewModel: CompanionViewModel,
) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        FilterGroup("Game series", options.gameSeries, filters.gameSeries) {
            viewModel.setAmiiboGameSeries(it); onDismiss()
        }
        FilterGroup("Collection", options.amiiboSeries, filters.amiiboSeries) {
            viewModel.setAmiiboSeries(it); onDismiss()
        }
        FilterGroup("Type", options.types, filters.type) {
            viewModel.setAmiiboType(it); onDismiss()
        }
        if (filters.any) {
            HorizontalDivider()
            DropdownMenuItem(
                text = { Text("Clear filters") },
                leadingIcon = { Icon(Icons.Default.Close, null) },
                onClick = { viewModel.clearAmiiboFilters(); onDismiss() },
            )
        }
    }
}

@Composable
private fun FilterGroup(
    title: String,
    values: List<String>,
    selected: String,
    onSelect: (String) -> Unit,
) {
    if (values.isEmpty()) return
    HorizontalDivider()
    Text(
        title,
        style = MaterialTheme.typography.labelSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space1),
    )
    values.forEach { value ->
        DropdownMenuItem(
            text = { Text(value) },
            onClick = { onSelect(if (value == selected) "" else value) },
            leadingIcon = if (value == selected) ({ Icon(Icons.Default.Check, null) }) else null,
        )
    }
}

/** Grid, Carousel or Detailed list. Presentation only. */
@Composable
private fun ViewMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    view: AmiiboViewMode,
    onSelect: (AmiiboViewMode) -> Unit,
) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        AmiiboViewMode.entries.forEach { mode ->
            DropdownMenuItem(
                text = { Text(mode.label()) },
                leadingIcon = { Icon(mode.icon(), null) },
                onClick = { onSelect(mode); onDismiss() },
                trailingIcon = if (mode == view) ({ Icon(Icons.Default.Check, null) }) else null,
            )
        }
    }
}

private fun AmiiboSort.label(): String = when (this) {
    AmiiboSort.Default -> "Recently added"
    AmiiboSort.Name -> "Name"
    AmiiboSort.Number -> "Figure number"
    AmiiboSort.Release -> "Release date"
}

private fun AmiiboViewMode.label(): String = when (this) {
    AmiiboViewMode.Grid -> "Grid"
    AmiiboViewMode.Carousel -> "Carousel"
    AmiiboViewMode.List -> "Detailed list"
}

private fun AmiiboViewMode.icon() = when (this) {
    AmiiboViewMode.Grid -> Icons.Default.GridView
    AmiiboViewMode.Carousel -> Icons.Default.ViewCarousel
    AmiiboViewMode.List -> Icons.AutoMirrored.Filled.List
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

/**
 * The library grid.
 *
 * Adaptive cells with a deliberate minimum: a fixed column count produces
 * postage-stamp artwork on a wide handheld and one giant card per row on a
 * narrow one. Each cell is artwork, then name, then one subordinate line --
 * nothing else competes with the figure.
 */
/**
 * The library browser, in whichever view the user chose.
 *
 * THREE VIEWS, ONE CARD LIST. All of them are handed the same projected
 * [cards] and differ only in how they lay them out, so switching view neither
 * re-runs the query nor loses the selection. Every one is lazy: a 1000-entry
 * library composes only what is on screen.
 *
 * Tapping selects. Tapping an already-selected item opens the details, so
 * browsing never costs two taps on a phone.
 */
@Composable
private fun AmiiboBrowser(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    cards: List<AmiiboCard>,
    modifier: Modifier,
) {
    if (cards.isEmpty()) {
        EmptyStateBlock(
            icon = Icons.Default.Contactless,
            title = if (ui.library.isEmpty()) "No Amiibo yet" else "Nothing matches",
            body = if (ui.library.isEmpty()) {
                "Import a 540, 572, or 2048-byte backup, a folder of them, or a library ZIP."
            } else {
                "Nothing matches the current search and filters."
            },
            modifier = modifier,
        )
        return
    }

    // GESTURES TRANSLATE TO DOMAIN ACTIONS AND DO NOTHING ELSE. Single = browse,
    // double = inspect, long press = select. The rules those three invoke are
    // shared with the Windows companion and tested without composing anything.
    val gestures = AmiiboGestures(
        onTap = { card -> viewModel.activateAmiibo(card.id) },
        // Refused by the domain while selecting, so a double tap during a bulk
        // selection toggles once and opens nothing.
        onDoubleTap = { card -> viewModel.openAmiiboDetails(card.id) },
        onLongPress = { card -> viewModel.startAmiiboSelection(card.id) },
        // Toggle rather than enter-selection, because the accessible action has
        // to be able to say "Deselect" and mean it. Toggling also starts a
        // selection from nothing, so one action covers both directions.
        onToggle = { card -> viewModel.toggleAmiiboSelection(card.id) },
    )

    when (ui.amiiboFilters.view) {
        AmiiboViewMode.Grid -> AmiiboGridView(cards, ui.amiiboInteraction, modifier, gestures)
        AmiiboViewMode.Carousel -> AmiiboCarouselView(cards, ui.amiiboInteraction, modifier, gestures)
        AmiiboViewMode.List -> AmiiboListView(cards, ui.amiiboInteraction, modifier, gestures)
    }
}

/**
 * What is selected, and the two things that can be done to it.
 *
 * Deliberately two commands. Multi-selection existing is not a reason to invent
 * a dozen batch operations, and a crowded bar makes the destructive pair harder
 * to aim at rather than easier.
 */
@Composable
private fun AmiiboSelectionBar(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    cards: List<AmiiboCard>,
) {
    val interaction = ui.amiiboInteraction
    if (!interaction.selecting) return

    var initializeOpen by rememberSaveable { mutableStateOf(false) }
    var deleteOpen by rememberSaveable { mutableStateOf(false) }

    val count = interaction.selectedCount
    // Selection deliberately survives a filter change, so the set can hold
    // entries the current query hides. Saying so here — not only in the
    // confirmation — is what stops somebody destroying more than they can see.
    val hidden = remember(interaction.selection, cards) {
        interaction.hiddenSelectedCount(cards.map { it.id })
    }

    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = MaterialTheme.shapes.small,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        ) {
            IconButton(onClick = viewModel::clearAmiiboSelection) {
                Icon(Icons.Default.Close, "Exit selection mode")
            }
            Column(Modifier.weight(1f)) {
                Text(
                    "$count selected",
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                )
                if (hidden > 0) {
                    Text(
                        "$hidden hidden by filters",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            TextButton(
                onClick = { initializeOpen = true },
                enabled = !ui.busy,
            ) { Text("Initialize") }
            TextButton(
                onClick = { deleteOpen = true },
                enabled = !ui.busy,
                colors = ButtonDefaults.textButtonColors(
                    contentColor = MaterialTheme.colorScheme.error,
                ),
            ) { Text("Delete") }
        }
    }
    Spacer(Modifier.height(LayoutTokens.Space2))

    val hiddenNote = if (hidden == 0) "" else
        "$hidden of these are hidden by the current search or filters. "

    // ONE confirmation, not one per entry: twelve dialogs is a prompt users
    // learn to dismiss without reading, which is worse than one they stop at.
    if (initializeOpen) ConfirmDialog(
        onDismiss = { initializeOpen = false },
        title = "Initialize $count Amiibo?",
        body = hiddenNote +
            "This erases the owner, nickname, registration and game data from the selected " +
            "backups on this phone and re-signs them. The figures themselves are unchanged, " +
            "and so is the adapter. This cannot be undone.",
        confirmLabel = "Initialize $count",
        destructive = true,
        onConfirm = { initializeOpen = false; viewModel.initializeSelectedAmiibos() },
    )

    if (deleteOpen) ConfirmDialog(
        onDismiss = { deleteOpen = false },
        title = "Delete $count Amiibo from your library?",
        body = hiddenNote +
            "This removes these backups from this phone only. It cannot be undone, and the " +
            "Amiibo currently on the adapter is not affected.",
        confirmLabel = "Delete $count",
        destructive = true,
        onConfirm = { deleteOpen = false; viewModel.deleteSelectedAmiibos() },
    )
}

/**
 * The three gestures every view raises, whatever it looks like.
 *
 * Bundled so adding a fourth does not mean changing the signature of every view
 * and every call site, and so all three views demonstrably raise the same set.
 */
private data class AmiiboGestures(
    val onTap: (AmiiboCard) -> Unit,
    val onDoubleTap: (AmiiboCard) -> Unit,
    val onLongPress: (AmiiboCard) -> Unit,
    val onToggle: (AmiiboCard) -> Unit,
)

/**
 * Membership of the bulk set, drawn on the item.
 *
 * Present on EVERY item while selecting, not only the ticked ones: an empty
 * circle is what tells a user that tapping now toggles rather than browses. It
 * disappears entirely in normal mode, where it would be a control with nothing
 * to control.
 *
 * Carries no semantics of its own — the row already announces its selected
 * state — and no click handler, because the whole item is the target and a small
 * second target is only something to miss.
 */
@Composable
private fun AmiiboSelectionTick(
    interaction: AmiiboInteractionState,
    card: AmiiboCard,
    modifier: Modifier = Modifier,
) {
    if (!interaction.selecting) return

    val ticked = interaction.isSelected(card.id)
    Box(
        modifier
            .padding(LayoutTokens.Space1)
            .size(22.dp)
            .clip(CircleShape)
            .background(
                if (ticked) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.surface.copy(alpha = 0.85f),
            )
            .border(
                1.dp,
                if (ticked) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.onSurfaceVariant,
                CircleShape,
            )
            .clearAndSetSemantics { },
        contentAlignment = Alignment.Center,
    ) {
        if (ticked) {
            Icon(
                Icons.Default.Check,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onPrimary,
                modifier = Modifier.size(14.dp),
            )
        }
    }
}

/**
 * Tap, double tap and long press, plus the accessible equivalents.
 *
 * LONG PRESS AND DOUBLE TAP MUST NOT BE THE ONLY ROUTES. Neither exists for a
 * screen reader, a keyboard or a switch device, so both transitions are also
 * exposed as named custom actions that assistive technology can invoke directly.
 */
@Composable
private fun Modifier.amiiboGestures(
    card: AmiiboCard,
    interaction: AmiiboInteractionState,
    gestures: AmiiboGestures,
): Modifier {
    val ticked = interaction.isSelected(card.id)
    return this
        .combinedClickable(
            onClick = { gestures.onTap(card) },
            onDoubleClick = { gestures.onDoubleTap(card) },
            onLongClick = { gestures.onLongPress(card) },
        )
        .semantics {
            // Announces "selected" for the bulk set rather than for the
            // highlight: membership is what a destructive command acts on, and
            // it is the fact a user cannot otherwise discover.
            if (interaction.selecting) selected = ticked
            customActions = listOf(
                CustomAccessibilityAction(
                    if (interaction.selecting && ticked) "Deselect" else "Select",
                ) { gestures.onToggle(card); true },
                CustomAccessibilityAction("Open details") {
                    gestures.onDoubleTap(card); true
                },
            )
        }
}

/** Adaptive card grid: artwork first, for recognising a figure at a glance. */
@Composable
private fun AmiiboGridView(
    cards: List<AmiiboCard>,
    interaction: AmiiboInteractionState,
    modifier: Modifier,
    gestures: AmiiboGestures,
) {
    LazyVerticalGrid(
        columns = GridCells.Adaptive(LayoutTokens.AmiiboCellMinWidth),
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        contentPadding = PaddingValues(bottom = LayoutTokens.Space5),
    ) {
        items(cards, key = { it.id }) { card ->
            AmiiboCardTile(card, interaction, Modifier.fillMaxWidth(), gestures = gestures)
        }
    }
}

/**
 * Large-art, low-density browsing.
 *
 * A horizontal lazy row rather than a pager: a pager loads its neighbours
 * eagerly, which is the wrong trade at a thousand items, and swiping a row still
 * reads as a carousel. Only what is on screen is composed, so the artwork memory
 * cost stays bounded.
 */
@Composable
private fun AmiiboCarouselView(
    cards: List<AmiiboCard>,
    interaction: AmiiboInteractionState,
    modifier: Modifier,
    gestures: AmiiboGestures,
) {
    val state = rememberLazyListState()
    val focusedId = interaction.focusedId

    // Follow the FOCUS when it changes from elsewhere — switching into this
    // view, or a sync focusing the synced tag — so the carousel is never showing
    // one Amiibo while the details surface describes another.
    //
    // Deliberately not keyed on the bulk set: ticking items while browsing must
    // not drag the carousel back to whichever one was focused first.
    LaunchedEffect(focusedId, cards) {
        val index = cards.indexOfFirst { it.id == focusedId }
        if (index >= 0) state.animateScrollToItem(index)
    }

    LazyRow(
        state = state,
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
        contentPadding = PaddingValues(horizontal = LayoutTokens.Space3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        items(cards, key = { it.id }) { card ->
            AmiiboCardTile(
                card,
                interaction,
                // A fraction of the viewport, not a fixed width: the carousel
                // shows ONE figure at a time with the next one peeking, which
                // is the only thing it does that the grid does not. A fixed
                // 220dp card left most of a phone page empty and most of a
                // tablet pane empty too.
                Modifier
                    .fillParentMaxWidth(if (cards.size > 1) 0.78f else 1f)
                    .widthIn(max = LayoutTokens.AmiiboCarouselMaxWidth)
                    .fillMaxHeight(),
                large = true,
                gestures = gestures,
            )
        }
    }
}

/**
 * The dense scanning mode.
 *
 * A compact row with a thumbnail and the fields worth sorting by. Deliberately
 * not a desktop multi-column table: on a phone the useful columns are the name,
 * what it is, and its state.
 */
@Composable
private fun AmiiboListView(
    cards: List<AmiiboCard>,
    interaction: AmiiboInteractionState,
    modifier: Modifier,
    gestures: AmiiboGestures,
) {
    LazyColumn(
        modifier = modifier,
        contentPadding = PaddingValues(bottom = LayoutTokens.Space5),
    ) {
        items(cards, key = { it.id }) { card ->
            val focused = card.id == interaction.focusedId
            Row(
                Modifier.fillMaxWidth()
                    .background(
                        if (focused) MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.surface,
                    )
                    .amiiboGestures(card, interaction, gestures)
                    .padding(horizontal = LayoutTokens.Space2, vertical = LayoutTokens.Space2),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
            ) {
                AmiiboSelectionTick(interaction, card)
                AmiiboArtwork(card.imageUrl, card.title, Modifier.size(36.dp))
                Column(Modifier.weight(1f)) {
                    Text(
                        card.title,
                        style = MaterialTheme.typography.bodyMedium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        card.subtitle,
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                if (card.badge.isNotEmpty()) {
                    StatusChip(card.badge, tone = ChipTone.Positive)
                }
            }
            HorizontalDivider()
        }
    }
}

/**
 * One card, in grid or carousel size.
 *
 * Selection is a filled container plus a border rather than a hairline outline:
 * on a wall of artwork a barely-visible border is not a state anyone can see.
 */
@Composable
private fun AmiiboCardTile(
    card: AmiiboCard,
    interaction: AmiiboInteractionState,
    modifier: Modifier,
    large: Boolean = false,
    gestures: AmiiboGestures,
) {
    // FOCUS FILLS THE CARD; MEMBERSHIP IS A MARK ON IT. Two different ideas, so
    // two different presentations — a user has to be able to tell "where I am"
    // from "what I have ticked" without counting.
    val focused = card.id == interaction.focusedId

    Card(
        modifier.amiiboGestures(card, interaction, gestures),
        colors = CardDefaults.cardColors(
            containerColor = if (focused) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant,
        ),
        border = if (focused) CardDefaults.outlinedCardBorder() else null,
    ) {
        Column(
            Modifier.padding(LayoutTokens.Space2).fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // In the carousel the artwork takes whatever height the row was
            // given: the whole reason to leave the grid is to see the figure
            // larger, and a fixed art height left a phone-sized page mostly
            // empty around one small card. In the grid it stays fixed, because
            // there every tile must agree on a row height.
            Box(if (large) Modifier.fillMaxWidth().weight(1f) else Modifier.fillMaxWidth()) {
                AmiiboArtwork(
                    card.imageUrl,
                    card.title,
                    if (large) Modifier.fillMaxSize()
                    else Modifier.fillMaxWidth().height(LayoutTokens.AmiiboArtHeight),
                )
                if (card.badge.isNotEmpty()) {
                    StatusChip(
                        card.badge,
                        tone = ChipTone.Positive,
                        modifier = Modifier.align(Alignment.TopEnd),
                    )
                }
                // Opposite corner from the adapter badge: two marks that can
                // both be present must never land on top of each other.
                AmiiboSelectionTick(interaction, card, Modifier.align(Alignment.TopStart))
            }
            Spacer(Modifier.height(LayoutTokens.Space2))
            Text(
                card.title,
                style = if (large) MaterialTheme.typography.titleMedium
                else MaterialTheme.typography.titleSmall,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
            Text(
                card.subtitle,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
        }
    }
}

// ---------------------------------------------------------------------------
// Detail
// ---------------------------------------------------------------------------

@Composable
private fun AmiiboDetailPanel(
    item: AmiiboLibraryItem?,
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onImportKeys: () -> Unit,
    framed: Boolean = true,
    /**
     * Supplied by the two-pane layout only. A sheet is dismissed by swiping or
     * by Back, both native and both wired; a pane beside the browser has neither
     * and needs a visible way out, or it is pinned for the rest of the session.
     */
    onClose: (() -> Unit)? = null,
) {
    if (item == null) {
        if (framed) {
            SectionCard {
                EmptyStateBlock(
                    Icons.Default.TouchApp,
                    "Select an Amiibo",
                    "Choose a figure to inspect it or load it onto the adapter.",
                    Modifier.fillMaxWidth().heightIn(min = 200.dp),
                )
            }
        }
        return
    }

    var renameOpen by rememberSaveable(item.id) { mutableStateOf(false) }
    var deleteOpen by rememberSaveable(item.id) { mutableStateOf(false) }
    var initializeOpen by rememberSaveable(item.id) { mutableStateOf(false) }
    var name by rememberSaveable(item.id) { mutableStateOf(item.displayName) }
    var menuOpen by remember { mutableStateOf(false) }

    // Survives selection changes on purpose: someone comparing the UIDs of two
    // dumps should not be dropped back to Overview between them.
    var category by rememberSaveable { mutableStateOf(AmiiboCategory.Overview) }

    val catalog = ui.selectedAmiiboCatalog
    val details = ui.selectedAmiiboDetails
    val amiibo = ui.snapshot.amiibo
    val adapterHasAmiibo = amiibo.loaded || amiibo.v3Loaded
    val online = ui.connection.connected && !ui.busy

    // The adapter holds THIS figure, not merely some figure. Every adapter
    // command below acts on whatever is resident, so the distinction decides
    // whether they are the user's Amiibo's commands or someone else's.
    val adapterMatches = AmiiboGallery.residentOn(item, amiibo.uid, adapterHasAmiibo)

    // A key is held but this dump has not been decoded yet.
    val decodePending = details == null && ui.amiiboKeysLoaded

    val body: @Composable ColumnScope.() -> Unit = {
        Row(verticalAlignment = Alignment.CenterVertically) {
            AmiiboArtwork(
                catalog?.imageUrl.orEmpty(),
                catalogTitle(catalog, item.displayName),
                Modifier.size(72.dp),
            )
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f)) {
                Text(
                    catalogTitle(catalog, item.displayName),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    catalogSubtitle(catalog).ifBlank { "Figure ${item.figureId}" },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            onClose?.let {
                IconButton(onClick = it) {
                    Icon(Icons.Default.Close, "Close details")
                }
            }
            Box {
                IconButton(onClick = { menuOpen = true }) {
                    Icon(Icons.Default.MoreVert, "More actions for ${item.displayName}")
                }
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                    DropdownMenuItem(
                        text = { Text("Rename") },
                        leadingIcon = { Icon(Icons.Default.Edit, null) },
                        onClick = { menuOpen = false; renameOpen = true },
                    )
                    DropdownMenuItem(
                        text = { Text(if (ui.amiiboKeysLoaded) "Initialize locally" else "Import key to initialize") },
                        leadingIcon = { Icon(Icons.Default.RestartAlt, null) },
                        onClick = {
                            menuOpen = false
                            if (ui.amiiboKeysLoaded) initializeOpen = true else onImportKeys()
                        },
                    )
                    HorizontalDivider()
                    DropdownMenuItem(
                        text = { Text("Delete from phone") },
                        leadingIcon = { Icon(Icons.Default.DeleteOutline, null) },
                        onClick = { menuOpen = false; deleteOpen = true },
                    )
                }
            }
        }

        // Adapter state, right under the identity, so the hero answers "what is
        // this and where is it" in one glance.
        Text(
            when {
                adapterMatches && amiibo.dirty -> "On the adapter · changed by the console"
                adapterMatches -> "On the adapter"
                else -> "Not on the adapter"
            },
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        if (amiibo.dirty) {
            InlineNotice(
                "The console changed the Amiibo on the adapter. Sync before replacing or clearing it.",
                icon = Icons.Default.Warning,
                tone = ChipTone.Error,
            )
        }

        // PRIMARY ACTION. Visible and disabled with its reason when there is no
        // adapter, because this is the control the user is reaching for.
        Button(
            onClick = viewModel::loadSelectedAmiibo,
            enabled = online && !amiibo.dirty,
            modifier = Modifier.fillMaxWidth(),
        ) { Text("Send to adapter") }
        if (!online || amiibo.dirty) {
            Text(
                if (amiibo.dirty) "Sync the adapter's changed Amiibo first."
                else "Connect the adapter to send this Amiibo.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        // CATEGORIES, not one giant scroll.
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
            AmiiboCategory.entries.forEachIndexed { index, entry ->
                SegmentedButton(
                    selected = category == entry,
                    onClick = { category = entry },
                    shape = SegmentedButtonDefaults.itemShape(index, AmiiboCategory.entries.size),
                ) { Text(entry.name) }
            }
        }

        val groups = remember(item.id, catalog, details, adapterMatches, amiibo.dirty) {
            AmiiboInspection.build(item, catalog, details, adapterMatches, amiibo.dirty && adapterMatches)
        }
        val shown = AmiiboInspection.forCategory(category, groups)

        shown.forEach { group ->
            // The heading earns its space only when the category holds more than
            // one group; over a lone list it just repeats the tab above it.
            if (shown.size > 1) {
                Text(
                    group.title,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            group.rows.forEach { row ->
                LabelValueRow(
                    row.label,
                    // The shared model describes a FIGURE, not what the app is
                    // currently doing to it, so it reads an absent decode as
                    // "no key imported". While a key is in fact held the decode
                    // is merely still running, and telling the user to import
                    // the key they already imported is the one answer that is
                    // certainly wrong. Presentation-time state, so it stays here
                    // rather than diverging the model from Windows.
                    if (decodePending && row.label == "Contents") "Reading…" else row.value,
                    monospace = row.monospace,
                    // Identifiers are copied to compare against a dump or a bug
                    // report; prose is not.
                    copyable = row.monospace,
                )
            }
        }

        if (category == AmiiboCategory.Overview && catalog == null) {
            Text(
                if (ui.amiiboCatalogLoading) "Looking this figure up in the catalog…"
                else "Catalog unavailable; the identity stored on this phone is authoritative.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        // The adapter category's commands, exposed only when they can be used.
        // A wall of five disabled buttons teaches nobody that a capability
        // exists; the concise state above already said the adapter is away.
        //
        // ADAPTERMATCHES, NOT ADAPTERHASAMIIBO. Present, Eject and Sync all act
        // on whatever the adapter is holding, which need not be this figure.
        // Offering them under a detail pane headed "Link" while the adapter
        // holds Zelda invites the user to eject a figure they are not looking
        // at, or to overwrite Link's backup with Zelda's bytes. The card above
        // the browser owns the resident figure's controls.
        if (category == AmiiboCategory.Adapter && online && adapterMatches) {
            Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                FilledTonalButton(
                    onClick = { viewModel.setPresented(!amiibo.presented) },
                    modifier = Modifier.weight(1f),
                ) { Text(if (amiibo.presented) "Eject" else "Present") }
                OutlinedButton(
                    onClick = viewModel::syncSelectedAmiibo,
                    modifier = Modifier.weight(1f),
                ) { Text("Sync") }
            }
            if (amiibo.hasSave2) {
                OutlinedButton(
                    onClick = { viewModel.selectCopy(!amiibo.usingSave2) },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(if (amiibo.usingSave2) "Use clean copy" else "Use console copy") }
            }
        }

        // One sentence in place of the commands, saying which of the two reasons
        // applies. Both are things the user can act on.
        if (category == AmiiboCategory.Adapter && !adapterMatches) {
            Text(
                if (!online) "Adapter unavailable. Connect the adapter to send this Amiibo."
                else "Send this Amiibo to the adapter to present, eject or sync it.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }

    if (framed) SectionCard(content = body) else Column(
        Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
        content = body,
    )

    if (renameOpen) PicoDialog(
        onDismiss = { renameOpen = false },
        title = "Rename backup",
        confirmLabel = "Rename",
        confirmEnabled = name.isNotBlank(),
        onConfirm = { viewModel.renameSelectedAmiibo(name.trim()); renameOpen = false },
    ) {
        OutlinedTextField(
            value = name,
            onValueChange = { name = it.take(120) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            label = { Text("Name") },
            supportingText = { Text("Only the copy on this phone is renamed.") },
        )
    }

    if (deleteOpen) ConfirmDialog(
        onDismiss = { deleteOpen = false },
        title = "Delete backup?",
        body = "This removes ${item.displayName} from this phone only. It cannot be undone and does not clear the adapter.",
        confirmLabel = "Delete",
        destructive = true,
        onConfirm = { viewModel.deleteSelectedAmiibo(); deleteOpen = false },
    )

    if (initializeOpen) ConfirmDialog(
        onDismiss = { initializeOpen = false },
        title = "Initialize this Amiibo?",
        body = "This wipes the owner, nickname, registration and game data in the private phone copy, then re-signs it with your imported key_retail.bin. The UID and identity stay the same, the adapter is not changed, and this cannot be undone.",
        confirmLabel = "Initialize",
        destructive = true,
        onConfirm = { initializeOpen = false; viewModel.initializeSelectedAmiibo() },
    )

}

/**
 * The Amiibo currently on the adapter when it is not one of the phone's own.
 *
 * Deliberately distinct from the library detail: this data exists only on the
 * adapter, so its primary action is to bring a copy back to the phone.
 */
@Composable
private fun AdapterAmiiboCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var clearOpen by rememberSaveable { mutableStateOf(false) }
    val status = ui.snapshot.amiibo
    val catalog = ui.adapterAmiiboCatalog
    val pro2 = ui.snapshot.personality.current == Personality.Pro2
    val enabled = ui.connection.connected && !ui.busy &&
        ui.snapshot.capabilities.amiibo != CapabilityState.Unsupported && pro2

    SectionCard(container = MaterialTheme.colorScheme.tertiaryContainer) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            AmiiboArtwork(
                catalog?.imageUrl.orEmpty(),
                catalogTitle(catalog, "Amiibo on adapter"),
                Modifier.size(56.dp),
            )
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f)) {
                Text("On the adapter", style = MaterialTheme.typography.labelMedium)
                Text(
                    catalogTitle(catalog, "Unknown figure"),
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    "No copy on this phone",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            StatusChip(if (status.presented) "Presented" else "Loaded", tone = ChipTone.Positive)
        }
        if (!pro2) {
            InlineNotice(
                "Switch the adapter to Pro Controller 2 mode to manage virtual Amiibo.",
                tone = ChipTone.Error,
            )
        }
        // Weighted rather than equal: "Save to phone" is both the longer label
        // and the primary action, and splitting the row evenly wrapped it onto
        // two lines inside the 360 dp detail pane.
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Button(
                onClick = viewModel::syncSelectedAmiibo,
                enabled = enabled,
                modifier = Modifier.weight(1.5f),
            ) { Text("Save to phone", maxLines = 1, overflow = TextOverflow.Ellipsis) }
            OutlinedButton(
                onClick = { viewModel.setPresented(!status.presented) },
                enabled = enabled,
                modifier = Modifier.weight(1f),
            ) { Text(if (status.presented) "Eject" else "Present", maxLines = 1) }
            IconButton(onClick = { clearOpen = true }, enabled = enabled && !status.dirty) {
                Icon(Icons.Default.DeleteOutline, "Clear the adapter's Amiibo")
            }
        }
        if (status.dirty) {
            InlineNotice(
                "Console-written data is not saved. Save it to the phone before clearing.",
                icon = Icons.Default.Warning,
                tone = ChipTone.Error,
            )
        }
    }

    if (clearOpen) ConfirmDialog(
        onDismiss = { clearOpen = false },
        title = "Clear adapter Amiibo?",
        body = "This Amiibo has no backup on this phone. Save it first if you may need it later.",
        confirmLabel = "Clear adapter",
        destructive = true,
        confirmEnabled = enabled && !status.dirty,
        onConfirm = { viewModel.clearAdapterAmiibo(); clearOpen = false },
    )
}

// ---------------------------------------------------------------------------
// Amiibo settings
// ---------------------------------------------------------------------------

/**
 * Library maintenance, moved off the browsing surface.
 *
 * Import, export, and key management are each used a handful of times in the
 * life of a library and were consuming permanent width above the grid.
 */
@Composable
fun AmiiboSettingsScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onImportFiles: () -> Unit,
    onImportFolder: () -> Unit,
    onExportArchive: () -> Unit,
    onImportKeys: () -> Unit,
) {
    var forgetKeysOpen by rememberSaveable { mutableStateOf(false) }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4),
    ) {
        ScreenHeader("Amiibo settings", subtitle = "") {
            IconButton(onClick = viewModel::closeOverlay) { Icon(Icons.Default.Close, "Close Amiibo settings") }
        }

        SectionCard(title = "Library", icon = Icons.Default.Inventory2) {
            LabelValueRow("Saved figures", ui.library.size.toString())
            LabelValueRow(
                "Adapter",
                if (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded) "One Amiibo loaded" else "Empty",
            )
            ui.libraryWarnings.forEach { InlineNotice(it, icon = Icons.Default.Warning, tone = ChipTone.Error) }
        }

        SectionCard(title = "Import and export", icon = Icons.Default.SwapVert) {
            // Two rows, not four. One takes any number of dumps and archives at
            // once and works out what each is; the other is the same thing
            // pointed at a folder. Neither asks the user to classify their files
            // first, which is work the app can simply do.
            SettingsRow(
                title = "Import files",
                supporting = "Pick any number of .bin dumps or ZIPs, in any mix",
                leading = Icons.Default.Add,
                enabled = !ui.busy,
                onClick = onImportFiles,
                trailing = { Icon(Icons.Default.ChevronRight, null) },
            )
            SettingsRow(
                title = "Import a folder",
                supporting = "Adds every dump found inside, including subfolders",
                leading = Icons.Default.FolderOpen,
                enabled = !ui.busy,
                onClick = onImportFolder,
                trailing = { Icon(Icons.Default.ChevronRight, null) },
            )
            SettingsRow(
                title = "Export the library",
                supporting = "Writes every saved figure to one ZIP",
                leading = Icons.Default.Archive,
                enabled = !ui.busy && ui.library.isNotEmpty(),
                onClick = onExportArchive,
                trailing = { Icon(Icons.Default.ChevronRight, null) },
            )
        }

        SectionCard(title = "Amiibo metadata", icon = Icons.Default.Key) {
            Text(
                "Your own portal-compatible key_retail.bin unlocks owner, nickname, registration, and game-data fields. It stays private to this app and never reaches the adapter.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            SettingsRow(
                title = if (ui.amiiboKeysLoaded) "Replace key file" else "Import key file",
                supporting = if (ui.amiiboKeysLoaded) "A key is available" else "No key imported",
                leading = Icons.Default.Key,
                enabled = !ui.busy,
                onClick = onImportKeys,
                trailing = { StatusChip(
                        if (ui.amiiboKeysLoaded) "Loaded" else "None",
                        tone = if (ui.amiiboKeysLoaded) ChipTone.Positive else ChipTone.Neutral,
                    ) },
            )
            // Offered only once there is something to forget.
            if (ui.amiiboKeysLoaded) {
                SettingsRow(
                    title = "Forget key file",
                    supporting = "Removes the key from this phone; your backups are kept",
                    leading = Icons.Default.DeleteOutline,
                    enabled = !ui.busy,
                    onClick = { forgetKeysOpen = true },
                )
            }
        }

        Spacer(Modifier.height(LayoutTokens.Space5))
    }

    if (forgetKeysOpen) ConfirmDialog(
        onDismiss = { forgetKeysOpen = false },
        title = "Forget the key file?",
        body = "Owner, nickname, registration and game-data fields become unreadable until you import a key again. Your saved Amiibo backups are not changed or deleted.",
        confirmLabel = "Forget",
        destructive = true,
        onConfirm = { forgetKeysOpen = false; viewModel.forgetAmiiboKeys() },
    )

    // No confirmation any more: importing is ADDITIVE. The old library-ZIP path
    // replaced the phone's library wholesale, which needed a destructive warning;
    // bulk import only ever adds, and anything already held is reported as a
    // duplicate rather than written twice.
}

// ---------------------------------------------------------------------------
// Shared pieces
// ---------------------------------------------------------------------------

internal fun catalogTitle(catalog: AmiiboCatalogEntry?, fallback: String): String =
    catalog?.name?.takeIf(String::isNotBlank) ?: catalog?.character?.takeIf(String::isNotBlank) ?: fallback

internal fun catalogSubtitle(catalog: AmiiboCatalogEntry?): String = listOfNotNull(
    catalog?.gameSeries?.takeIf(String::isNotBlank),
    catalog?.amiiboSeries?.takeIf(String::isNotBlank),
).joinToString(" · ")

/**
 * Best-effort catalog artwork.
 *
 * Optional by design: the local identity is authoritative and every Amiibo
 * operation works with no network at all, so a failed fetch degrades to the
 * placeholder rather than to an error.
 */
@Composable
internal fun AmiiboArtwork(imageUrl: String, contentDescription: String, modifier: Modifier = Modifier) {
    val image = produceState<ImageBitmap?>(null, imageUrl) {
        value = if (imageUrl.isBlank()) null else runCatching {
            withContext(Dispatchers.IO) {
                val connection = URL(imageUrl).openConnection() as HttpURLConnection
                try {
                    connection.connectTimeout = 2_500
                    connection.readTimeout = 8_000
                    connection.instanceFollowRedirects = true
                    if (connection.responseCode !in 200..299) return@withContext null
                    val bytes = connection.inputStream.use { input ->
                        val output = java.io.ByteArrayOutputStream()
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            if (output.size() + count > 2 * 1024 * 1024) return@withContext null
                            output.write(buffer, 0, count)
                        }
                        output.toByteArray()
                    }
                    BitmapFactory.decodeByteArray(bytes, 0, bytes.size)?.asImageBitmap()
                } finally {
                    connection.disconnect()
                }
            }
        }.getOrNull()
    }.value
    if (image != null) {
        Image(
            bitmap = image,
            contentDescription = contentDescription.ifBlank { "Amiibo artwork" },
            modifier = modifier,
            contentScale = ContentScale.Fit,
        )
    } else {
        // Scales with the space it was given. A fixed 36dp mark is right on a
        // grid tile and reads as a broken image when the same composable is
        // handed a full-height carousel card, so the placeholder takes a share
        // of the box and stops before it becomes a billboard.
        BoxWithConstraints(modifier, contentAlignment = Alignment.Center) {
            val size = (minOf(maxWidth, maxHeight) * 0.4f).coerceIn(28.dp, 96.dp)
            Icon(
                Icons.Default.Contactless,
                contentDescription,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(size),
            )
        }
    }
}
