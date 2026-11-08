package dev.picoswitch.companion.ui

import android.text.format.DateUtils
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import dev.picoswitch.companion.data.AdapterAlias
import dev.picoswitch.companion.data.AdapterId
import dev.picoswitch.companion.data.AdapterRecord
import dev.picoswitch.companion.data.AndroidBondState
import dev.picoswitch.companion.data.CompanionAssociationState
import dev.picoswitch.companion.data.PeerListing
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.model.PairingState
import dev.picoswitch.companion.model.PeerRole
import dev.picoswitch.companion.model.PeerTransport

// The Adapter page's Controllers and Adapters tabs
// (docs/architecture/companion-shell-layout.md, "What lives where"). Moved here
// from Settings: they are facts about the adapter, not this app's preferences.

/**
 * Controllers: pair a controller to the adapter, and the controllers it has
 * paired (connected, paired, recent).
 */
@Composable
internal fun AdapterControllersTab(ui: CompanionUiState, viewModel: CompanionViewModel) {
    var removeHistory by remember { mutableStateOf<PeerListing?>(null) }
    var forgetPeer by remember { mutableStateOf<PeerListing?>(null) }

    PairedControllersCard(
        ui = ui,
        viewModel = viewModel,
        onRemoveHistory = { removeHistory = it },
        onForgetPeer = { forgetPeer = it },
    )

    forgetPeer?.let { listing ->
        ConfirmDialog(
            onDismiss = { forgetPeer = null },
            title = "Forget ${listing.displayName}?",
            // Design section 53: say the consequence, not a factory-reset scare.
            // The extra sentence appears only for a controller that is connected
            // right now, because that is the only case with an immediate effect.
            body = buildString {
                append("It will need to be paired again before it can reconnect. ")
                if (listing.connected) append("This will disconnect it now. ")
                append("Its history entry is kept, so it will still appear under Recent.")
            },
            confirmLabel = "Forget",
            destructive = true,
            onConfirm = {
                viewModel.forgetPeer(listing.peerId, listing.displayName)
                forgetPeer = null
            },
        )
    }

    removeHistory?.let { listing ->
        ConfirmDialog(
            onDismiss = { removeHistory = null },
            title = "Remove ${listing.displayName} from history?",
            // Explicitly not a Bluetooth operation, and said so plainly: the
            // adapter already holds no key for this device. Confusing this with
            // "Forget pairing" is how a user believes a controller was unpaired
            // when nothing on the adapter changed.
            body = "This app forgets that ${listing.displayName} was ever connected to this adapter. " +
                "The adapter already has no saved pairing for it, so nothing on the adapter changes.",
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = {
                viewModel.removePeerFromHistory(listing.peerId)
                removeHistory = null
            },
        )
    }
}

/**
 * Adapters: add an adapter, and the adapters this phone remembers, with
 * rename, repair and remove.
 *
 * "Which adapters does this app know" and "what has this adapter paired with"
 * stay on separate tabs: removing an adapter is app-local, forgetting a
 * controller is not, and one list carrying both invited the user to read a
 * destructive action against the wrong list.
 */
@Composable
internal fun AdapterAdaptersTab(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onPairAdapter: () -> Unit,
    onRepairAdapter: (AdapterId?) -> Unit,
) {
    var renameTarget by remember { mutableStateOf<AdapterRecord?>(null) }
    var removeTarget by remember { mutableStateOf<AdapterRecord?>(null) }
    val actionEnabled = !ui.busy && !ui.relationshipStatus.attemptActive

    SectionCard {
        SettingsRow(
            title = "Add an adapter",
            supporting = "Double-tap the adapter's pairing button first.",
            leading = Icons.Default.AddLink,
            trailing = {
                Button(
                    onClick = onPairAdapter,
                    enabled = actionEnabled,
                    modifier = Modifier.widthIn(min = LayoutTokens.CommandMinWidth),
                ) { Text("Pair") }
            },
        )
    }

    // Not drawn while nothing is remembered: "Add an adapter" above already says
    // there is none, and an empty list plus a bond row for no adapter said it
    // twice more.
    if (ui.adapters.isNotEmpty()) SectionCard(title = "Remembered adapters", icon = Icons.Default.Link) {
        run {
            // Several adapters may be known; exactly one is active. The row says
            // which, because "connected" alone cannot: an adapter can be the
            // selected one and still be powered off.
            ui.adapters.forEach { record ->
                val active = record.id == ui.activeAdapterId
                SettingsRow(
                    title = if (ui.adapters.needsShortLabel(record)) {
                        "${record.displayName} • ${record.id.shortLabel}"
                    } else record.displayName,
                    supporting = adapterSupportingText(ui, record, active),
                    enabled = actionEnabled,
                    // Tapping the selected-but-disconnected row retries it. The
                    // coordinator refuses to tear down a healthy session, so
                    // this is safe on the connected row too.
                    onClick = { viewModel.selectAdapter(record.id) },
                    trailing = {
                        if (active && ui.connection.connected) {
                            StatusChip("Connected", tone = ChipTone.Positive)
                        } else if (record.repairRequired) {
                            // Repair targets the adapter whose pairing actually
                            // broke, never "whatever adapters this app owns".
                            TextButton(onClick = { onRepairAdapter(record.id) }, enabled = actionEnabled) {
                                Text("Repair")
                            }
                        } else if (active) {
                            StatusChip("Selected", tone = ChipTone.Neutral)
                        }
                        IconButton(
                            onClick = { renameTarget = record },
                            enabled = !ui.busy,
                        ) { Icon(Icons.Default.DriveFileRenameOutline, "Rename ${record.displayName}") }
                        IconButton(
                            onClick = { removeTarget = record },
                            enabled = !ui.busy,
                        ) { Icon(Icons.Default.Delete, "Remove ${record.displayName} from this app") }
                    },
                )
            }
        }
        HorizontalDivider()
        LabelValueRow(
            "Android Bluetooth pairing",
            when (ui.relationshipStatus.bond) {
                AndroidBondState.Bonded -> "Paired"
                AndroidBondState.Bonding -> "Pairing"
                AndroidBondState.None -> "Not paired"
                AndroidBondState.Unknown -> "Not checked"
            },
        )
        // The CompanionDeviceManager association row is deliberately NOT here.
        // Pairing goes through this app's own BLE scan, and nothing in the app
        // calls CompanionDeviceManager.associate() -- so for an adapter paired
        // the normal way the row would read "Not present" permanently, which
        // looks like a fault on a healthy adapter. It lives in Diagnostics.
    }

    renameTarget?.let { record ->
        AdapterRenameDialog(
            record = record,
            onDismiss = { renameTarget = null },
            onConfirm = { alias ->
                renameTarget = null
                viewModel.renameAdapter(record.id, alias)
            },
        )
    }

    removeTarget?.let { record ->
        ConfirmDialog(
            onDismiss = { removeTarget = null },
            title = "Remove ${record.displayName}?",
            // Say exactly what survives. This removes an app record; it is not a
            // Bluetooth operation, and claiming otherwise would leave the user
            // believing bonds were deleted that are still there.
            body = "This app forgets its name and saved details for this adapter. " +
                "Android Bluetooth pairing and the adapter's own stored pairings are not changed, " +
                "and your other adapters are unaffected.",
            confirmLabel = "Remove",
            destructive = true,
            onConfirm = { removeTarget = null; viewModel.removeAdapterFromApp(record.id) },
        )
    }
}

/**
 * What the adapter has paired with, as opposed to what is connected right now.
 *
 * Four separations the copy has to keep straight, because collapsing any of them
 * is how a user ends up acting on the wrong device:
 *
 *  * **Bonded is not connected.** A saved controller that is switched off is
 *    still saved, and belongs under Paired rather than nowhere.
 *  * **A controller is not the phone.** This phone appears in the adapter's
 *    inventory in up to two roles -- BLE management and Controller Link -- and
 *    neither belongs in a list of controllers.
 *  * **Unknown is not "none".** After the adapter reboots it can see its stored
 *    keys but cannot yet say whose they are. That is reported as unidentified,
 *    never guessed into a controller.
 *  * **Recent is not saved.** A row under Recent has no key on the adapter at
 *    all. Its only action removes an app-local memory, which is why it says
 *    "Remove from history" and not "Forget".
 *
 * **This card holds physical controllers and nothing else.** The management
 * phone, raw LE bond slots and unattributable security records are in
 * Diagnostics, where a reader is asking a different question.
 */
@Composable
private fun PairedControllersCard(
    ui: CompanionUiState,
    viewModel: CompanionViewModel,
    onRemoveHistory: (PeerListing) -> Unit,
    onForgetPeer: (PeerListing) -> Unit,
) {
    val inventory = ui.controllerInventory
    SectionCard(
        title = "Controllers",
        icon = Icons.Default.Bluetooth,
        trailing = {
            IconButton(
                onClick = viewModel::refreshPeers,
                enabled = ui.connection.connected && !ui.busy,
            ) { Icon(Icons.Default.Refresh, "Refresh paired controllers") }
        },
    ) {
        // Pair New Controller (design §31). Offered only over a live management
        // session, because that session IS the authority for the request.
        val pairing = ui.pairing
        if (ui.snapshot.capabilities.remotePairing == CapabilityState.Unsupported) {
            // Named, not hidden: the user should know the feature exists and
            // what would give it to them (design §87).
            InlineNotice("Update the adapter firmware to pair controllers from here.")
        } else if (pairing != null && pairing.active) {
            InlineNotice(
                if (pairing.state == PairingState.Connecting) {
                    "Connecting to a controller…"
                } else {
                    "Looking for a controller. Put it in pairing mode. " +
                        "${(pairing.remainingMillis / 1000).coerceAtLeast(0)}s left."
                },
                icon = Icons.Default.BluetoothSearching,
                tone = ChipTone.Attention,
            )
            OutlinedButton(
                onClick = viewModel::cancelControllerPairing,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Stop pairing") }
        } else {
            FilledTonalButton(
                onClick = viewModel::startControllerPairing,
                enabled = ui.connection.connected && !ui.busy,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Default.Add, null, Modifier.size(LayoutTokens.IconSize))
                Spacer(Modifier.width(LayoutTokens.Space2))
                Text("Pair new controller")
            }
            // Says where the controller has to be, since the adapter may be
            // behind a TV and the user is holding neither.
            Text(
                "Opens pairing on the adapter for 30 seconds. You do not need to touch it.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        HorizontalDivider()

        when {
            ui.snapshot.capabilities.peers == CapabilityState.Unsupported ->
                InlineNotice("Update the adapter firmware to see its paired controllers here.")
            // Recent survives a disconnect on purpose: it is this app's memory,
            // not the adapter's, and it is the only thing there is to show while
            // the adapter is away. Everything else needs the adapter present.
            !ui.connection.connected && inventory.recent.isEmpty() ->
                InlineNotice("Connect to the adapter to see what it has paired.")
            !inventory.hasControllers && ui.snapshot.capabilities.peers == CapabilityState.Unknown ->
                InlineNotice("Tap refresh to read the adapter's paired controllers.")
            !inventory.hasControllers && inventory.hasDiagnosticPeers ->
                // The adapter holds records, but none of them is a controller
                // pairing. Saying "no controllers" alone would be true and
                // unhelpful; this says where the rows it does hold went.
                InlineNotice(
                    "No paired controllers. The Bluetooth records this adapter holds are this " +
                        "phone's own management relationship; it is shown in Diagnostics.",
                )
            !inventory.hasControllers ->
                InlineNotice("This adapter has no paired controllers.")
            else -> {
                // Forget is offered on both, because a controller that is
                // connected right now is exactly the one a user most often wants
                // rid of. The confirmation says it will be disconnected.
                // Not offered at all on firmware that cannot perform it: a
                // Forget button that answers `unknown command` is worse than an
                // absent one, and the list itself still works (design §87).
                val canForget = ui.snapshot.capabilities.peerForget != CapabilityState.Unsupported
                val forgetAction: @Composable (PeerListing) -> (@Composable RowScope.() -> Unit)? =
                    { listing ->
                        if (!canForget) null else {
                            {
                                IconButton(
                                    onClick = { onForgetPeer(listing) },
                                    enabled = ui.connection.connected && !ui.busy,
                                ) { Icon(Icons.Default.LinkOff, "Forget ${listing.displayName}") }
                            }
                        }
                    }
                if (inventory.connected.isNotEmpty()) {
                    SubsectionLabel("Connected")
                    inventory.connected.forEach { PeerRow(it, trailing = forgetAction(it)) }
                }
                if (inventory.paired.isNotEmpty()) {
                    if (inventory.connected.isNotEmpty()) HorizontalDivider()
                    SubsectionLabel("Paired")
                    inventory.paired.forEach { PeerRow(it, trailing = forgetAction(it)) }
                }
                if (inventory.recent.isNotEmpty()) {
                    HorizontalDivider()
                    SubsectionLabel("Recent")
                    inventory.recent.forEach { listing ->
                        PeerRow(
                            listing,
                            trailing = {
                                IconButton(
                                    onClick = { onRemoveHistory(listing) },
                                    enabled = !ui.busy,
                                ) {
                                    // Deliberately not the icon a bond removal
                                    // uses. One trash can for both meanings is
                                    // exactly what the design forbids.
                                    Icon(
                                        Icons.Default.HistoryToggleOff,
                                        "Remove ${listing.displayName} from history",
                                    )
                                }
                            },
                        )
                    }
                }
            }
        }

    }
}

@Composable
private fun PeerRow(
    listing: PeerListing,
    trailing: @Composable (RowScope.() -> Unit)? = null,
) {
    SettingsRow(
        title = listing.displayName,
        supporting = peerSupportingText(listing),
        // Read-only rows. Nothing here is tappable until selective forget
        // exists, and a row that highlights but does nothing reads as broken.
        enabled = trailing != null,
        trailing = trailing ?: { PeerStateChip(listing) },
    )
}

/**
 * The peer's state in the user's vocabulary.
 *
 * There is no manual save action anywhere in this product, so "Saved" was never
 * a state the user could reach or reason about. These are the four states that
 * actually exist:
 *
 *  * **Connected** — linked now, with a durable credential.
 *  * **Pairing** — linked now, credential not committed yet. A Classic
 *    controller genuinely passes through this between the ACL coming up and its
 *    link key being written, and the row must not imply the user has to do
 *    something about it.
 *  * **Paired** — the adapter holds a credential; the controller is elsewhere.
 *  * **Not paired** — remembered by this app only.
 */
@Composable
private fun PeerStateChip(listing: PeerListing) {
    when {
        listing.connected && listing.bonded -> StatusChip("Connected", tone = ChipTone.Positive)
        listing.connected -> StatusChip("Pairing", tone = ChipTone.Attention)
        listing.bonded -> StatusChip("Paired", tone = ChipTone.Neutral)
        else -> StatusChip("Not paired", tone = ChipTone.Neutral)
    }
}

/**
 * One line describing a peer, sourced honestly.
 *
 * Anything the adapter cannot currently prove is attributed to this app's
 * memory in the text itself. The protocol requires that an `unknown` role be
 * rendered as unidentified rather than promoted, and remembering what the
 * adapter once proved is not the same claim as asserting it now.
 */
private fun peerSupportingText(listing: PeerListing): String {
    // "Controller" whether the adapter can prove it right now or this app
    // remembers it, because the difference is ours and not the user's.
    //
    // It also is not a difference between the CONTROLLERS. The adapter's only
    // persistent role hint is the single `JPLC` last-connected record, which
    // holds one BLE peer -- so a BLE controller keeps a live `controller` role
    // while disconnected purely by being the reconnect target, and a Classic
    // controller never can. Two equally paired controllers were being described
    // differently because of which transport they happen to use.
    val role = when {
        listing.role == PeerRole.ManagementCompanion -> "This phone — management"
        listing.role == PeerRole.ControllerLink -> "This phone — Controller Link"
        listing.rememberedRole == PeerRole.ManagementCompanion -> "This phone — management"
        listing.rememberedRole == PeerRole.ControllerLink -> "This phone — Controller Link"
        listing.role == PeerRole.PhysicalController ||
            listing.rememberedRole == PeerRole.PhysicalController -> "Controller"
        // Deliberately not "unrecognised device": the adapter holds a credential
        // for it, so the honest statement is that it cannot say what it is YET.
        // Role is live evidence, and after a reboot every paired controller
        // reads like this until it reconnects.
        else -> "Not yet identified"
    }
    val transports = when {
        // A row that exists only in history describes a device the adapter no
        // longer stores anything for, so it has no transport to report.
        listing.historyOnly -> null
        listing.transports.size > 1 -> "Bluetooth Classic + LE"
        listing.transports.contains(PeerTransport.Classic) -> "Bluetooth Classic"
        listing.transports.contains(PeerTransport.Le) -> "Bluetooth LE"
        // Connected with no credential yet: mid-pairing, not a state the user
        // has to act on, so it is named for what it is rather than as an absence.
        listing.connected -> "Completing pairing"
        else -> null
    }
    val lastConnected = listing.lastConnectedAtMillis
        ?.takeIf { !listing.connected }
        ?.let { "Last connected ${relativeTime(it)}" }
    return listOfNotNull(role, transports, lastConnected).joinToString(" · ")
}

/**
 * Human phrasing for a timestamp this app recorded itself.
 *
 * Only ever applied to app-side history. The adapter has no real-time clock and
 * no time sync, so it never supplies a wall-clock time and none is invented for
 * it; every value formatted here was stamped on this phone.
 */
private fun relativeTime(millis: Long): String = DateUtils.getRelativeTimeSpanString(
    millis,
    System.currentTimeMillis(),
    DateUtils.MINUTE_IN_MILLIS,
    DateUtils.FORMAT_ABBREV_RELATIVE,
).toString()

/**
 * Two rows reading "Living Room" are not a list.
 *
 * Duplicate aliases are allowed on purpose — the user may genuinely own two
 * adapters they think of the same way — so the disambiguation is additive
 * rather than a validation error.
 */
private fun List<AdapterRecord>.needsShortLabel(record: AdapterRecord): Boolean =
    count { it.displayName.equals(record.displayName, ignoreCase = true) } > 1

/**
 * What a row can honestly say about an adapter that may not be connected.
 *
 * Everything here except the live connection state is cache from the last
 * verified session, so it is phrased as history ("Last connected") rather than
 * as current truth.
 */
private fun adapterSupportingText(
    ui: CompanionUiState,
    record: AdapterRecord,
    active: Boolean,
): String {
    if (active && ui.connection.connected) {
        return listOfNotNull(
            record.lastFirmwareVersion?.let { "Firmware $it" },
            record.lastPersonality,
        ).joinToString(" · ").ifBlank { "Connected" }
    }
    // A failed switch is reported against the adapter that was chosen, and says
    // so plainly rather than pretending the previous adapter is still the one.
    if (active && ui.adapterSwitchFailure != null) return "Selected, not connected · tap to retry"
    if (record.repairRequired) return "Needs to be paired with this phone again"
    return when (ui.associationStates[record.id] ?: CompanionAssociationState.Unknown) {
        CompanionAssociationState.Ambiguous -> "Duplicate companion records; repair to resolve"
        else -> if (record.lastConnectedAtMillis != null) "Saved" else "Not connected yet"
    }
}

/** Local rename. The alias never leaves this phone and never becomes Bluetooth identity. */
@Composable
private fun AdapterRenameDialog(
    record: AdapterRecord,
    onDismiss: () -> Unit,
    onConfirm: (String?) -> Unit,
) {
    var text by rememberSaveable(record.id.value) { mutableStateOf(record.userAlias.orEmpty()) }
    PicoDialog(
        onDismiss = onDismiss,
        title = "Adapter name",
        confirmLabel = "Save",
        onConfirm = { onConfirm(text) },
    ) {
        OutlinedTextField(
            value = text,
            onValueChange = { text = it.take(AdapterAlias.MAX_LENGTH) },
            singleLine = true,
            label = { Text("Name on this phone") },
            placeholder = { Text(record.lastKnownName) },
            supportingText = { Text("Leave empty to use ${record.lastKnownName} • ${record.id.shortLabel}") },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}
