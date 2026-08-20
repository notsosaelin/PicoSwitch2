@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)

package dev.picoswitch.companion.ui

import android.graphics.BitmapFactory
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Sort
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
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
    var query by rememberSaveable { mutableStateOf("") }
    var searchOpen by rememberSaveable { mutableStateOf(false) }
    var sortOrder by rememberSaveable { mutableStateOf(AmiiboSortOrder.Name) }
    var gameSeriesFilter by rememberSaveable { mutableStateOf("") }
    var detailOpen by rememberSaveable { mutableStateOf(false) }

    val adapter = ui.snapshot.amiibo
    val adapterLoaded = adapter.loaded || adapter.v3Loaded
    val selected = ui.library.firstOrNull { it.id == ui.selectedAmiiboId }
    val adapterMatchesSelected = selected != null && adapterLoaded &&
        selected.uid.isNotBlank() && selected.uid.equals(adapter.uid, ignoreCase = true)
    val adapterOnly = adapterLoaded && !adapterMatchesSelected

    val filtered = remember(ui.library, ui.amiiboCatalogEntries, query, sortOrder, gameSeriesFilter) {
        sortAmiiboLibrary(
            ui.library.filter { item ->
                val catalog = ui.amiiboCatalogEntries[item.id]
                val matchesQuery = query.isBlank() || listOf(
                    item.displayName, item.figureId, item.uid, item.typeName, item.characterGameCode,
                    catalog?.name.orEmpty(), catalog?.character.orEmpty(),
                    catalog?.gameSeries.orEmpty(), catalog?.amiiboSeries.orEmpty(),
                ).joinToString(" ").contains(query.trim(), ignoreCase = true)
                matchesQuery &&
                    (gameSeriesFilter.isBlank() || catalog?.gameSeries.equals(gameSeriesFilter, ignoreCase = true))
            },
            ui.amiiboCatalogEntries,
            sortOrder,
        )
    }
    val seriesOptions = remember(ui.amiiboCatalogEntries) {
        ui.amiiboCatalogEntries.values.mapNotNull { it.gameSeries.takeIf(String::isNotBlank) }.distinct().sorted()
    }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)
        Column(Modifier.fillMaxSize()) {
            AmiiboToolbar(
                ui = ui,
                viewModel = viewModel,
                count = ui.library.size,
                matches = filtered.size,
                query = query,
                onQuery = { query = it },
                searchOpen = searchOpen,
                onSearchOpen = { searchOpen = it; if (!it) query = "" },
                sortOrder = sortOrder,
                onSort = { sortOrder = it },
                seriesOptions = seriesOptions,
                seriesFilter = gameSeriesFilter,
                onSeriesFilter = { gameSeriesFilter = it },
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

            Box(Modifier.weight(1f).fillMaxWidth()) {
                if (twoColumn) {
                    Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
                        AmiiboGrid(ui, viewModel, filtered, query, Modifier.weight(1f).fillMaxHeight()) {
                            detailOpen = true
                        }
                        Column(
                            Modifier.width(LayoutTokens.DetailWidth).fillMaxHeight()
                                .verticalScroll(rememberScrollState()),
                            verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
                        ) {
                            if (adapterOnly) AdapterAmiiboCard(ui, viewModel)
                            AmiiboDetailPanel(selected, ui, viewModel, onImportKeys)
                        }
                    }
                } else {
                    Column(Modifier.fillMaxSize()) {
                        if (adapterOnly) {
                            AdapterAmiiboCard(ui, viewModel)
                            Spacer(Modifier.height(LayoutTokens.Space2))
                        }
                        AmiiboGrid(ui, viewModel, filtered, query, Modifier.weight(1f).fillMaxWidth()) {
                            detailOpen = true
                        }
                    }
                }
            }
        }
    }

    // Compact layouts get the detail as a sheet so the grid stays the page.
    if (detailOpen && selected != null) {
        ModalBottomSheet(onDismissRequest = { detailOpen = false }) {
            Column(
                Modifier.fillMaxWidth().padding(horizontal = LayoutTokens.Space4)
                    .padding(bottom = LayoutTokens.Space5)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
            ) {
                AmiiboDetailPanel(selected, ui, viewModel, onImportKeys, framed = false)
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
    query: String,
    onQuery: (String) -> Unit,
    searchOpen: Boolean,
    onSearchOpen: (Boolean) -> Unit,
    sortOrder: AmiiboSortOrder,
    onSort: (AmiiboSortOrder) -> Unit,
    seriesOptions: List<String>,
    seriesFilter: String,
    onSeriesFilter: (String) -> Unit,
    onImport: () -> Unit,
    onScan: () -> Unit,
) {
    var sortOpen by remember { mutableStateOf(false) }
    var filterOpen by remember { mutableStateOf(false) }
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
                            if (query.isBlank() && seriesFilter.isBlank()) "$count saved"
                            else "$matches of $count",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    IconButton(onClick = { onSearchOpen(true) }) {
                        Icon(Icons.Default.Search, "Search library")
                    }
                    if (roomy) {
                        Box {
                            IconButton(onClick = { filterOpen = true }, enabled = seriesOptions.isNotEmpty()) {
                                Icon(
                                    if (seriesFilter.isBlank()) Icons.Default.FilterList else Icons.Default.FilterAlt,
                                    "Filter by series",
                                )
                            }
                            SeriesMenu(filterOpen, { filterOpen = false }, seriesOptions, seriesFilter, onSeriesFilter)
                        }
                        Box {
                            IconButton(onClick = { sortOpen = true }) {
                                Icon(Icons.AutoMirrored.Filled.Sort, "Sort library")
                            }
                            SortMenu(sortOpen, { sortOpen = false }, sortOrder, onSort)
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
                                    text = { Text("Sort: ${sortOrder.label()}") },
                                    leadingIcon = { Icon(Icons.AutoMirrored.Filled.Sort, null) },
                                    onClick = { overflowOpen = false; sortOpen = true },
                                )
                                DropdownMenuItem(
                                    text = { Text(if (seriesFilter.isBlank()) "Filter by series" else seriesFilter) },
                                    leadingIcon = { Icon(Icons.Default.FilterList, null) },
                                    enabled = seriesOptions.isNotEmpty(),
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
                            SortMenu(sortOpen, { sortOpen = false }, sortOrder, onSort)
                            SeriesMenu(filterOpen, { filterOpen = false }, seriesOptions, seriesFilter, onSeriesFilter)
                        }
                    }
                }
            }
            if (seriesFilter.isNotBlank() && !searchOpen) {
                Spacer(Modifier.height(LayoutTokens.Space1))
                FilterChip(
                    selected = true,
                    onClick = { onSeriesFilter("") },
                    label = { Text(seriesFilter, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    trailingIcon = { Icon(Icons.Default.Close, "Clear series filter", Modifier.size(16.dp)) },
                )
            }
        }
    }
}

@Composable
private fun SortMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    sortOrder: AmiiboSortOrder,
    onSort: (AmiiboSortOrder) -> Unit,
) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        // Only orders the local library can actually be sorted by. Import time
        // is recorded per item; "recently used" and "last modified" are not, so
        // they are not offered.
        AmiiboSortOrder.entries.forEach { order ->
            DropdownMenuItem(
                text = { Text(order.label()) },
                onClick = { onSort(order); onDismiss() },
                leadingIcon = if (order == sortOrder) ({ Icon(Icons.Default.Check, null) }) else null,
            )
        }
    }
}

@Composable
private fun SeriesMenu(
    expanded: Boolean,
    onDismiss: () -> Unit,
    options: List<String>,
    selected: String,
    onSelect: (String) -> Unit,
) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        DropdownMenuItem(
            text = { Text("All series") },
            onClick = { onSelect(""); onDismiss() },
            leadingIcon = if (selected.isBlank()) ({ Icon(Icons.Default.Check, null) }) else null,
        )
        options.forEach { series ->
            DropdownMenuItem(
                text = { Text(series) },
                onClick = { onSelect(series); onDismiss() },
                leadingIcon = if (series == selected) ({ Icon(Icons.Default.Check, null) }) else null,
            )
        }
    }
}

private fun AmiiboSortOrder.label(): String = when (this) {
    AmiiboSortOrder.Name -> "Name"
    AmiiboSortOrder.Series -> "Series"
    AmiiboSortOrder.RecentlyAdded -> "Recently imported"
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
@Composable
private fun AmiiboGrid(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    items: List<AmiiboLibraryItem>,
    query: String,
    modifier: Modifier,
    onOpenDetail: () -> Unit,
) {
    if (items.isEmpty()) {
        EmptyStateBlock(
            icon = Icons.Default.Contactless,
            title = if (ui.library.isEmpty()) "No Amiibo yet" else "Nothing matches",
            body = if (ui.library.isEmpty()) {
                "Import a 540, 572, or 2048-byte backup to start your private library."
            } else "No Amiibo matches \"${query.trim()}\" or the current filter.",
            modifier = modifier,
        )
        return
    }
    LazyVerticalGrid(
        columns = GridCells.Adaptive(LayoutTokens.AmiiboCellMinWidth),
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
        contentPadding = PaddingValues(bottom = LayoutTokens.Space5),
    ) {
        items(items, key = { it.id }) { item ->
            val selected = item.id == ui.selectedAmiiboId
            val catalog = ui.amiiboCatalogEntries[item.id]
            Card(
                Modifier.fillMaxWidth().clickable {
                    // Tapping selects, which is the primary action; opening the
                    // detail is the same gesture on an already-selected item so
                    // browsing never costs two taps.
                    if (selected) onOpenDetail() else viewModel.selectAmiibo(item.id)
                },
                colors = CardDefaults.cardColors(
                    containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer
                    else MaterialTheme.colorScheme.surfaceVariant,
                ),
                border = if (selected) CardDefaults.outlinedCardBorder() else null,
            ) {
                Column(
                    Modifier.padding(LayoutTokens.Space2).fillMaxWidth(),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    AmiiboArtwork(
                        catalog?.imageUrl.orEmpty(),
                        catalogTitle(catalog, item.displayName),
                        Modifier.fillMaxWidth().height(LayoutTokens.AmiiboArtHeight),
                    )
                    Spacer(Modifier.height(LayoutTokens.Space2))
                    Text(
                        catalogTitle(catalog, item.displayName),
                        style = MaterialTheme.typography.titleSmall,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                        textAlign = TextAlign.Center,
                    )
                    Text(
                        catalogSubtitle(catalog).ifBlank { item.typeName.ifBlank { "Figure" } },
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        textAlign = TextAlign.Center,
                    )
                }
            }
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
    var detailsOpen by rememberSaveable(item.id) { mutableStateOf(false) }
    var name by rememberSaveable(item.id) { mutableStateOf(item.displayName) }
    var menuOpen by remember { mutableStateOf(false) }

    val catalog = ui.selectedAmiiboCatalog
    val details = ui.selectedAmiiboDetails
    val amiibo = ui.snapshot.amiibo
    val adapterHasAmiibo = amiibo.loaded || amiibo.v3Loaded
    val online = ui.connection.connected && !ui.busy

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
                    DropdownMenuItem(
                        text = { Text("Technical details") },
                        leadingIcon = { Icon(Icons.Default.Info, null) },
                        onClick = { menuOpen = false; detailsOpen = true },
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

        if (details?.crypto == AmiiboCryptoState.Valid) {
            LabelValueRow("Owner", details.owner.ifBlank { "Not set" })
            LabelValueRow("Nickname", details.nickname.ifBlank { "Not set" })
        }

        if (amiibo.dirty) {
            InlineNotice(
                "The console changed the Amiibo on the adapter. Sync before replacing or clearing it.",
                icon = Icons.Default.Warning,
                tone = ChipTone.Error,
            )
        }

        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Button(
                onClick = viewModel::loadSelectedAmiibo,
                enabled = online,
                modifier = Modifier.weight(1f),
            ) { Text("Load") }
            OutlinedButton(
                onClick = viewModel::syncSelectedAmiibo,
                enabled = online && adapterHasAmiibo,
                modifier = Modifier.weight(1f),
            ) { Text("Sync") }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            FilledTonalButton(
                onClick = { viewModel.setPresented(!amiibo.presented) },
                enabled = online && adapterHasAmiibo,
                modifier = Modifier.weight(1f),
            ) { Text(if (amiibo.presented) "Eject" else "Present") }
            if (amiibo.hasSave2) {
                OutlinedButton(
                    onClick = { viewModel.selectCopy(!amiibo.usingSave2) },
                    enabled = online,
                    modifier = Modifier.weight(1f),
                ) { Text(if (amiibo.usingSave2) "Use clean" else "Use written") }
            }
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

    if (detailsOpen) PicoDialog(
        onDismiss = { detailsOpen = false },
        title = catalogTitle(catalog, item.displayName),
        dismissLabel = "Close",
    ) {
        Column(
            Modifier.fillMaxWidth().heightIn(max = LayoutTokens.DialogListMaxHeight)
                .verticalScroll(rememberScrollState()),
        ) {
            LabelValueRow("UID", item.uid, monospace = true, copyable = true)
            LabelValueRow("Figure ID", item.figureId, monospace = true, copyable = true)
            LabelValueRow("CRC32", item.crc32, monospace = true)
            LabelValueRow("Format", if (item.size == 2048) "Figure v3 · 2 KB" else "NTAG215 · ${item.size} B")
            LabelValueRow("Type", item.typeName.ifBlank { "Figure" })
            if (item.characterGameCode.isNotBlank()) LabelValueRow("Character code", item.characterGameCode)
            if (item.modelNumber.isNotBlank()) LabelValueRow("Model", item.modelNumber)
            catalog?.let {
                if (it.character.isNotBlank()) LabelValueRow("Character", it.character)
                if (it.gameSeries.isNotBlank()) LabelValueRow("Game series", it.gameSeries)
                if (it.amiiboSeries.isNotBlank()) LabelValueRow("Amiibo series", it.amiiboSeries)
                if (it.releaseDate.isNotBlank()) LabelValueRow("First release", it.releaseDate)
            }
            when {
                details == null && ui.amiiboKeysLoaded ->
                    LabelValueRow("Private data", "Reading…")
                details?.crypto == AmiiboCryptoState.Invalid ->
                    LabelValueRow("Private data", "Key did not verify this dump")
                details?.crypto == AmiiboCryptoState.Valid -> {
                    LabelValueRow("Registered", details.setupDate ?: "Not registered")
                    LabelValueRow("Last written", details.lastWriteDate ?: "Never")
                    LabelValueRow("Write count", details.writeCounter?.toString() ?: "—")
                    LabelValueRow("Game data", ui.selectedAmiiboTitleGame ?: details.appDataLabel.ifBlank { "None" })
                }
                else -> LabelValueRow("Private data", "Import a key to read")
            }
            if (ui.amiiboCatalogLoading) {
                LabelValueRow("Catalog", "Looking up…")
            } else if (catalog == null) {
                LabelValueRow("Catalog", "Offline; local identity is authoritative")
            }
        }
    }
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
    onImportArchive: () -> Unit,
    onExportArchive: () -> Unit,
    onImportKeys: () -> Unit,
) {
    var importOpen by rememberSaveable { mutableStateOf(false) }
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
            SettingsRow(
                title = "Import a library ZIP",
                supporting = "Replaces the phone library with a portal-compatible archive",
                leading = Icons.Default.FolderOpen,
                enabled = !ui.busy,
                onClick = { importOpen = true },
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
        }

        Spacer(Modifier.height(LayoutTokens.Space5))
    }

    if (importOpen) ConfirmDialog(
        onDismiss = { importOpen = false },
        title = "Replace phone library?",
        body = "This imports every validated Amiibo in the ZIP and replaces the current private phone library. The adapter is not changed. A failed import leaves the current library untouched.",
        confirmLabel = "Choose ZIP",
        destructive = true,
        onConfirm = { importOpen = false; onImportArchive() },
    )
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
        Box(modifier, contentAlignment = Alignment.Center) {
            Icon(
                Icons.Default.Contactless,
                contentDescription,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(36.dp),
            )
        }
    }
}
