package dev.picoswitch.companion.bridge

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.session.BatteryBackend

/**
 * Android battery level -> the bridge's battery field, so the console shows a
 * real controller battery instead of nothing.
 *
 * Read from the sticky `ACTION_BATTERY_CHANGED` broadcast, which needs no
 * receiver lifecycle and no permission.
 */
class AndroidBatteryBackend(context: Context) : BatteryBackend {
    private val appContext = context.applicationContext

    /** Every Android device the bridge runs on has a battery. */
    override val available: Boolean = true

    override fun read(): ControllerBattery {
        val intent: Intent? = runCatching {
            appContext.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        }.getOrNull() ?: return ControllerBattery.Unknown

        val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        if (level < 0 || scale <= 0) return ControllerBattery.Unknown

        val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL
        return ControllerBattery(
            levelPercent = (level * 100 / scale).coerceIn(0, 100),
            charging = charging,
            valid = true,
        )
    }
}
