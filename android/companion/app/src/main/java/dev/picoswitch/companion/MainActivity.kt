package dev.picoswitch.companion

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.Intent
import android.content.IntentSender
import android.content.BroadcastReceiver
import android.content.Context
import android.content.IntentFilter
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.FileProvider
import dev.picoswitch.companion.bluetooth.AdapterBluetoothIdentity
import dev.picoswitch.companion.ui.CompanionApp
import dev.picoswitch.companion.ui.CompanionViewModel
import java.util.regex.Pattern

class MainActivity : ComponentActivity() {
    private val viewModel: CompanionViewModel by viewModels()
    private enum class ManagementAction { Pair, Reconnect }
    private var pendingManagementAction = ManagementAction.Reconnect
    private var pendingBondDevice: BluetoothDevice? = null
    private var bondReceiverRegistered = false
    private val bondReceiver = object : BroadcastReceiver() {
        @SuppressLint("MissingPermission")
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            @Suppress("DEPRECATION")
            val device = (if (Build.VERSION.SDK_INT >= 33) {
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            } else intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE) as? BluetoothDevice) ?: return
            if (device.address != pendingBondDevice?.address) return
            when (device.bondState) {
                BluetoothDevice.BOND_BONDED -> {
                    pendingBondDevice = null
                    viewModel.reconnectKnownAdapter()
                }
                BluetoothDevice.BOND_NONE -> {
                    pendingBondDevice = null
                    Toast.makeText(this@MainActivity, "Adapter bonding did not complete", Toast.LENGTH_LONG).show()
                }
            }
        }
    }
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = viewModel.refreshSources()
        override fun onInputDeviceRemoved(deviceId: Int) = viewModel.refreshSources()
        override fun onInputDeviceChanged(deviceId: Int) = viewModel.refreshSources()
    }

    private val managementPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) {
            if (pendingManagementAction == ManagementAction.Pair) pairAdapter() else viewModel.reconnectKnownAdapter()
        }
        else Toast.makeText(this, "Nearby devices permission is required to find PicoSwitch2", Toast.LENGTH_LONG).show()
    }

    private val controllerPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) viewModel.acquireControllerBridge()
        else Toast.makeText(this, "Nearby devices permission is required for the controller bridge", Toast.LENGTH_LONG).show()
    }

    private val importAmiibo = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { viewModel.importAmiibo(it, "") }
    }

    private val importAmiiboKeys = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { viewModel.importAmiiboKeys(it) }
    }

    private val associationChooser = registerForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@registerForActivityResult
        val associationAddress: String?
        val associationId: Int?
        if (Build.VERSION.SDK_INT >= 33) {
            val association = result.data?.getParcelableExtra(
                CompanionDeviceManager.EXTRA_ASSOCIATION,
                AssociationInfo::class.java,
            )
            associationAddress = association?.deviceMacAddress?.toString()
            associationId = association?.id
        } else {
            associationAddress = null
            associationId = null
        }
        @Suppress("DEPRECATION")
        val legacyDevice = if (Build.VERSION.SDK_INT >= 33) {
            result.data?.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE, BluetoothDevice::class.java)
        } else result.data?.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE) as? BluetoothDevice
        val device = associationAddress?.let(::remoteDevice) ?: legacyDevice
        if (device == null) {
            Toast.makeText(this, "Android associated the adapter but did not return its Bluetooth address", Toast.LENGTH_LONG).show()
        } else rememberBondAndConnect(device, associationId)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CompanionApp(
                viewModel = viewModel,
                onConnectAdapter = ::requestAdapterConnection,
                onPairAdapter = ::requestNewAdapterPairing,
                onImportAmiibo = { importAmiibo.launch(arrayOf("*/*")) },
                onImportAmiiboKeys = { importAmiiboKeys.launch(arrayOf("application/octet-stream", "application/*", "*/*")) },
                onPrepareController = ::requestControllerBridge,
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
        return viewModel.inputRouter.onKey(event) || super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        return viewModel.inputRouter.onMotion(event) || super.dispatchGenericMotionEvent(event)
    }

    override fun onPause() {
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
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(bondReceiver, filter, RECEIVER_NOT_EXPORTED)
            else @Suppress("DEPRECATION") registerReceiver(bondReceiver, filter)
            bondReceiverRegistered = true
        }
        getSystemService(InputManager::class.java)?.registerInputDeviceListener(inputDeviceListener, null)
        if (hasManagementPermissions()) {
            restoreSystemAssociation()
            viewModel.tryAutoReconnect()
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

    @SuppressLint("MissingPermission")
    private fun pairAdapter() {
        val request = AssociationRequest.Builder()
            .addDeviceFilter(BluetoothDeviceFilter.Builder().setNamePattern(
                Pattern.compile(AdapterBluetoothIdentity.CHOOSER_NAME_PATTERN, Pattern.CASE_INSENSITIVE),
            ).build())
            .setSingleDevice(false)
            .build()
        val manager = getSystemService(CompanionDeviceManager::class.java) ?: return Toast.makeText(
            this, "Companion Device Manager is unavailable on this Android build", Toast.LENGTH_LONG,
        ).show()
        val callback = object : CompanionDeviceManager.Callback() {
            @Deprecated("Legacy association callback")
            override fun onDeviceFound(chooserLauncher: IntentSender) = launchAssociation(chooserLauncher)
            override fun onAssociationPending(intentSender: IntentSender) = launchAssociation(intentSender)
            override fun onAssociationCreated(associationInfo: AssociationInfo) {
                val address: String?
                val associationId: Int?
                if (Build.VERSION.SDK_INT >= 33) {
                    address = associationInfo.deviceMacAddress?.toString()
                    associationId = associationInfo.id
                } else {
                    address = null
                    associationId = null
                }
                val device = address?.let(::remoteDevice)
                if (device != null) rememberBondAndConnect(device, associationId)
                else Toast.makeText(this@MainActivity, "Adapter associated, but Android did not return its address.", Toast.LENGTH_LONG).show()
            }
            override fun onFailure(error: CharSequence?) {
                Toast.makeText(this@MainActivity, error ?: "Adapter pairing was cancelled", Toast.LENGTH_LONG).show()
            }
        }
        try {
            if (Build.VERSION.SDK_INT >= 33) manager.associate(request, mainExecutor, callback)
            else @Suppress("DEPRECATION") manager.associate(request, callback, Handler(Looper.getMainLooper()))
        } catch (error: Throwable) {
            // Some OEM frameworks reject association synchronously. Controller setup must
            // report that as a recoverable error, never take down the foreground app.
            viewModel.diagnostics.error("controller", "companion association", error)
            Toast.makeText(
                this,
                error.message?.take(180) ?: "Android could not open the controller pairing chooser",
                Toast.LENGTH_LONG,
            ).show()
        }
    }

    private fun launchAssociation(sender: IntentSender) {
        runOnUiThread { associationChooser.launch(IntentSenderRequest.Builder(sender).build()) }
    }

    @SuppressLint("MissingPermission")
    private fun rememberBondAndConnect(device: BluetoothDevice, associationId: Int?) {
        viewModel.recordAdapterAssociation(device.address, associationId, runCatching { device.name }.getOrNull())
        when (device.bondState) {
            BluetoothDevice.BOND_BONDED -> viewModel.reconnectKnownAdapter()
            BluetoothDevice.BOND_NONE -> {
                pendingBondDevice = device
                if (device.createBond()) Toast.makeText(this, "Approve Android’s adapter bond prompt", Toast.LENGTH_LONG).show()
                else Toast.makeText(this, "Android could not start bonding", Toast.LENGTH_LONG).show()
            }
            BluetoothDevice.BOND_BONDING -> pendingBondDevice = device
        }
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
        if (viewModel.ui.value.adapterRelationship != null) return
        val manager = getSystemService(CompanionDeviceManager::class.java) ?: return
        if (Build.VERSION.SDK_INT >= 33) {
            manager.myAssociations.firstOrNull { it.deviceMacAddress != null }?.let { association ->
                viewModel.recordAdapterAssociation(
                    association.deviceMacAddress.toString(), association.id,
                    association.displayName?.toString(),
                )
            }
        } else {
            @Suppress("DEPRECATION")
            manager.associations.firstOrNull()?.let { address ->
                viewModel.recordAdapterAssociation(address, null, remoteDevice(address)?.name)
            }
        }
        if (viewModel.ui.value.adapterRelationship == null) {
            viewModel.pairedControllerHosts().firstOrNull()?.let { device ->
                viewModel.recordAdapterAssociation(device.address, null, runCatching { device.name }.getOrNull())
            }
        }
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
