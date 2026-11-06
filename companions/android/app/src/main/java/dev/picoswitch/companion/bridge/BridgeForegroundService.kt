package dev.picoswitch.companion.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import dev.picoswitch.bridge.core.BridgeDiagnostics

/**
 * Keeps the process foreground while the controller link is live.
 *
 * NOT cosmetic, and not only about process death. Android's vibrator service
 * checks the caller's process state BEFORE it consults any setting:
 *
 *     VibrationSettings.shouldIgnoreVibration()
 *       if (!isUidForeground(uid) &&
 *           !BACKGROUND_PROCESS_USAGE_ALLOWLIST.contains(usage))
 *           return IGNORED_BACKGROUND;
 *
 * That allowlist is {RINGTONE, ALARM, NOTIFICATION, COMMUNICATION_REQUEST,
 * HARDWARE_FEEDBACK, PHYSICAL_EMULATION}. USAGE_MEDIA is not on it, and neither
 * is USAGE_UNKNOWN. `isUidForeground` requires process state <=
 * PROCESS_STATE_IMPORTANT_FOREGROUND (6); an Activity whose screen has turned off
 * is TOP_SLEEPING (12), and one the user has switched away from is
 * CACHED_ACTIVITY (16).
 *
 * That is exactly this app's situation in real use: the player is looking at a
 * television, not at MainActivity. Every rumble would be discarded before any
 * setting was consulted, and `Vibrator.vibrate()` still returns normally, so
 * nothing inside the app can observe it. PROCESS_STATE_FOREGROUND_SERVICE is 4,
 * which passes.
 *
 * This is also simply correct for a Bluetooth HID bridge: an Activity-only
 * process is eligible for cached-process kill at any moment.
 *
 * The InputDevice-scoped vibrator path (see HapticRouting) goes through
 * InputManagerService instead and has no such gate, so these are two independent
 * fixes for the same symptom rather than alternatives.
 *
 * STATUS: reasoned from AOSP sources, NOT yet confirmed on the Thor.
 */
class BridgeForegroundService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        // The bridge owns this lifecycle explicitly; never let the system
        // resurrect the service without a live link.
        return START_NOT_STICKY
    }

    private fun buildNotification(): Notification {
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val manager = getSystemService(NotificationManager::class.java)
            if (manager?.getNotificationChannel(CHANNEL_ID) == null) {
                manager?.createNotificationChannel(
                    NotificationChannel(
                        CHANNEL_ID,
                        "Controller link",
                        NotificationManager.IMPORTANCE_LOW,
                    ).apply { setShowBadge(false) },
                )
            }
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("Controller connected")
            .setContentText("Streaming input to the PicoSwitch2 adapter")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "controller_link"
        private const val NOTIFICATION_ID = 1

        /**
         * Best-effort: a failure here must never take down the controller link.
         *
         * But it must never be SILENT either. This call failing costs the user all
         * rumble whenever the app is not on screen, and `Vibrator.vibrate()` still
         * returns normally afterwards, so nothing downstream can observe it. It
         * failed exactly that way once already — the service was implemented but
         * never declared in the manifest, so `startForegroundService()` threw into
         * a bare `runCatching` and the foreground state was never entered.
         */
        fun start(context: Context, diagnostics: BridgeDiagnostics = BridgeDiagnostics.None) =
            runCatching {
                val intent = Intent(context, BridgeForegroundService::class.java)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(intent)
                } else {
                    context.startService(intent)
                }
            }.onFailure { error ->
                diagnostics.error("transport", "foreground service", error)
                diagnostics.event(
                    "transport", "foreground service FAILED",
                    "the process cannot enter PROCESS_STATE_FOREGROUND_SERVICE; " +
                        "Android will discard rumble whenever this app is not on screen",
                )
            }

        fun stop(context: Context, diagnostics: BridgeDiagnostics = BridgeDiagnostics.None) =
            runCatching {
                context.stopService(Intent(context, BridgeForegroundService::class.java))
            }.onFailure { error -> diagnostics.error("transport", "foreground service stop", error) }
    }
}
