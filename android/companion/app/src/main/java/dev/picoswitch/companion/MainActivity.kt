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
import dev.picoswitch.companion.ui.CompanionApp
import dev.picoswitch.companion.ui.CompanionViewModel
import java.util.concurrent.Executors
import java.util.regex.Pattern

class MainActivity : ComponentActivity() {
    private val viewModel: CompanionViewModel by viewModels()

    private val managementPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) viewModel.connect()
        else Toast.makeText(this, "Nearby devices permission is required to find PicoSwitch2", Toast.LENGTH_LONG).show()
    }

    private val controllerPermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        if (result.values.all { it }) viewModel.acquireControllerBridge()
        else Toast.makeText(this, "Nearby devices permission is required for the controller bridge", Toast.LENGTH_LONG).show()
    }

    private val importAmiibo = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let {
            runCatching { contentResolver.takePersistableUriPermission(it, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
            viewModel.importAmiibo(it, "")
        }
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
                onPrepareController = ::requestControllerBridge,
                onPairControllerHost = ::pairControllerHost,
            )
        }
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
        super.onPause()
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
        val manager = getSystemService(CompanionDeviceManager::class.java)
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
        if (Build.VERSION.SDK_INT >= 33) manager.associate(request, Executors.newSingleThreadExecutor(), callback)
        else @Suppress("DEPRECATION") manager.associate(request, callback, Handler(Looper.getMainLooper()))
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
}
