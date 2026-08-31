package dev.picoswitch.companion

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.companion.CompanionDeviceManager
import android.content.Intent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.IntentFilter
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.FileProvider
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.companion.data.AdapterConnectReason
import dev.picoswitch.companion.data.AdapterId
import dev.picoswitch.companion.data.AndroidBondState
import dev.picoswitch.companion.data.SystemCompanionAssociation
import dev.picoswitch.companion.nfc.AndroidNtag215Reader
import dev.picoswitch.companion.ui.CompanionApp
import dev.picoswitch.companion.ui.CompanionViewModel
import dev.picoswitch.companion.ui.applyEdgeToEdgeChrome

class MainActivity : ComponentActivity() {
    private val viewModel: CompanionViewModel by viewModels()
    private val nfcReader by lazy {
        AndroidNtag215Reader(
            activity = this,
            onTagDetected = viewModel::nfcScanReading,
            onAccepted = viewModel::importNfcAmiibo,
            onRejected = viewModel::nfcScanRejected,
            onReaderError = viewModel::nfcReaderError,
        )
    }
    private enum class ManagementAction { Pair, Reconnect }
    private var pendingManagementAction = ManagementAction.Reconnect
    private var bondReceiverRegistered = false
    private val bondReceiver = object : BroadcastReceiver() {
        @SuppressLint("MissingPermission")
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            @Suppress("DEPRECATION")
            val device = (if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            } else intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE) as? BluetoothDevice) ?: return
            val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, device.bondState).toProductBondState()
            viewModel.adapterBondChanged(device.address, state)
        }
    }
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = viewModel.refreshSources()
        override fun onInputDeviceRemoved(deviceId: Int) = viewModel.refreshSources()
        override fun onInputDeviceChanged(deviceId: Int) = viewModel.refreshSources()
    }

    private val managementPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) {
            if (pendingManagementAction == ManagementAction.Pair) pairAdapter()
            else viewModel.reconnectKnownAdapter(AdapterConnectReason.Manual)
        }
        else Toast.makeText(this, "Nearby devices permission is required to find PicoSwitch2", Toast.LENGTH_LONG).show()
    }

    private val controllerPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) viewModel.acquireControllerBridge()
        else Toast.makeText(this, "Nearby devices permission is required for the controller bridge", Toast.LENGTH_LONG).show()
    }

    // Multi-select, and dumps and ZIPs together: which kind of file the user has
    // is the app's problem to work out, not theirs to declare before picking.
    private val importAmiibo = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments(),
    ) { uris ->
        if (uris.isNotEmpty()) viewModel.importAmiiboFiles(uris)
    }

    private val importAmiiboFolder = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree(),
    ) { uri ->
        uri?.let(viewModel::importAmiiboFolder)
    }

    private val exportAmiiboArchive = registerForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip"),
    ) { uri ->
        uri?.let(viewModel::exportAmiiboArchive)
    }

    private val importAmiiboKeys = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { viewModel.importAmiiboKeys(it) }
    }

    /**
     * One picture for the on-screen controller's background.
     *
     * The system photo picker, image-only. No storage or media permission is
     * requested and none is needed: the picker hands back a single grant, the app
     * immediately copies the image into its own files, and the grant is never
     * relied on again. AndroidX falls back to the document picker on devices with
     * no photo picker, which behaves identically for this purpose.
     */
    private val pickTouchBackground = registerForActivityResult(
        ActivityResultContracts.PickVisualMedia(),
    ) { uri ->
        uri?.let(viewModel::adoptTouchBackground)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Before setContent: the window's inset behaviour has to be settled
        // before the first composition reads WindowInsets.safeDrawing.
        applyEdgeToEdgeChrome()
        viewModel.setNfcReaderAvailable(nfcReader.isAvailable)
        setContent {
            CompanionApp(
                viewModel = viewModel,
                onConnectAdapter = ::requestAdapterConnection,
                onPairAdapter = ::requestNewAdapterPairing,
                onRepairAdapter = ::requestAdapterRepair,
                onImportAmiibo = { importAmiibo.launch(arrayOf("*/*")) },
                onImportAmiiboFolder = { importAmiiboFolder.launch(null) },
                onExportAmiiboArchive = { exportAmiiboArchive.launch("PicoSwitch2-Amiibo-Library.zip") },
                onScanAmiibo = ::requestNfcScan,
                onImportAmiiboKeys = { importAmiiboKeys.launch(arrayOf("application/octet-stream", "application/*", "*/*")) },
                onPrepareController = ::requestControllerBridge,
                onOpenTouchGamepad = ::openTouchGamepad,
                onPickTouchBackground = {
                    pickTouchBackground.launch(
                        androidx.activity.result.PickVisualMediaRequest(
                            ActivityResultContracts.PickVisualMedia.ImageOnly,
                        ),
                    )
                },
                onExportDiagnostics = ::shareDiagnostics,
            )
        }
        viewModel.recordLifecycle("created")
    }

    fun requestAdapterConnection() {
        pendingManagementAction = if (viewModel.ui.value.adapterRelationship == null) ManagementAction.Pair else ManagementAction.Reconnect
        requestManagementPermissions()
    }

    private fun requestNewAdapterPairing() {
        pendingManagementAction = ManagementAction.Pair
        requestManagementPermissions()
    }

    private fun requestManagementPermissions() {
        val needed = if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        managementPermissions.launch(needed)
    }

    @SuppressLint("RestrictedApi")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        return viewModel.inputBackend.onKey(event) || super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        return viewModel.inputBackend.onMotion(event) || super.dispatchGenericMotionEvent(event)
    }

    override fun onPause() {
        nfcReader.disable()
        viewModel.nfcScanDisarmed()
        viewModel.cancelAutomaticControllerResume()
        viewModel.neutralizeController()
        viewModel.recordLifecycle("paused; controller neutralized")
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        super.onPause()
    }

    override fun onStart() {
        super.onStart()
        if (!bondReceiverRegistered) {
            val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            // Bluetooth broadcasts originate from the privileged Bluetooth package rather than
            // this app. NOT_EXPORTED silently drops them on affected Android 13 builds.
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(bondReceiver, filter, RECEIVER_EXPORTED)
            else @Suppress("DEPRECATION") registerReceiver(bondReceiver, filter)
            bondReceiverRegistered = true
        }
        getSystemService(InputManager::class.java)?.registerInputDeviceListener(inputDeviceListener, null)
        if (hasManagementPermissions()) {
            restoreSystemAssociation()
            viewModel.resumePendingAdapterBond()
            viewModel.beginForegroundSession()
        }
    }

    override fun onStop() {
        getSystemService(InputManager::class.java)?.unregisterInputDeviceListener(inputDeviceListener)
        if (bondReceiverRegistered) {
            unregisterReceiver(bondReceiver)
            bondReceiverRegistered = false
        }
        super.onStop()
    }

    override fun onDestroy() {
        nfcReader.close()
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        // Controller mode is intentionally foreground-only. Keep the handheld display awake
        // while the app is active so Android cannot silently suspend the input surface.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        viewModel.recordLifecycle("resumed")
        viewModel.refreshSources()
        if (hasManagementPermissions()) viewModel.requestAutomaticControllerResume()
    }

    private fun requestControllerBridge() {
        if (Build.VERSION.SDK_INT >= 31) controllerPermissions.launch(arrayOf(Manifest.permission.BLUETOOTH_CONNECT))
        else viewModel.acquireControllerBridge()
    }

    /**
     * Open the on-screen controller, and get the link going if it is not already.
     *
     * The surface opens either way. Waiting for the link before showing anything
     * would leave the user looking at the previous page with no explanation,
     * whereas the controller with a "connecting" strip is honest and is already
     * the state it will be in after any later disconnect.
     */
    private fun openTouchGamepad() {
        viewModel.enterTouchGamepad()
        val phase = viewModel.ui.value.bridge.phase
        if (phase !in ACTIVE_BRIDGE_PHASES) requestControllerBridge()
    }

    private fun requestNfcScan() {
        if (!nfcReader.isAvailable) {
            viewModel.nfcReaderUnavailable("This phone does not expose an NFC-A reader.")
            return
        }
        viewModel.armNfcScan()
        nfcReader.arm()
    }

    private fun pairAdapter() {
        viewModel.beginAdapterPairing()
    }

    @SuppressLint("MissingPermission")
    private fun remoteDevice(address: String): BluetoothDevice? = runCatching {
        getSystemService(BluetoothManager::class.java).adapter.getRemoteDevice(address)
    }.getOrNull()

    private fun hasManagementPermissions(): Boolean = if (Build.VERSION.SDK_INT >= 31) {
        checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == android.content.pm.PackageManager.PERMISSION_GRANTED &&
            checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == android.content.pm.PackageManager.PERMISSION_GRANTED
    } else {
        checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == android.content.pm.PackageManager.PERMISSION_GRANTED
    }

    @SuppressLint("MissingPermission")
    private fun restoreSystemAssociation() {
        val manager = getSystemService(CompanionDeviceManager::class.java) ?: return
        val associations = runCatching {
            if (Build.VERSION.SDK_INT >= 33) {
                manager.myAssociations.mapNotNull { association ->
                    association.deviceMacAddress?.toString()?.let { address ->
                        SystemCompanionAssociation(association.id, address, association.displayName?.toString())
                    }
                }
            } else {
                @Suppress("DEPRECATION")
                manager.associations.map { address ->
                    SystemCompanionAssociation(null, address, remoteDevice(address)?.name)
                }
            }
        }.onFailure(viewModel::systemAssociationQueryFailed).getOrNull() ?: return
        viewModel.reconcileAdapterRelationships(associations)
    }

    private fun requestAdapterRepair(adapterId: AdapterId?) {
        viewModel.prepareRepairPairing(adapterId) { needsAndroidSettings ->
            if (needsAndroidSettings) {
                Toast.makeText(
                    this,
                    "Remove PicoSwitch2 from Android's paired devices, then return and tap Pair Adapter.",
                    Toast.LENGTH_LONG,
                ).show()
                startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
            } else {
                requestNewAdapterPairing()
            }
        }
    }

    private companion object {
        /** Phases in which the controller link is already being established or is live. */
        val ACTIVE_BRIDGE_PHASES = setOf(
            BridgeLinkPhase.Preparing,
            BridgeLinkPhase.Registering,
            BridgeLinkPhase.Connecting,
            BridgeLinkPhase.Playing,
        )
    }

    private fun Int.toProductBondState(): AndroidBondState = when (this) {
        BluetoothDevice.BOND_NONE -> AndroidBondState.None
        BluetoothDevice.BOND_BONDING -> AndroidBondState.Bonding
        BluetoothDevice.BOND_BONDED -> AndroidBondState.Bonded
        else -> AndroidBondState.Unknown
    }

    private fun shareDiagnostics() {
        runCatching {
            val file = viewModel.exportDiagnostics()
            val uri = FileProvider.getUriForFile(this, "$packageName.files", file)
            val intent = Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_STREAM, uri)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(Intent.createChooser(intent, "Share PicoSwitch diagnostics"))
        }.onFailure {
            Toast.makeText(this, it.message ?: "Could not export diagnostics", Toast.LENGTH_LONG).show()
        }
    }
}
