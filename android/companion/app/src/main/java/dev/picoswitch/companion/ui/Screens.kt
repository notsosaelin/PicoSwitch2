@file:OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)

package dev.picoswitch.companion.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.background
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import dev.picoswitch.companion.BuildConfig
import dev.picoswitch.companion.controller.BridgePhase
import dev.picoswitch.companion.controller.ControllerFaceLayout
import dev.picoswitch.companion.data.ColorTarget
import dev.picoswitch.companion.model.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

@Composable
fun HomeScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    ScreenColumn("PicoSwitch2", "") {
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val wide = maxWidth >= LayoutTokens.TwoPaneBreakpoint
            if (wide) {
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                        AdapterHero(ui, viewModel)
                        ControllerCard(ui)
                    }
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                        AmiiboStatusCard(ui, viewModel)
                    }
                }
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                    AdapterHero(ui, viewModel); ControllerCard(ui); AmiiboStatusCard(ui, viewModel)
                }
            }
        }
    }
}

@Composable
private fun AdapterHero(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val snapshot = ui.snapshot
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Surface(shape = CircleShape, color = MaterialTheme.colorScheme.primaryContainer, modifier = Modifier.size(44.dp)) {
                Box(contentAlignment = Alignment.Center) { Icon(Icons.Default.Gamepad, null) }
            }
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f)) {
                Text(snapshot.personality.current.title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                Text(snapshot.firmware.version.ifBlank { "Firmware unavailable" }, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = viewModel::wake, enabled = ui.connection.connected && !ui.busy && ui.snapshot.capabilities.wake != CapabilityState.Unsupported) {
                Icon(Icons.Default.PowerSettingsNew, "Wake console")
            }
            IconButton(onClick = { viewModel.navigate(AppSection.Modes) }) {
                Icon(Icons.Default.Tune, "Change adapter mode")
            }
        }
    }
}

@Composable
private fun ControllerCard(ui: CompanionUiState) {
    val controller = ui.snapshot.controller
    HardwareCard {
        SectionHeading(Icons.Default.SportsEsports, "Input controller")
        Spacer(Modifier.height(LayoutTokens.Space3))
        if (controller.attached) {
            Text(controller.name, style = MaterialTheme.typography.titleLarge)
            if (controller.batteryValid) {
                Spacer(Modifier.height(LayoutTokens.Space2))
                LinearProgressIndicator({ controller.batteryPercent / 100f }, Modifier.fillMaxWidth())
                Text("${controller.batteryPercent}%${if (controller.charging) " · charging" else ""}", style = MaterialTheme.typography.labelMedium)
            }
        } else Text("No controller connected", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun AmiiboStatusCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    val a = ui.snapshot.amiibo
    val catalog = ui.adapterAmiiboCatalog
    val loaded = a.loaded || a.v3Loaded
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            SectionHeading(Icons.Default.Contactless, "Virtual Amiibo", Modifier.weight(1f))
            if (a.dirty) StatusPill("Needs sync", false) else if (loaded) StatusPill(if (a.presented) "Presented" else "Loaded", true)
        }
        Spacer(Modifier.height(LayoutTokens.Space3))
        if (loaded) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                AmiiboArtwork(catalog?.imageUrl.orEmpty(), catalogTitle(catalog, "Amiibo on adapter"), Modifier.size(64.dp))
                Spacer(Modifier.width(LayoutTokens.Space3))
                Column(Modifier.weight(1f)) {
                    Text(catalogTitle(catalog, "Amiibo on adapter"), style = MaterialTheme.typography.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(catalogSubtitle(catalog).ifBlank { "Figure ID ${a.figureId.ifBlank { "not reported" }}" }, style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    AmiiboCatalogStatus(ui.adapterAmiiboCatalogState)
                }
            }
        } else {
            Text("No Amiibo loaded", style = MaterialTheme.typography.titleLarge)
        }
        TextButton(onClick = { viewModel.navigate(AppSection.Amiibo) }, modifier = Modifier.align(Alignment.End)) {
            Text("Open library"); Icon(Icons.Default.ChevronRight, null)
        }
    }
}

@Composable
fun AmiiboScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onImport: () -> Unit,
    onImportArchive: () -> Unit,
    onExportArchive: () -> Unit,
    onImportKeys: () -> Unit,
    onScan: () -> Unit,
) {
    val selected = ui.library.firstOrNull { it.id == ui.selectedAmiiboId }
    val adapter = ui.snapshot.amiibo
    val adapterLoaded = adapter.loaded || adapter.v3Loaded
    val adapterMatchesSelected = selected != null && adapterLoaded &&
        selected.uid.isNotBlank() && selected.uid.equals(adapter.uid, ignoreCase = true)
    val adapterOnly = adapterLoaded && !adapterMatchesSelected
    var query by rememberSaveable { mutableStateOf("") }
    var filtersOpen by rememberSaveable { mutableStateOf(false) }
    var gameSeriesFilter by rememberSaveable { mutableStateOf("") }
    var amiiboSeriesFilter by rememberSaveable { mutableStateOf("") }
    var typeFilter by rememberSaveable { mutableStateOf("") }
    var sortOrder by rememberSaveable { mutableStateOf(AmiiboSortOrder.Name) }
    var importArchiveOpen by rememberSaveable { mutableStateOf(false) }
    val filtered = sortAmiiboLibrary(ui.library.filter { item ->
        val catalog = ui.amiiboCatalogEntries[item.id]
        val searchable = listOf(
            item.displayName, item.figureId, item.uid, item.typeName, item.characterGameCode,
            catalog?.name.orEmpty(), catalog?.character.orEmpty(), catalog?.gameSeries.orEmpty(), catalog?.amiiboSeries.orEmpty(),
        ).joinToString(" ").contains(query.trim(), ignoreCase = true)
        searchable &&
            (gameSeriesFilter.isBlank() || catalog?.gameSeries.equals(gameSeriesFilter, ignoreCase = true)) &&
            (amiiboSeriesFilter.isBlank() || catalog?.amiiboSeries.equals(amiiboSeriesFilter, ignoreCase = true)) &&
            (typeFilter.isBlank() || catalog?.type.equals(typeFilter, ignoreCase = true))
    }, ui.amiiboCatalogEntries, sortOrder)
    val gameSeriesOptions = ui.amiiboCatalogEntries.values.mapNotNull { it.gameSeries.takeIf(String::isNotBlank) }.distinct().sorted()
    val amiiboSeriesOptions = ui.amiiboCatalogEntries.values.mapNotNull { it.amiiboSeries.takeIf(String::isNotBlank) }.distinct().sorted()
    val typeOptions = ui.amiiboCatalogEntries.values.mapNotNull { it.type.takeIf(String::isNotBlank) }.distinct().sorted()

    Column(
        Modifier.fillMaxSize().padding(horizontal = LayoutTokens.Space2, vertical = LayoutTokens.Space2),
        verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
    ) {
    AmiiboToolbar(
            ui, onImport, { importArchiveOpen = true }, onExportArchive, onScan, query, { query = it },
            filtersOpen, { filtersOpen = !filtersOpen }, sortOrder, { sortOrder = it },
        )
        NfcScanStatusCard(ui)
        if (importArchiveOpen) AlertDialog(
            onDismissRequest = { importArchiveOpen = false },
            title = { Text("Replace phone library?") },
            text = { Text("This imports every validated Amiibo in the ZIP and replaces the current private phone library. The adapter is not changed. A failed import leaves the current library untouched.") },
            confirmButton = {
                TextButton(onClick = { importArchiveOpen = false; onImportArchive() }) { Text("Choose ZIP") }
            },
            dismissButton = { TextButton(onClick = { importArchiveOpen = false }) { Text("Cancel") } },
        )
        if (filtersOpen && (gameSeriesOptions.isNotEmpty() || amiiboSeriesOptions.isNotEmpty() || typeOptions.isNotEmpty())) {
            AmiiboFilterRow(
                gameSeriesFilter, amiiboSeriesFilter, typeFilter,
                gameSeriesOptions, amiiboSeriesOptions, typeOptions,
                { gameSeriesFilter = nextFilter(gameSeriesFilter, gameSeriesOptions) },
                { amiiboSeriesFilter = nextFilter(amiiboSeriesFilter, amiiboSeriesOptions) },
                { typeFilter = nextFilter(typeFilter, typeOptions) },
            )
        }
        ui.libraryWarnings.firstOrNull()?.let {
            Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
            if (maxWidth >= LayoutTokens.TwoPaneBreakpoint) {
                Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
                    AmiiboGrid(ui, viewModel, Modifier.weight(1f).fillMaxHeight(), filtered)
                    if (adapterOnly) {
                        AmiiboAdapterHero(ui, viewModel, Modifier.width(LayoutTokens.DetailWidth))
                    } else {
                        AmiiboDetail(selected, ui, viewModel, onImportKeys, Modifier.width(LayoutTokens.DetailWidth).fillMaxHeight())
                    }
                }
            } else {
                LazyColumn(
                    Modifier.fillMaxSize(),
                    verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
                    contentPadding = PaddingValues(bottom = LayoutTokens.Space4),
                ) {
                    if (adapterOnly) item { AmiiboAdapterHero(ui, viewModel, Modifier.fillMaxWidth()) }
                    else if (selected != null) item { AmiiboSelectedHero(selected, ui, viewModel, onImportKeys, Modifier.fillMaxWidth()) }
                    if (ui.library.isNotEmpty()) {
                        item {
                            Text(
                                if (query.isBlank()) "Your library · ${ui.library.size}" else "Matches · ${filtered.size}",
                                style = MaterialTheme.typography.titleSmall,
                                fontWeight = FontWeight.SemiBold,
                            )
                        }
                        if (filtered.isEmpty()) item { AmiiboInlineEmpty("No Amiibo matches that search or filter.") }
                        else items(filtered, key = { it.id }) { item -> AmiiboCompactListItem(item, ui, viewModel) }
                    } else {
                        item {
                            AmiiboInlineEmpty(
                                if (adapterLoaded) "No phone backup yet · Download to phone above to save this active tag."
                                else "Import a 540, 572, or 2048-byte backup to start your private library.",
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun AmiiboToolbar(
    ui: CompanionUiState,
    onImport: () -> Unit,
    onImportArchive: () -> Unit,
    onExportArchive: () -> Unit,
    onScan: () -> Unit,
    query: String,
    onQueryChanged: (String) -> Unit,
    filtersOpen: Boolean,
    onToggleFilters: () -> Unit,
    sortOrder: AmiiboSortOrder,
    onSortOrder: (AmiiboSortOrder) -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
        Column(Modifier.weight(1f)) {
            Text("Amiibo", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
            Text(
                "${ui.library.size} saved",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space1)) {
            FilledTonalButton(
                onClick = onScan,
                enabled = !ui.busy && ui.nfcScan.phase != NfcScanPhase.Unavailable,
                contentPadding = PaddingValues(horizontal = 10.dp),
            ) {
                Icon(Icons.Default.Contactless, null); Spacer(Modifier.width(LayoutTokens.Space1)); Text("Scan")
            }
            FilledTonalButton(onClick = onImport, enabled = !ui.busy, contentPadding = PaddingValues(horizontal = 10.dp)) {
                Icon(Icons.Default.Add, null); Spacer(Modifier.width(LayoutTokens.Space1)); Text("Import")
            }
        }
        OutlinedButton(onClick = onImportArchive, enabled = !ui.busy, contentPadding = PaddingValues(horizontal = 10.dp)) {
            Icon(Icons.Default.FolderOpen, null); Spacer(Modifier.width(LayoutTokens.Space1)); Text("ZIP")
        }
        TextButton(onClick = onExportArchive, enabled = !ui.busy && ui.library.isNotEmpty(), contentPadding = PaddingValues(horizontal = 8.dp)) {
            Icon(Icons.Default.Archive, null); Spacer(Modifier.width(LayoutTokens.Space1)); Text("Export")
        }
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
        OutlinedTextField(
            value = query,
            onValueChange = onQueryChanged,
            modifier = Modifier.weight(1f),
            singleLine = true,
            placeholder = { Text("Find an Amiibo") },
            leadingIcon = { Icon(Icons.Default.Search, null) },
        )
        IconButton(onClick = onToggleFilters, enabled = filtersOpen || ui.amiiboCatalogEntries.isNotEmpty()) {
            Icon(Icons.Default.FilterList, if (filtersOpen) "Hide filters" else "Filter library")
        }
        Box {
            var sortOpen by rememberSaveable { mutableStateOf(false) }
            IconButton(onClick = { sortOpen = true }) { Icon(Icons.Default.SortByAlpha, "Sort Amiibo") }
            DropdownMenu(expanded = sortOpen, onDismissRequest = { sortOpen = false }) {
                AmiiboSortOrder.entries.forEach { order ->
                    DropdownMenuItem(
                        text = { Text(order.label()) },
                        onClick = { onSortOrder(order); sortOpen = false },
                        leadingIcon = if (order == sortOrder) ({ Icon(Icons.Default.Check, null) }) else null,
                    )
                }
            }
        }
    }
}

@Composable
private fun NfcScanStatusCard(ui: CompanionUiState) {
    val status = ui.nfcScan
    val background = when (status.phase) {
        NfcScanPhase.Rejected -> MaterialTheme.colorScheme.errorContainer
        NfcScanPhase.Armed, NfcScanPhase.Reading, NfcScanPhase.Saving -> MaterialTheme.colorScheme.primaryContainer
        NfcScanPhase.Saved -> MaterialTheme.colorScheme.secondaryContainer
        else -> MaterialTheme.colorScheme.surfaceVariant
    }
    Surface(
        Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        color = background,
    ) {
        Row(
            Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Default.Contactless, null)
            Spacer(Modifier.width(LayoutTokens.Space2))
            Column {
                Text("Phone NFC backup", style = MaterialTheme.typography.titleSmall)
                Text(
                    status.message.ifBlank {
                        "Reads ordinary NTAG215 tags only; figure-v3 is deliberately rejected."
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

private fun AmiiboSortOrder.label(): String = when (this) {
    AmiiboSortOrder.Name -> "Name"
    AmiiboSortOrder.Series -> "Series"
    AmiiboSortOrder.RecentlyAdded -> "Recently added"
}

@Composable
private fun AmiiboFilterRow(
    gameSeries: String,
    amiiboSeries: String,
    type: String,
    gameSeriesOptions: List<String>,
    amiiboSeriesOptions: List<String>,
    typeOptions: List<String>,
    onGameSeries: () -> Unit,
    onAmiiboSeries: () -> Unit,
    onType: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2),
    ) {
        FilterChip(selected = gameSeries.isNotBlank(), onClick = onGameSeries, label = { Text(gameSeries.ifBlank { "Game series" }) }, enabled = gameSeriesOptions.isNotEmpty())
        FilterChip(selected = amiiboSeries.isNotBlank(), onClick = onAmiiboSeries, label = { Text(amiiboSeries.ifBlank { "Amiibo series" }) }, enabled = amiiboSeriesOptions.isNotEmpty())
        FilterChip(selected = type.isNotBlank(), onClick = onType, label = { Text(type.ifBlank { "Product type" }) }, enabled = typeOptions.isNotEmpty())
    }
}

private fun nextFilter(current: String, options: List<String>): String {
    if (options.isEmpty()) return ""
    val index = options.indexOf(current)
    return if (index < 0 || index == options.lastIndex) "" else options[index + 1]
}

@Composable
private fun AmiiboInlineEmpty(message: String) {
    Surface(shape = MaterialTheme.shapes.medium, color = MaterialTheme.colorScheme.surfaceVariant) {
        Row(Modifier.fillMaxWidth().padding(LayoutTokens.Space3), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Default.Contactless, null, tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.width(LayoutTokens.Space2))
            Text(message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun AmiiboAdapterHero(ui: CompanionUiState, viewModel: CompanionViewModel, modifier: Modifier) {
    var clearOpen by rememberSaveable { mutableStateOf(false) }
    val status = ui.snapshot.amiibo
    val catalog = ui.adapterAmiiboCatalog
    val pro2 = ui.snapshot.personality.current == Personality.Pro2
    val enabled = ui.connection.connected && !ui.busy &&
        ui.snapshot.capabilities.amiibo != CapabilityState.Unsupported && pro2
    if (clearOpen) AlertDialog(
        onDismissRequest = { clearOpen = false }, title = { Text("Clear adapter Amiibo?") },
        text = { Text("This active Amiibo has no local backup in the app. Download it first if you may need it later.") },
        confirmButton = { TextButton(onClick = { viewModel.clearAdapterAmiibo(); clearOpen = false }) { Text("Clear adapter") } },
        dismissButton = { TextButton(onClick = { clearOpen = false }) { Text("Cancel") } },
    )
    Card(modifier) {
        Column(Modifier.padding(LayoutTokens.Space3), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                AmiiboArtwork(catalog?.imageUrl.orEmpty(), catalogTitle(catalog, "Amiibo on adapter"), Modifier.size(80.dp))
                Spacer(Modifier.width(LayoutTokens.Space3))
                Column(Modifier.weight(1f)) {
                    Text(catalogTitle(catalog, "Amiibo on adapter"), style = MaterialTheme.typography.titleLarge, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(catalogSubtitle(catalog).ifBlank { "Figure ID ${status.figureId.ifBlank { "not reported" }}" }, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Spacer(Modifier.height(LayoutTokens.Space1))
                    StatusPill(if (status.presented) "Presented" else "Loaded", true)
                }
            }
            AmiiboCatalogStatus(ui.adapterAmiiboCatalogState)
            Text("UID ${status.uid.ifBlank { "unknown" }} · ${status.size} bytes · generation ${status.generation}", style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
        if (!pro2) {
            Text("Switch to Pro Controller 2 mode to manage virtual Amiibo.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
            Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                FilledTonalButton(onClick = viewModel::syncSelectedAmiibo, enabled = enabled, modifier = Modifier.weight(1f)) { Text("Download to phone") }
                OutlinedButton(onClick = { viewModel.setPresented(!status.presented) }, enabled = enabled, modifier = Modifier.weight(1f)) {
                    Text(if (status.presented) "Eject" else "Present")
                }
            }
            TextButton(onClick = { clearOpen = true }, enabled = enabled && !status.dirty, modifier = Modifier.align(Alignment.End)) { Text("Clear adapter") }
            if (status.dirty) Text("Console-written data is unsynced. Download it before clearing.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun AmiiboSelectedHero(item: AmiiboLibraryItem, ui: CompanionUiState, viewModel: CompanionViewModel, onImportKeys: () -> Unit, modifier: Modifier) {
    val catalog = ui.selectedAmiiboCatalog
    val details = ui.selectedAmiiboDetails
    var initializeOpen by rememberSaveable(item.id) { mutableStateOf(false) }
    if (initializeOpen) AlertDialog(
        onDismissRequest = { initializeOpen = false },
        title = { Text("Initialize this Amiibo?") },
        text = { Text("This wipes the owner, nickname, registration and game data in the private phone copy, then re-signs it with your imported key_retail.bin. The UID and identity stay the same, the adapter is not changed, and this cannot be undone.") },
        confirmButton = {
            TextButton(onClick = { initializeOpen = false; viewModel.initializeSelectedAmiibo() }) { Text("Initialize") }
        },
        dismissButton = { TextButton(onClick = { initializeOpen = false }) { Text("Cancel") } },
    )
    Card(modifier) {
        Column(Modifier.padding(LayoutTokens.Space3), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                AmiiboArtwork(catalog?.imageUrl.orEmpty(), catalogTitle(catalog, item.displayName), Modifier.size(80.dp))
                Spacer(Modifier.width(LayoutTokens.Space3))
                Column(Modifier.weight(1f)) {
                    Text(catalogTitle(catalog, item.displayName), style = MaterialTheme.typography.titleLarge, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text(catalogSubtitle(catalog).ifBlank { "Figure ID ${item.figureId}" }, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text("${item.size} bytes · ${item.uid}", style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
            AmiiboCatalogStatus(if (catalog != null) AmiiboCatalogState.Available else if (ui.amiiboCatalogLoading) AmiiboCatalogState.Loading else AmiiboCatalogState.Offline)
            if (details?.crypto == AmiiboCryptoState.Valid) {
                Text(
                    "Owner ${details.owner.ifBlank { "not set" }} · Nickname ${details.nickname.ifBlank { "not set" }}",
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    "Registered ${details.setupDate ?: "not registered"} · Last written ${details.lastWriteDate ?: "never"} · Writes ${details.writeCounter ?: 0}",
                    style = MaterialTheme.typography.labelSmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                Button(onClick = viewModel::loadSelectedAmiibo, enabled = ui.connection.connected && !ui.busy, modifier = Modifier.weight(1f)) { Text("Load") }
                OutlinedButton(onClick = viewModel::syncSelectedAmiibo, enabled = ui.connection.connected && (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded) && !ui.busy, modifier = Modifier.weight(1f)) { Text("Sync") }
            }
            OutlinedButton(
                onClick = { if (ui.amiiboKeysLoaded) initializeOpen = true else onImportKeys() },
                enabled = !ui.busy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(if (ui.amiiboKeysLoaded) "Initialize locally" else "Import key to initialize") }
            if (ui.snapshot.amiibo.dirty) {
                Text("Console changes are not synced; download before clearing.", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable
private fun AmiiboCompactListItem(item: AmiiboLibraryItem, ui: CompanionUiState, viewModel: CompanionViewModel) {
    val selected = item.id == ui.selectedAmiiboId
    val catalog = ui.amiiboCatalogEntries[item.id]
    Card(
        Modifier.fillMaxWidth().clickable { viewModel.selectAmiibo(item.id) },
        colors = CardDefaults.cardColors(containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant),
        border = if (selected) CardDefaults.outlinedCardBorder() else null,
    ) {
        Row(Modifier.padding(LayoutTokens.Space2), verticalAlignment = Alignment.CenterVertically) {
            AmiiboArtwork(catalog?.imageUrl.orEmpty(), catalogTitle(catalog, item.displayName), Modifier.size(60.dp))
            Spacer(Modifier.width(LayoutTokens.Space2))
            Column(Modifier.weight(1f)) {
                Text(catalogTitle(catalog, item.displayName), style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(catalogSubtitle(catalog).ifBlank { item.figureId }, style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("${item.size} bytes · ${item.uid}", style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            if (selected) Icon(Icons.Default.ChevronRight, "Selected", tint = MaterialTheme.colorScheme.primary)
        }
    }
}

private fun catalogTitle(catalog: AmiiboCatalogEntry?, fallback: String): String =
    catalog?.name?.takeIf(String::isNotBlank) ?: catalog?.character?.takeIf(String::isNotBlank) ?: fallback

private fun catalogSubtitle(catalog: AmiiboCatalogEntry?): String = listOfNotNull(
    catalog?.gameSeries?.takeIf(String::isNotBlank),
    catalog?.amiiboSeries?.takeIf(String::isNotBlank),
    catalog?.type?.takeIf(String::isNotBlank),
).joinToString(" · ")

@Composable
private fun AmiiboCatalogStatus(state: AmiiboCatalogState) {
    when (state) {
        AmiiboCatalogState.Loading -> Text("Catalog: loading", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        AmiiboCatalogState.Offline -> Text("Catalog: offline", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        AmiiboCatalogState.Unmatched -> Text("Catalog: unmatched", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        else -> Unit
    }
}

@Composable
private fun AmiiboGrid(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    modifier: Modifier,
    items: List<AmiiboLibraryItem> = ui.library,
) {
    if (items.isEmpty()) {
        EmptyState(Icons.Default.Contactless, "Your library is empty", "Import your own 540, 572, or 2048-byte Amiibo backup.", modifier)
        return
    }
    LazyVerticalGrid(
        columns = GridCells.Adaptive(168.dp), modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space3),
        contentPadding = PaddingValues(bottom = LayoutTokens.Space5),
    ) {
        items(items, key = { it.id }) { item ->
            val selected = item.id == ui.selectedAmiiboId
            val catalog = ui.amiiboCatalogEntries[item.id]
            Card(
                modifier = Modifier.fillMaxWidth().clickable { viewModel.selectAmiibo(item.id) },
                colors = CardDefaults.cardColors(containerColor = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant),
                border = if (selected) CardDefaults.outlinedCardBorder() else null,
            ) {
                Column(Modifier.padding(LayoutTokens.Space4)) {
                    if (catalog?.imageUrl?.isNotBlank() == true) {
                        AmiiboArtwork(catalog.imageUrl, catalogTitle(catalog, item.displayName), Modifier.size(72.dp))
                    } else {
                        Icon(Icons.Default.Contactless, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(36.dp))
                    }
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    Text(
                        catalog?.name?.ifBlank { null } ?: catalog?.character?.ifBlank { null } ?: item.displayName,
                        maxLines = 2, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleMedium,
                    )
                    Spacer(Modifier.height(LayoutTokens.Space1))
                    val catalogSubtitle = listOfNotNull(
                        catalog?.gameSeries?.takeIf { it.isNotBlank() },
                        catalog?.amiiboSeries?.takeIf { it.isNotBlank() },
                        catalog?.type?.takeIf { it.isNotBlank() },
                    ).joinToString(" · ")
                    val subtitle = catalogSubtitle.ifBlank {
                        listOfNotNull(item.typeName.takeIf { it.isNotBlank() }, item.characterGameCode.takeIf { it.isNotBlank() })
                            .joinToString(" · ").ifBlank { item.figureId.ifBlank { "Unknown figure" } }
                    }
                    Text(
                        subtitle,
                        maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodySmall,
                    )
                    Text("${item.size} bytes · ${item.uid}", maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.labelSmall)
                }
            }
        }
    }
}

@Composable
private fun AmiiboDetail(item: AmiiboLibraryItem?, ui: CompanionUiState, viewModel: CompanionViewModel, onImportKeys: () -> Unit, modifier: Modifier) {
    var renameOpen by rememberSaveable(item?.id) { mutableStateOf(false) }
    var deleteOpen by rememberSaveable(item?.id) { mutableStateOf(false) }
    var clearOpen by rememberSaveable { mutableStateOf(false) }
    var initializeOpen by rememberSaveable(item?.id) { mutableStateOf(false) }
    var name by rememberSaveable(item?.id) { mutableStateOf(item?.displayName.orEmpty()) }
    if (renameOpen && item != null) AlertDialog(
        onDismissRequest = { renameOpen = false },
        title = { Text("Rename local backup") },
        text = { OutlinedTextField(name, { name = it.take(120) }, label = { Text("Name") }, singleLine = true) },
        confirmButton = { TextButton(onClick = { viewModel.renameSelectedAmiibo(name); renameOpen = false }) { Text("Rename") } },
        dismissButton = { TextButton(onClick = { renameOpen = false }) { Text("Cancel") } },
    )
    if (deleteOpen && item != null) AlertDialog(
        onDismissRequest = { deleteOpen = false }, title = { Text("Delete local backup?") },
        text = { Text("This removes ${item.displayName} from this phone only. It cannot be undone and does not clear the adapter.") },
        confirmButton = { TextButton(onClick = { viewModel.deleteSelectedAmiibo(); deleteOpen = false }) { Text("Delete") } },
        dismissButton = { TextButton(onClick = { deleteOpen = false }) { Text("Cancel") } },
    )
    if (clearOpen) AlertDialog(
        onDismissRequest = { clearOpen = false }, title = { Text("Clear adapter Amiibo?") },
        text = { Text("This removes the stored virtual Amiibo from the adapter. Your private phone backup remains available.") },
        confirmButton = { TextButton(onClick = { viewModel.clearAdapterAmiibo(); clearOpen = false }) { Text("Clear adapter") } },
        dismissButton = { TextButton(onClick = { clearOpen = false }) { Text("Cancel") } },
    )
    if (initializeOpen && item != null) AlertDialog(
        onDismissRequest = { initializeOpen = false },
        title = { Text("Initialize this Amiibo?") },
        text = { Text("This wipes the owner, nickname, registration and game data in the private phone copy, then re-signs it with your imported key_retail.bin. The UID and identity stay the same, the adapter is not changed, and this cannot be undone.") },
        confirmButton = {
            TextButton(onClick = { initializeOpen = false; viewModel.initializeSelectedAmiibo() }) { Text("Initialize") }
        },
        dismissButton = { TextButton(onClick = { initializeOpen = false }) { Text("Cancel") } },
    )
    Card(modifier) {
        if (item == null) {
            EmptyState(Icons.Default.TouchApp, "Select an Amiibo", "Choose a local backup to inspect or load.", Modifier.fillMaxSize())
        } else {
            Column(Modifier.padding(LayoutTokens.Space4).verticalScroll(rememberScrollState())) {
                val catalog = ui.selectedAmiiboCatalog
                if (catalog != null) {
                    AmiiboArtwork(catalog.imageUrl, catalog.name.ifBlank { catalog.character })
                    Spacer(Modifier.height(LayoutTokens.Space3))
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(catalog?.name?.ifBlank { null } ?: catalog?.character?.ifBlank { null } ?: item.displayName, style = MaterialTheme.typography.titleLarge)
                        Text(
                            listOfNotNull(
                                catalog?.gameSeries?.takeIf { it.isNotBlank() },
                                catalog?.amiiboSeries?.takeIf { it.isNotBlank() },
                                catalog?.type?.takeIf { it.isNotBlank() },
                            ).joinToString(" · ").ifBlank {
                                listOfNotNull(item.typeName.takeIf { it.isNotBlank() }, item.characterGameCode.takeIf { it.isNotBlank() })
                                    .joinToString(" · ").ifBlank { "Figure ${item.figureId}" }
                            },
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    IconButton(onClick = { renameOpen = true }) { Icon(Icons.Default.Edit, "Rename local copy") }
                    IconButton(onClick = { deleteOpen = true }) { Icon(Icons.Default.DeleteOutline, "Delete local copy") }
                }
                Spacer(Modifier.height(LayoutTokens.Space3))
                MetadataLine("UID", item.uid)
                MetadataLine("CRC32", item.crc32)
                MetadataLine("Format", if (item.size == 2048) "Figure v3 · 2 KB" else "NTAG215 · ${item.size} B")
                MetadataLine("Character code", item.characterGameCode.ifBlank { "—" })
                MetadataLine("Character variant", "%02X".format(item.characterVariant))
                MetadataLine("Type", item.typeName.ifBlank { "Figure" })
                MetadataLine("Model / series", listOf(item.modelNumber, "%02X".format(item.seriesCode)).joinToString(" · "))
                MetadataLine("Format version", "%02X".format(item.formatVersion))
                if (item.extendedVariant.isNotBlank()) MetadataLine("Extended variant", item.extendedVariant)
                AmiiboCatalogDetails(catalog, ui.amiiboCatalogLoading)
                AmiiboRegisterDetails(ui)
                HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space3))
                Button(onClick = viewModel::loadSelectedAmiibo, enabled = ui.connection.connected && !ui.busy, modifier = Modifier.fillMaxWidth()) { Text("Load onto adapter") }
                Spacer(Modifier.height(LayoutTokens.Space2))
                OutlinedButton(
                    onClick = { if (ui.amiiboKeysLoaded) initializeOpen = true else onImportKeys() },
                    enabled = !ui.busy,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(if (ui.amiiboKeysLoaded) "Initialize locally" else "Import key to initialize") }
                Spacer(Modifier.height(LayoutTokens.Space2))
                Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2)) {
                    FilledTonalButton(onClick = { viewModel.setPresented(!ui.snapshot.amiibo.presented) }, enabled = ui.connection.connected && (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded), modifier = Modifier.weight(1f)) {
                        Text(if (ui.snapshot.amiibo.presented) "Eject" else "Present")
                    }
                    OutlinedButton(onClick = viewModel::syncSelectedAmiibo, enabled = ui.connection.connected && (ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded), modifier = Modifier.weight(1f)) { Text("Sync") }
                }
                if (ui.snapshot.amiibo.hasSave2) {
                    Spacer(Modifier.height(LayoutTokens.Space2))
                    OutlinedButton(onClick = { viewModel.selectCopy(!ui.snapshot.amiibo.usingSave2) }, Modifier.fillMaxWidth()) {
                        Text(if (ui.snapshot.amiibo.usingSave2) "Use clean copy" else "Use console-written copy")
                    }
                }
                if (ui.snapshot.amiibo.dirty) {
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    Surface(shape = MaterialTheme.shapes.small, color = MaterialTheme.colorScheme.errorContainer) {
                        Row(Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2), verticalAlignment = Alignment.CenterVertically) {
                            Icon(Icons.Default.Warning, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Console changes are not synced")
                        }
                    }
                }
                if ((ui.snapshot.amiibo.loaded || ui.snapshot.amiibo.v3Loaded) && !ui.snapshot.amiibo.dirty) {
                    Spacer(Modifier.height(LayoutTokens.Space3))
                    TextButton(onClick = { clearOpen = true }, Modifier.fillMaxWidth()) { Text("Clear adapter Amiibo") }
                }
            }
        }
    }
}

@Composable
private fun AmiiboCatalogDetails(catalog: AmiiboCatalogEntry?, loading: Boolean) {
    Spacer(Modifier.height(LayoutTokens.Space2))
    Text("Catalog details", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
    when {
        catalog != null -> {
            if (catalog.name.isNotBlank() && catalog.name != catalog.character)
                MetadataLine("Name", catalog.name)
            MetadataLine("Character", catalog.character.ifBlank { "Unknown" })
            MetadataLine("Game series", catalog.gameSeries.ifBlank { "Unknown" })
            MetadataLine("Amiibo series", catalog.amiiboSeries.ifBlank { "Unknown" })
            MetadataLine("Product type", catalog.type.ifBlank { "Unknown" })
            MetadataLine("First release", catalog.releaseDate.ifBlank { "Unknown" })
            if (catalog.games.isNotEmpty()) {
                val compatible = catalog.games.entries.joinToString(" · ") { (platform, names) ->
                    "$platform: ${names.distinct().joinToString(", ")}"
                }
                MetadataLine("Works with", compatible.ifBlank { "Catalog listed" })
            }
        }
        loading -> Text("Looking up optional AmiiboAPI details…", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        else -> Text("Catalog unavailable offline; the local identity above remains authoritative.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun AmiiboArtwork(
    imageUrl: String,
    contentDescription: String,
    modifier: Modifier = Modifier.fillMaxWidth().heightIn(max = 180.dp),
) {
    val image = produceState<androidx.compose.ui.graphics.ImageBitmap?>(null, imageUrl) {
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
        Box(modifier.heightIn(min = 76.dp), contentAlignment = Alignment.Center) {
            Icon(Icons.Default.Contactless, contentDescription, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(48.dp))
        }
    }
}

@Composable
private fun AmiiboRegisterDetails(ui: CompanionUiState) {
    val details = ui.selectedAmiiboDetails
    if (!ui.amiiboKeysLoaded && details == null) return
    Spacer(Modifier.height(LayoutTokens.Space2))
    Text("Private register details", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
    when {
        details == null -> {
            Spacer(Modifier.height(LayoutTokens.Space1))
            Text("Reading local encrypted metadata…", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        details.crypto == AmiiboCryptoState.Invalid -> {
            Spacer(Modifier.height(LayoutTokens.Space1))
            Text("The imported key did not verify this dump. No decrypted fields are shown.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        }
        details.crypto == AmiiboCryptoState.Valid -> {
            MetadataLine("Owner", details.owner.ifBlank { "Not set" })
            MetadataLine("Nickname", details.nickname.ifBlank { "Not set" })
            MetadataLine("Registered", details.setupDate ?: "Not registered")
            MetadataLine("Last written", details.lastWriteDate ?: "Never written")
            MetadataLine("Write count", details.writeCounter?.toString() ?: "—")
            MetadataLine("Game data", ui.selectedAmiiboTitleGame ?: details.appDataLabel.ifBlank { "None" })
            if (details.titleId.isNotBlank()) MetadataLine("Title ID", details.titleId)
            if (details.appId.isNotBlank()) MetadataLine("App ID", details.appId)
            Text("HMAC verified locally", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
        }
    }
}

@Composable
fun ControllerScreen(ui: CompanionUiState, viewModel: CompanionViewModel, onPrepare: () -> Unit) {
    ScreenColumn("Input", "Use this handheld as the active controller") {
        Surface(color = MaterialTheme.colorScheme.secondaryContainer, shape = MaterialTheme.shapes.medium) {
            Row(Modifier.fillMaxWidth().padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space2), verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.Visibility, null)
                Spacer(Modifier.width(LayoutTokens.Space2))
                Text("Keep this screen open while playing", style = MaterialTheme.typography.bodyMedium)
            }
        }
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val wide = maxWidth >= LayoutTokens.TwoPaneBreakpoint
            val source: @Composable () -> Unit = { InputSourceCard(ui, viewModel) }
            val layout: @Composable () -> Unit = { ControllerLayoutCard(ui, viewModel) }
            val bridge: @Composable () -> Unit = { BridgeCard(ui, viewModel, onPrepare) }
            if (wide) Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) { source(); layout() }
                Box(Modifier.weight(1f)) { bridge() }
            } else Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) { source(); layout(); bridge() }
        }
        InputDiagnostics(ui.controllerState)
    }
}

@Composable
private fun ControllerLayoutCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    HardwareCard {
        SectionHeading(Icons.Default.SwapHoriz, "Controller layout")
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text(
            "Android reports face-button positions, which may not match the letters printed on Nintendo-style handhelds.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        ControllerFaceLayout.entries.forEach { layout ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = LayoutTokens.TouchHeight)
                    .selectable(
                        selected = ui.requestedFaceLayout == layout,
                        enabled = ui.selectedSourceDescriptor != null,
                        role = Role.RadioButton,
                        onClick = { viewModel.setControllerFaceLayout(layout) },
                    ),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                RadioButton(selected = ui.requestedFaceLayout == layout, onClick = null, enabled = ui.selectedSourceDescriptor != null)
                Spacer(Modifier.width(LayoutTokens.Space2))
                Column(Modifier.weight(1f)) {
                    Text(layout.title)
                    Text(layout.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        if (ui.requestedFaceLayout == ControllerFaceLayout.Auto) {
            Text(
                "Auto resolved to ${ui.resolvedFaceLayout.layout.title}: ${ui.resolvedFaceLayout.reason}.",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
    }
}

@Composable
private fun InputSourceCard(ui: CompanionUiState, viewModel: CompanionViewModel) {
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            SectionHeading(Icons.Default.Gamepad, "Built-in input", Modifier.weight(1f))
            IconButton(onClick = viewModel::refreshSources) { Icon(Icons.Default.Refresh, "Refresh inputs") }
        }
        if (ui.sourceDevices.isEmpty()) Text("No Android gamepad/joystick input device is visible to the app.")
        ui.sourceDevices.forEach { source ->
            val selected = source.descriptor == ui.selectedSourceDescriptor
            Surface(
                Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2).clickable { viewModel.selectSource(source.descriptor) },
                shape = MaterialTheme.shapes.medium,
                color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
            ) {
                Row(Modifier.padding(LayoutTokens.Space3), verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected, { viewModel.selectSource(source.descriptor) })
                    Column(Modifier.weight(1f)) {
                        Text(source.name, style = MaterialTheme.typography.titleSmall)
                        Text("VID %04X · PID %04X".format(source.vendorId, source.productId), style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
    }
}

@Composable
private fun BridgeCard(ui: CompanionUiState, viewModel: CompanionViewModel, onPrepare: () -> Unit) {
    HardwareCard {
        SectionHeading(Icons.Default.BluetoothAudio, "Controller link")
        Spacer(Modifier.height(LayoutTokens.Space2))
        Text(ui.bridge.phase.name, style = MaterialTheme.typography.titleLarge)
        ui.bridge.message?.let { Text(it, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        Spacer(Modifier.height(LayoutTokens.Space3))
        when (ui.bridge.phase) {
            BridgePhase.Idle, BridgePhase.Unsupported, BridgePhase.Failed ->
                Button(
                    onClick = onPrepare,
                    enabled = ui.selectedSourceDescriptor != null && ui.adapterRelationship != null,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(if (ui.adapterRelationship == null) "Pair Adapter first" else "Use this handheld") }
            BridgePhase.Playing ->
                Button(onClick = viewModel::stopControllerBridge, modifier = Modifier.fillMaxWidth(), colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)) { Text("Stop playing") }
            else -> LinearProgressIndicator(Modifier.fillMaxWidth())
        }
        if (ui.bridge.phase == BridgePhase.Ready) {
            val host = viewModel.knownControllerHost()
            FilledTonalButton(
                onClick = { host?.let(viewModel::connectControllerHost) },
                enabled = host != null,
                modifier = Modifier.fillMaxWidth().padding(top = LayoutTokens.Space2),
            ) { Text("Reconnect controller mode") }
            Text(
                if (host == null) "The saved adapter bond is unavailable. Reconnect or pair the adapter again."
                else "Controller mode uses the adapter relationship already established by Pair Adapter; there is no second chooser.",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun InputDiagnostics(state: dev.picoswitch.companion.controller.ControllerState) {
    HardwareCard {
        SectionHeading(Icons.Default.MonitorHeart, "Live input")
        Spacer(Modifier.height(LayoutTokens.Space3))
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space3)) {
            AxisMeter("LX", state.leftX, Modifier.weight(1f)); AxisMeter("LY", state.leftY, Modifier.weight(1f))
            AxisMeter("RX", state.rightX, Modifier.weight(1f)); AxisMeter("RY", state.rightY, Modifier.weight(1f))
        }
        Spacer(Modifier.height(LayoutTokens.Space3))
        Text("L2 ${state.leftTrigger}  ·  R2 ${state.rightTrigger}  ·  Hat ${dev.picoswitch.companion.controller.ControllerReportEncoder.hat(state)}", style = MaterialTheme.typography.bodyMedium)
        Text(if (state.buttons.isEmpty()) "No buttons held" else state.buttons.joinToString(" · ") { it.name }, style = MaterialTheme.typography.bodySmall)
        Text("Motion and rumble are not part of the v1 bridge protocol.", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun AxisMeter(label: String, value: Int, modifier: Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        LinearProgressIndicator({ value / 255f }, Modifier.fillMaxWidth())
        Text("$label $value", style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
fun ModesScreen(ui: CompanionUiState, viewModel: CompanionViewModel) {
    ScreenColumn("Adapter", "Personality and controller colors") {
        HardwareCard {
            SectionHeading(Icons.Default.Cable, "Output personality")
            FlowRow(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space2), modifier = Modifier.padding(top = LayoutTokens.Space3)) {
                val choices = ui.snapshot.personality.available.ifEmpty { listOf(Personality.Pro2, Personality.GameCube, Personality.JoyConLeft, Personality.JoyConRight) }
                choices.forEach { mode ->
                    FilterChip(
                        selected = mode == ui.snapshot.personality.current,
                        onClick = { viewModel.switchPersonality(mode) }, enabled = ui.connection.connected && ui.snapshot.capabilities.personality != CapabilityState.Unsupported,
                        label = { Text(mode.title) }, leadingIcon = if (mode == ui.snapshot.personality.current) ({ Icon(Icons.Default.Check, null) }) else null,
                    )
                }
            }
        }
        ColorEditor("Body / lightbar", ColorTarget.Body, ui.snapshot.config.bodyColor, ui.connection.connected, viewModel)
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            if (maxWidth >= LayoutTokens.TwoPaneBreakpoint) Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                Box(Modifier.weight(1f)) { ColorEditor("Joy-Con 2 left accent", ColorTarget.LeftAccent, ui.snapshot.config.leftAccent, ui.connection.connected, viewModel) }
                Box(Modifier.weight(1f)) { ColorEditor("Joy-Con 2 right accent", ColorTarget.RightAccent, ui.snapshot.config.rightAccent, ui.connection.connected, viewModel) }
            } else Column(verticalArrangement = Arrangement.spacedBy(LayoutTokens.Space4)) {
                ColorEditor("Joy-Con 2 left accent", ColorTarget.LeftAccent, ui.snapshot.config.leftAccent, ui.connection.connected, viewModel)
                ColorEditor("Joy-Con 2 right accent", ColorTarget.RightAccent, ui.snapshot.config.rightAccent, ui.connection.connected, viewModel)
            }
        }
        if (ui.identityRefreshPending) {
            HardwareCard(container = MaterialTheme.colorScheme.primaryContainer) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Apply identity changes", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                        Text("Reconnects USB without changing the selected adapter mode.", style = MaterialTheme.typography.bodySmall)
                    }
                    Spacer(Modifier.width(LayoutTokens.Space3))
                    Button(onClick = viewModel::applyIdentityChanges, enabled = ui.connection.connected && !ui.busy) {
                        Text("Apply")
                    }
                }
            }
        }
    }
}

@Composable
private fun ColorEditor(title: String, target: ColorTarget, initial: RgbColor, enabled: Boolean, viewModel: CompanionViewModel) {
    var red by rememberSaveable(initial) { mutableFloatStateOf(initial.red.toFloat()) }
    var green by rememberSaveable(initial) { mutableFloatStateOf(initial.green.toFloat()) }
    var blue by rememberSaveable(initial) { mutableFloatStateOf(initial.blue.toFloat()) }
    val current = RgbColor(red.toInt(), green.toInt(), blue.toInt())
    HardwareCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(36.dp).clip(CircleShape).background(Color(current.argb())))
            Spacer(Modifier.width(LayoutTokens.Space3)); Text(title, Modifier.weight(1f), style = MaterialTheme.typography.titleMedium)
            Text("#%02X%02X%02X".format(current.red, current.green, current.blue), style = MaterialTheme.typography.labelMedium)
        }
        ColorSlider("R", red, { red = it }, Color(0xFFFF6B6B)); ColorSlider("G", green, { green = it }, Color(0xFF5DDA84)); ColorSlider("B", blue, { blue = it }, Color(0xFF69A7FF))
        Button(onClick = { viewModel.saveColor(target, current) }, enabled = enabled, modifier = Modifier.fillMaxWidth()) { Text("Save color") }
    }
}

@Composable
private fun ColorSlider(label: String, value: Float, onValue: (Float) -> Unit, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, Modifier.width(24.dp), fontWeight = FontWeight.Bold, color = color)
        Slider(value, onValue, valueRange = 0f..255f, modifier = Modifier.weight(1f).semantics { contentDescription = "$label color value" })
        Text(value.toInt().toString(), Modifier.width(38.dp), style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
fun SettingsScreen(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onExportDiagnostics: () -> Unit,
    onImportAmiiboKeys: () -> Unit,
    theme: ThemeSelection,
) {
    var appearanceOpen by rememberSaveable { mutableStateOf(false) }
    var keysOpen by rememberSaveable { mutableStateOf(false) }
    var connectionOpen by rememberSaveable { mutableStateOf(false) }
    var aboutOpen by rememberSaveable { mutableStateOf(false) }
    var developerOpen by rememberSaveable { mutableStateOf(false) }
    ScreenColumn("Settings", "") {
        SettingsGroup(Icons.Default.Palette, "Appearance", "${theme.mode.title} · ${theme.palette.title}", appearanceOpen, { appearanceOpen = !appearanceOpen }) {
            ThemeSettingsContent(theme, viewModel)
        }
        SettingsGroup(Icons.Default.Key, "Amiibo metadata", if (ui.amiiboKeysLoaded) "Key file available" else "No key file imported", keysOpen, { keysOpen = !keysOpen }) {
            AmiiboKeySettingsContent(ui, onImportAmiiboKeys)
        }
        SettingsGroup(Icons.Default.Link, "Connections", "Wireless management and phone bonds", connectionOpen, { connectionOpen = !connectionOpen }) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Wireless management", style = MaterialTheme.typography.titleSmall)
                    Text("Available until the adapter reboots", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(
                    checked = ui.snapshot.managementEnabled == true,
                    onCheckedChange = viewModel::setManagement,
                    enabled = ui.connection.connected && ui.snapshot.capabilities.managementGate == CapabilityState.Available,
                )
            }
            HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space2))
            Text("Phone bonds", style = MaterialTheme.typography.titleSmall)
            when {
                ui.snapshot.capabilities.bonds == CapabilityState.Unsupported -> Text("Bond controls are unavailable on this firmware.", style = MaterialTheme.typography.bodySmall)
                ui.snapshot.bondsComplete != true -> Text(
                    "Bond list completeness is unknown; no partial entries are shown.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
                ui.snapshot.bonds.isEmpty() -> Text("No management bonds", style = MaterialTheme.typography.bodySmall)
            }
            if (ui.snapshot.bondsComplete == true) {
                ui.snapshot.bonds.forEach { bond ->
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Text(bond.name ?: bond.address, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        IconButton(onClick = { viewModel.removeBond(bond.index) }, enabled = !ui.busy) {
                            Icon(Icons.Default.LinkOff, "Remove management bond ${bond.index}")
                        }
                    }
                }
            }
        }
        SettingsGroup(Icons.Default.Info, "About", "App and adapter versions", aboutOpen, { aboutOpen = !aboutOpen }) {
            MetadataLine("App", BuildConfig.VERSION_NAME)
            MetadataLine("Firmware", ui.snapshot.firmware.version.ifBlank { "Not connected" })
            MetadataLine("Protocol", "BLE GATT · JSON v1")
        }
        SettingsGroup(Icons.Default.DeveloperMode, "Developer", "Diagnostics, identity, and validation", developerOpen, { developerOpen = !developerOpen }) {
            MetadataLine("Bluetooth", "available ${ui.platform.bluetoothAvailable} · enabled ${ui.platform.bluetoothEnabled}")
            MetadataLine("Permissions", "scan ${ui.platform.scanPermission} · connect ${ui.platform.connectPermission}")
            MetadataLine("Management", ui.connection.phase.name)
            MetadataLine("HID", "${ui.bridge.phase.name} · registered ${ui.bridge.registered}")
            MetadataLine("Saved host", if (viewModel.pairedControllerHosts().isEmpty()) "No" else "Yes")
            MetadataLine("Reports", ui.bridge.reportCount.toString())
            MetadataLine("Last command", ui.diagnosticSummary.lastCommand)
            MetadataLine("Last result", ui.diagnosticSummary.lastResult)
            MetadataLine("Last error", ui.diagnosticSummary.lastError)
            if (ui.identityRefreshPending) TextButton(onClick = viewModel::clearIdentityRefreshPending) { Text("Mark identity refresh complete") }
            Button(onClick = onExportDiagnostics, Modifier.fillMaxWidth()) {
                Icon(Icons.Default.Share, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Share diagnostics")
            }
            HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space2))
            Text("Management writes are not authenticated by current firmware. Hardware validation is still required for controller coexistence and OEM HID behavior.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun AmiiboKeySettingsContent(ui: CompanionUiState, onImportAmiiboKeys: () -> Unit) {
    Text(
        "Choose your portal-compatible key_retail.bin to show private owner, nickname, date, and game-data fields.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(LayoutTokens.Space2))
    OutlinedButton(onClick = onImportAmiiboKeys, enabled = !ui.busy, modifier = Modifier.fillMaxWidth()) {
        Icon(Icons.Default.FolderOpen, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text("Choose key file")
    }
}

@Composable
private fun ThemeSettingsContent(selection: ThemeSelection, viewModel: CompanionViewModel) {
    Text("Theme", style = MaterialTheme.typography.titleSmall)
    ThemeMode.entries.forEach { mode ->
        ThemeChoiceRow(
            selected = selection.mode == mode,
            title = mode.title,
            description = mode.description,
            onClick = { viewModel.setThemeMode(mode) },
        )
    }
    HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space2))
    Text("Accent palette", style = MaterialTheme.typography.titleSmall)
    AccentPalette.entries.forEach { palette ->
        PaletteChoiceRow(
            selected = selection.palette == palette,
            palette = palette,
            onClick = { viewModel.setAccentPalette(palette) },
        )
    }
}

@Composable
private fun SettingsGroup(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    title: String,
    summary: String,
    expanded: Boolean,
    onToggle: () -> Unit,
    content: @Composable ColumnScope.() -> Unit,
) {
    HardwareCard {
        Row(
            Modifier.fillMaxWidth().clickable(onClick = onToggle).heightIn(min = LayoutTokens.TouchHeight),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, null)
            Spacer(Modifier.width(LayoutTokens.Space3))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Text(summary, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Icon(if (expanded) Icons.Default.ExpandLess else Icons.Default.ChevronRight, if (expanded) "Collapse $title" else "Open $title")
        }
        if (expanded) {
            HorizontalDivider(Modifier.padding(vertical = LayoutTokens.Space2))
            content()
        }
    }
}

@Composable
private fun ThemeChoiceRow(selected: Boolean, title: String, description: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = LayoutTokens.TouchHeight)
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(horizontal = LayoutTokens.Space1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Spacer(Modifier.width(LayoutTokens.Space2))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun PaletteChoiceRow(selected: Boolean, palette: AccentPalette, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = LayoutTokens.TouchHeight)
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(horizontal = LayoutTokens.Space1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Spacer(Modifier.width(LayoutTokens.Space2))
        Row(horizontalArrangement = Arrangement.spacedBy(LayoutTokens.Space1)) {
            PaletteSwatch(palette.leftSwatch, "Left accent sample")
            PaletteSwatch(palette.rightSwatch, "Right accent sample")
        }
        Spacer(Modifier.width(LayoutTokens.Space3))
        Column(Modifier.weight(1f)) {
            Text(palette.title, style = MaterialTheme.typography.bodyLarge)
            Text(palette.description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun PaletteSwatch(color: Color, description: String) {
    Box(
        Modifier
            .size(20.dp)
            .clip(CircleShape)
            .background(color)
            .semantics { contentDescription = description },
    )
}

@Composable
private fun ScreenColumn(title: String, subtitle: String, content: @Composable ColumnScope.() -> Unit) {
    val compact = LocalConfiguration.current.screenHeightDp < 600
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = if (compact) LayoutTokens.Space2 else LayoutTokens.Space5),
        verticalArrangement = Arrangement.spacedBy(if (compact) LayoutTokens.Space2 else LayoutTokens.Space4),
    ) {
        ScreenTitle(title, if (compact) "" else subtitle)
        content()
        Spacer(Modifier.height(if (compact) LayoutTokens.Space2 else LayoutTokens.Space5))
    }
}

@Composable
private fun ScreenFrame(title: String, subtitle: String, content: @Composable ColumnScope.() -> Unit) {
    val compact = LocalConfiguration.current.screenHeightDp < 600
    Column(Modifier.fillMaxSize().padding(vertical = if (compact) LayoutTokens.Space2 else LayoutTokens.Space5)) {
        ScreenTitle(title, if (compact) "" else subtitle)
        Spacer(Modifier.height(if (compact) LayoutTokens.Space2 else LayoutTokens.Space4))
        content()
    }
}

@Composable private fun ScreenTitle(title: String, subtitle: String) {
    Text(title, Modifier.semantics { heading() }, style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
    if (subtitle.isNotBlank()) Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun HardwareCard(modifier: Modifier = Modifier, container: Color = MaterialTheme.colorScheme.surfaceVariant, content: @Composable ColumnScope.() -> Unit) {
    Card(modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = container)) {
        Column(Modifier.fillMaxWidth().padding(LayoutTokens.Space4), content = content)
    }
}

@Composable private fun SectionHeading(icon: androidx.compose.ui.graphics.vector.ImageVector, title: String, modifier: Modifier = Modifier) {
    Row(modifier.semantics { heading() }, verticalAlignment = Alignment.CenterVertically) { Icon(icon, null); Spacer(Modifier.width(LayoutTokens.Space2)); Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold) }
}

@Composable private fun StatusPill(label: String, positive: Boolean) {
    Surface(shape = CircleShape, color = if (positive) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary) {
        Text(label, Modifier.padding(horizontal = LayoutTokens.Space3, vertical = LayoutTokens.Space1), color = if (positive) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSecondary, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable private fun MetadataLine(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = LayoutTokens.Space1)) {
        Text(label, Modifier.widthIn(min = 86.dp), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable private fun EmptyState(icon: androidx.compose.ui.graphics.vector.ImageVector, title: String, body: String, modifier: Modifier) {
    Box(modifier, contentAlignment = Alignment.Center) {
        Column(Modifier.widthIn(max = 320.dp).padding(LayoutTokens.Space5), horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(icon, null, Modifier.size(48.dp), tint = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(LayoutTokens.Space3)); Text(title, style = MaterialTheme.typography.titleLarge)
            Text(body, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
