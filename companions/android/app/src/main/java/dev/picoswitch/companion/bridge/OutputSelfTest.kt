package dev.picoswitch.companion.bridge

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.core.RumbleRequest
import dev.picoswitch.companion.BuildConfig

/**
 * Debug-only hook so the Android OUTPUT path can be exercised with no console, no
 * adapter and no Bluetooth link:
 *
 * ```
 * adb shell am broadcast -a dev.picoswitch.companion.SELF_TEST_RUMBLE \
 *     --ei left 220 --ei right 220
 * adb shell am broadcast -a dev.picoswitch.companion.SELF_TEST_RUMBLE \
 *     --ei left 0 --ei right 0
 * ```
 *
 * It exists because "did the adapter deliver rumble" and "can this handheld
 * vibrate at all" are separate questions, and answering the second should never
 * require setting up the first. It targets the platform backend directly rather
 * than the session for the same reason: the point is to isolate the platform.
 *
 * Registered dynamically and exported so `am broadcast` from the shell uid can
 * reach it; a manifest receiver would have to be exported permanently, which is
 * not something a release build should carry. Gated on `BuildConfig.DEBUG` so it
 * cannot exist in a release build at all.
 */
class OutputSelfTest(
    context: Context,
    private val output: AndroidOutputBackend,
    private val diagnostics: BridgeDiagnostics = BridgeDiagnostics.None,
) {
    private val appContext = context.applicationContext
    private var receiver: BroadcastReceiver? = null

    fun register() {
        if (!BuildConfig.DEBUG || receiver != null) return
        val created = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                val left = intent?.getIntExtra("left", 220) ?: 220
                val right = intent?.getIntExtra("right", left) ?: left
                diagnostics.event(
                    "controller", "haptic self-test",
                    "L=$left R=$right route=${output.diagnostics().route}",
                )
                output.apply(RumbleRequest(left, right))
            }
        }
        runCatching {
            val filter = IntentFilter(ACTION_SELF_TEST_RUMBLE)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                appContext.registerReceiver(created, filter, Context.RECEIVER_EXPORTED)
            } else {
                @Suppress("UnspecifiedRegisterReceiverFlag")
                appContext.registerReceiver(created, filter)
            }
            receiver = created
        }
    }

    fun unregister() {
        val current = receiver ?: return
        receiver = null
        runCatching { appContext.unregisterReceiver(current) }
    }

    companion object {
        const val ACTION_SELF_TEST_RUMBLE = "dev.picoswitch.companion.SELF_TEST_RUMBLE"
    }
}
