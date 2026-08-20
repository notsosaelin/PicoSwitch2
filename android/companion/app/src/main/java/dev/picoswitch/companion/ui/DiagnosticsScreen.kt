package dev.picoswitch.companion.ui

import android.content.ClipData
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ClipEntry
import androidx.compose.ui.platform.LocalClipboard
import androidx.compose.ui.unit.dp
import dev.picoswitch.companion.BuildConfig
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.model.title
import kotlinx.coroutines.launch

/**
 * The technical view.
 *
 * Allowed to be dense and to use protocol vocabulary -- this is where a fault
 * is localized -- but not allowed to be unstructured. Values are grouped by the
 * layer they describe so the first question a cross-layer failure asks ("which
 * side disagrees?") is answerable by reading down one column.
 *
 * Everything here is read-only except the two development gates at the bottom,
 * which exist to reproduce states during validation and are labelled with what
 * they cost.
 */
@Composable
fun DiagnosticsScreen(ui: CompanionUiState, viewModel: CompanionViewModel, onExportDiagnostics: () -> Unit) {
    val clipboard = LocalClipboard.current
    val scope = rememberCoroutineScope()
    var managementOpen by rememberSaveable { mutableStateOf(false) }

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val twoColumn = twoColumnLayout(maxWidth)
        val gap = if (LocalShortWindow.current) LayoutTokens.Space3 else LayoutTokens.Space4

        val identity: @Composable () -> Unit = {
            SectionCard(title = "Identity", icon = Icons.Default.Memory) {
                DetailList {
                LabelValueRow("App", BuildConfig.VERSION_NAME, copyable = true)
                LabelValueRow("Firmware", ui.snapshot.firmware.version.ifBlank { "—" })
                LabelValueRow(
                    "Firmware build",
                    ui.snapshot.firmware.build.ifBlank { "unreported" },
                    monospace = true,
                    copyable = ui.snapshot.firmware.build.isNotBlank(),
                )
                LabelValueRow("Product", ui.snapshot.firmware.product.ifBlank { "—" })
                LabelValueRow("Personality", ui.snapshot.personality.current.title)
                LabelValueRow(
                    "Bridge contract",
                    "app ${dev.picoswitch.bridge.protocol.BridgeContract.VERSION} · firmware ${ui.snapshot.firmware.bridgeContract}",
                    monospace = true,
                )
                LabelValueRow("Compatibility", ui.bridgeCompatibility.summary)
                }
            }
        }

        val link: @Composable () -> Unit = {
            SectionCard(title = "Management link", icon = Icons.Default.BluetoothConnected) {
                DetailList {
                LabelValueRow("Phase", ui.connection.phase.name)
                LabelValueRow(
                    "Address",
                    ui.connection.address ?: ui.adapterRelationship?.address ?: "—",
                    monospace = true,
                    copyable = ui.connection.address != null,
                )
                LabelValueRow("Protocol", "BLE GATT · newline JSON v${BuildConfig.MGMT_PROTOCOL_VERSION}")
                LabelValueRow("Wireless management", if (ui.snapshot.managementEnabled == true) "On" else "Off")
                LabelValueRow("Last command", ui.diagnosticSummary.lastCommand, monospace = true)
                LabelValueRow("Last result", ui.diagnosticSummary.lastResult, monospace = true)
                LabelValueRow("Last error", ui.diagnosticSummary.lastError)
                }
            }
        }

        val platform: @Composable () -> Unit = {
            SectionCard(title = "Android platform", icon = Icons.Default.PhoneAndroid) {
                DetailList {
                LabelValueRow(
                    "Bluetooth",
                    "available ${ui.platform.bluetoothAvailable} · enabled ${ui.platform.bluetoothEnabled}",
                )
                LabelValueRow(
                    "Permissions",
                    "scan ${ui.platform.scanPermission} · connect ${ui.platform.connectPermission}",
                )
                LabelValueRow("Companion manager", ui.platform.companionDeviceManager.toString())
                LabelValueRow("NFC reader", if (ui.nfcScan.phase == NfcScanPhaseUnavailable) "Unavailable" else "Available")
                }
            }
        }

        val bridge: @Composable () -> Unit = {
            SectionCard(title = "Controller bridge", icon = Icons.Default.BluetoothAudio) {
                DetailList {
                LabelValueRow("Phase", "${ui.bridge.phase.name} · registered ${ui.bridge.registered}")
                LabelValueRow("Saved host", if (viewModel.pairedControllerHosts().isEmpty()) "No" else "Yes")
                LabelValueRow("Reports", ui.bridge.reportCount.toString())
                // Platform, normalized and protocol are shown separately on
                // purpose: which layer disagrees with the next one is what
                // localizes a motion or rumble fault without a rebuild.
                LabelValueRow(
                    "Capabilities",
                    ui.bridge.capabilities.let {
                        "sticks ${it.analogSticks} · triggers ${it.analogTriggers} · motion ${it.motion} · motors ${it.rumbleMotors}"
                    },
                )
                LabelValueRow(
                    "Motion frame",
                    if (ui.bridge.motion.frameRotationMeasured) "${ui.bridge.motion.frameRotationDegrees}°"
                    else "unreadable; assuming 0°",
                )
                LabelValueRow("Motion platform", ui.bridge.motion.platformRaw, monospace = true)
                LabelValueRow("Motion canonical", ui.bridge.motion.canonical, monospace = true)
                LabelValueRow("Output route", ui.bridge.output.route)
                }
                ui.bridge.output.warning?.let { InlineNotice(it, tone = ChipTone.Error) }
            }
        }

        val kbm: @Composable () -> Unit = {
            SectionCard(title = "Keyboard & mouse", icon = Icons.Default.Keyboard) {
                val status = ui.kbm.status
                DetailList {
                LabelValueRow("Availability", ui.kbm.available.name)
                LabelValueRow("Mode", "${status.mode.wire} (override ${status.modeOverride.wire})", monospace = true)
                LabelValueRow("Profile", status.profile.wire, monospace = true)
                LabelValueRow("Roles", "keyboard ${status.keyboardConnected} · mouse ${status.mouseConnected}")
                // The connection indices are how a role is tied back to a named
                // peer in `input sources`; this is the only place they are shown.
                LabelValueRow(
                    "Role conn",
                    "keyboard ${status.keyboardConn} · mouse ${status.mouseConn}",
                    monospace = true,
                )
                LabelValueRow("Native pointer", status.nativeMouseOutput.toString())
                LabelValueRow("Reports", "kb ${status.keyboardReports} · mouse ${status.mouseReports}", monospace = true)
                // The rejection tallies are the arbitration answer to "input
                // arrived but nothing happened", which is why they are recorded
                // at all; they are meaningless on the product page.
                LabelValueRow(
                    "Rejected",
                    "mode ${status.rejectedMode} · dup ${status.rejectedDuplicate} · owner ${status.rejectedNotOwner}",
                    monospace = true,
                )
                LabelValueRow("Rollover", status.rollover.toString(), monospace = true)
                LabelValueRow("Role losses", status.roleLosses.toString(), monospace = true)
                LabelValueRow("Map generation", status.mapGeneration.toString(), monospace = true)
                LabelValueRow("Publishes", status.publishes.toString(), monospace = true)
                LabelValueRow("Stick recenters", status.recenters.toString(), monospace = true)
                LabelValueRow("Unsaved changes", ui.kbm.dirty.toString())
                }
            }
        }

        val adapterState: @Composable () -> Unit = {
            SectionCard(title = "Adapter state", icon = Icons.Default.Cable) {
                val capabilities = ui.snapshot.capabilities
                DetailList {
                LabelValueRow(
                    "Capabilities",
                    listOf(
                        "core ${capabilities.core.short()}",
                        "personality ${capabilities.personality.short()}",
                        "amiibo ${capabilities.amiibo.short()}",
                        "bonds ${capabilities.bonds.short()}",
                        "wake ${capabilities.wake.short()}",
                        "input ${capabilities.activeInput.short()}",
                    ).joinToString(" · "),
                    monospace = true,
                )
                LabelValueRow(
                    "Active input",
                    "id ${ui.snapshot.input.activeId} · pending ${ui.snapshot.input.pendingId} · transitions ${ui.snapshot.input.transitions}",
                    monospace = true,
                )
                LabelValueRow(
                    "Amiibo",
                    "loaded ${ui.snapshot.amiibo.loaded} · v3 ${ui.snapshot.amiibo.v3Loaded} · dirty ${ui.snapshot.amiibo.dirty} · gen ${ui.snapshot.amiibo.generation}",
                    monospace = true,
                )
                LabelValueRow(
                    "Bonds",
                    "${ui.snapshot.bonds.size}${ui.snapshot.bondsTotal?.let { " of $it" } ?: ""} · complete ${ui.snapshot.bondsComplete}",
                    monospace = true,
                )
                }
                if (ui.excludedSources.isNotEmpty()) {
                    HorizontalDivider()
                    SubsectionLabel("Ignored inputs")
                    DetailList {
                    // Listed with their reason so a wrongly-hidden device is
                    // identifiable from the field rather than guessed at.
                    ui.excludedSources.forEach { source ->
                        LabelValueRow(
                            source.name.ifBlank { "Unnamed" },
                            "VID %04X · PID %04X — %s".format(source.vendorId, source.productId, source.reason),
                            monospace = true,
                        )
                    }
                    }
                }
            }
        }

        val live: @Composable () -> Unit = {
            SectionCard(title = "Live input", icon = Icons.Default.MonitorHeart) {
                LiveInputBlock(ui.controllerState)
            }
        }

        val tools: @Composable () -> Unit = {
            SectionCard(title = "Tools", icon = Icons.Default.Build) {
                SettingsRow(
                    title = "Share a diagnostics report",
                    supporting = "Writes a text snapshot of everything on this page",
                    leading = Icons.Default.Share,
                    onClick = onExportDiagnostics,
                    trailing = { Icon(Icons.Default.ChevronRight, null) },
                )
                if (ui.identityRefreshPending) {
                    SettingsRow(
                        title = "Mark identity refresh complete",
                        supporting = "Clears the pending re-enumeration reminder without re-enumerating",
                        onClick = viewModel::clearIdentityRefreshPending,
                    )
                }
                // Development-only gate. Normal product behaviour is
                // management-on at boot, so this is deliberately not a user
                // setting: it exists to reproduce the disabled state during
                // validation, and turning it off ends the session issuing the
                // command.
                if (ui.connection.connected &&
                    ui.snapshot.capabilities.managementGate == CapabilityState.Available
                ) {
                    SettingsRow(
                        title = if (ui.snapshot.managementEnabled == true) {
                            "Disable wireless management"
                        } else "Enable wireless management",
                        supporting = if (ui.snapshot.managementEnabled == true) {
                            "Ends this session immediately"
                        } else "Restores the management transport",
                        enabled = !ui.busy,
                        onClick = { managementOpen = true },
                    )
                }
            }
        }

        Column(Modifier.fillMaxSize()) {
            ScreenHeader("Diagnostics", subtitle = "") {
                IconButton(
                    onClick = {
                        scope.launch {
                            clipboard.setClipEntry(
                                ClipEntry(ClipData.newPlainText("PicoSwitch2 diagnostics", diagnosticsSummary(ui))),
                            )
                        }
                    },
                ) { Icon(Icons.Default.ContentCopy, "Copy a diagnostics summary") }
                IconButton(onClick = viewModel::closeOverlay) { Icon(Icons.Default.Close, "Close diagnostics") }
            }
            Spacer(Modifier.height(LayoutTokens.Space3))
            if (twoColumn) {
                Row(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(gap),
                ) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                        identity(); link(); platform(); tools()
                    }
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                        bridge(); kbm(); adapterState(); live()
                    }
                }
            } else {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(gap),
                ) {
                    identity(); link(); bridge(); kbm(); adapterState(); platform(); live(); tools()
                    Spacer(Modifier.height(LayoutTokens.Space5))
                }
            }
        }
    }

    if (managementOpen) ConfirmDialog(
        onDismiss = { managementOpen = false },
        title = if (ui.snapshot.managementEnabled == true) "Disable wireless management?" else "Enable wireless management?",
        body = if (ui.snapshot.managementEnabled == true) {
            "This is the transport this app is using. Disabling it ends the session at once and the adapter will not accept another until it reboots or the gate is re-enabled over USB."
        } else "Re-enables the management transport for this boot.",
        confirmLabel = if (ui.snapshot.managementEnabled == true) "Disable" else "Enable",
        destructive = ui.snapshot.managementEnabled == true,
        onConfirm = {
            managementOpen = false
            viewModel.setManagement(ui.snapshot.managementEnabled != true)
        },
    )
}

private fun CapabilityState.short(): String = when (this) {
    CapabilityState.Available -> "ok"
    CapabilityState.Unsupported -> "no"
    CapabilityState.Unknown -> "?"
}

/** The one-tap summary a user is asked for when reporting a problem. */
private fun diagnosticsSummary(ui: CompanionUiState): String = buildString {
    appendLine("PicoSwitch2 companion diagnostics")
    appendLine("app=${BuildConfig.VERSION_NAME}")
    appendLine("firmware=${ui.snapshot.firmware.version.ifBlank { "-" }} build=${ui.snapshot.firmware.build.ifBlank { "-" }}")
    appendLine("bridgeContract app=${dev.picoswitch.bridge.protocol.BridgeContract.VERSION} firmware=${ui.snapshot.firmware.bridgeContract}")
    appendLine("personality=${ui.snapshot.personality.current.wireName}")
    appendLine("connection=${ui.connection.phase.name} address=${ui.connection.address ?: "-"}")
    appendLine("bridge=${ui.bridge.phase.name} reports=${ui.bridge.reportCount}")
    appendLine("kbm=${ui.kbm.available.name} mode=${ui.kbm.status.mode.wire} profile=${ui.kbm.status.profile.wire}")
    appendLine("kbm roles: keyboard=${ui.kbm.status.keyboardConnected} mouse=${ui.kbm.status.mouseConnected}")
    appendLine("lastCommand=${ui.diagnosticSummary.lastCommand}")
    appendLine("lastResult=${ui.diagnosticSummary.lastResult}")
    appendLine("lastError=${ui.diagnosticSummary.lastError}")
}

private val NfcScanPhaseUnavailable = dev.picoswitch.companion.model.NfcScanPhase.Unavailable
