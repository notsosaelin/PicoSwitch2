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
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.core.content.FileProvider
import dev.picoswitch.companion.ui.CompanionApp
import dev.picoswitch.companion.ui.CompanionViewModel
import java.util.regex.Pattern

class MainActivity : ComponentActivity() {
    private val viewModel: CompanionViewModel by viewModels()
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = viewModel.refreshSources()
        override fun onInputDeviceRemoved(deviceId: Int) = viewModel.refreshSources()
        override fun onInputDeviceChanged(deviceId: Int) = viewModel.refreshSources()
    }

    private val managementPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) viewModel.connect()
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
        @Suppress("DEPRECATION")
        val device = if (Build.VERSION.SDK_INT >= 33) {
            result.data?.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE, BluetoothDevice::class.java)
        } else result.data?.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE) as? BluetoothDevice
        if (device == null) {
            Toast.makeText(this, "Android associated the adapter; reopen Controller after bonding finishes", Toast.LENGTH_LONG).show()
        } else bondAndConnect(device)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CompanionApp(
                viewModel = viewModel,
                onConnectAdapter = ::requestAdapterConnection,
                onImportAmiibo = { importAmiibo.launch(arrayOf("*/*")) },
                onImportAmiiboKeys = { importAmiiboKeys.launch(arrayOf("application/octet-stream", "application/*", "*/*")) },
                onPrepareController = ::requestControllerBridge,
                onPairControllerHost = ::pairControllerHost,
                onExportDiagnostics = ::shareDiagnostics,
            )
        }
        viewModel.recordLifecycle("created")
    }

    fun requestAdapterConnection() {
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
        viewModel.neutralizeController()
        viewModel.recordLifecycle("paused; controller neutralized")
        super.onPause()
    }

    override fun onStart() {
        super.onStart()
        getSystemService(InputManager::class.java)?.registerInputDeviceListener(inputDeviceListener, null)
    }

    override fun onStop() {
        getSystemService(InputManager::class.java)?.unregisterInputDeviceListener(inputDeviceListener)
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        viewModel.recordLifecycle("resumed")
        viewModel.refreshSources()
    }

    private fun requestControllerBridge() {
        if (Build.VERSION.SDK_INT >= 31) controllerPermissions.launch(arrayOf(Manifest.permission.BLUETOOTH_CONNECT))
        else viewModel.acquireControllerBridge()
    }

    @SuppressLint("MissingPermission")
    private fun pairControllerHost() {
        val request = AssociationRequest.Builder()
            .addDeviceFilter(BluetoothDeviceFilter.Builder().setNamePattern(Pattern.compile("^(PicoSwitch2|Joypad Adapter).*$", Pattern.CASE_INSENSITIVE)).build())
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
                val address = if (Build.VERSION.SDK_INT >= 33) associationInfo.deviceMacAddress?.toString() else null
                val device = address?.let {
                    runCatching { getSystemService(BluetoothManager::class.java).adapter.getRemoteDevice(it) }.getOrNull()
                }
                if (device != null) bondAndConnect(device)
                else Toast.makeText(this@MainActivity, "Adapter associated. Select it from the saved-host list to connect.", Toast.LENGTH_LONG).show()
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
    private fun bondAndConnect(device: BluetoothDevice) {
        when (device.bondState) {
            BluetoothDevice.BOND_BONDED -> viewModel.connectControllerHost(device)
            BluetoothDevice.BOND_NONE -> {
                if (device.createBond()) Toast.makeText(this, "Approve Android’s bond prompt, then select the adapter again", Toast.LENGTH_LONG).show()
                else Toast.makeText(this, "Android could not start bonding", Toast.LENGTH_LONG).show()
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
