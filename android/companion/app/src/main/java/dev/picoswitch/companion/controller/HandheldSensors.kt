package dev.picoswitch.companion.controller

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.BatteryManager
import android.os.Build
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.Surface
import android.view.WindowManager

/**
 * Handheld gyroscope + accelerometer, converted to the adapter's wire units.
 *
 * Registration is on demand: the adapter tells us whether the console is actually
 * consuming motion (the motion-wanted flag in its feedback report), and we only
 * hold sensor registrations while it is. Streaming an IMU that nothing reads is a
 * pure battery cost on a phone.
 */
class MotionSource(context: Context) : SensorEventListener {
    private val appContext = context.applicationContext
    private val sensorManager = appContext.getSystemService(SensorManager::class.java)
    @Volatile private var cachedRotationDegrees = 0
    @Volatile private var rotationCheckedAtMs = 0L
    private val gyroscope = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val accelerometer = sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

    /** False on a handheld with no IMU; the bridge then simply never sends motion. */
    val available: Boolean get() = gyroscope != null && accelerometer != null

    @Volatile private var gyroX = 0
    @Volatile private var gyroY = 0
    @Volatile private var gyroZ = 0
    @Volatile private var accelX = 0
    @Volatile private var accelY = 0
    @Volatile private var accelZ = 0
    @Volatile private var sawGyro = false
    @Volatile private var sawAccel = false
    @Volatile private var running = false

    @Synchronized
    fun start() {
        if (running || !available) return
        val manager = sensorManager ?: return
        manager.registerListener(this, gyroscope, SAMPLING_PERIOD_US)
        manager.registerListener(this, accelerometer, SAMPLING_PERIOD_US)
        running = true
    }

    @Synchronized
    fun stop() {
        if (!running) return
        sensorManager?.unregisterListener(this)
        running = false
        sawGyro = false
        sawAccel = false
    }

    /**
     * Latest sample in wire units, rotated into the orientation the user is
     * actually holding. [ControllerMotion.valid] is false until both sensors have
     * reported at least once, so a half-populated first frame is never published
     * as motion.
     */
    fun sample(): ControllerMotion {
        if (!running || !sawGyro || !sawAccel) return ControllerMotion.None
        val rotation = currentRotationDegrees()
        return ControllerMotion(
            gyroX = MotionOrientation.remapX(gyroX, gyroY, rotation),
            gyroY = MotionOrientation.remapY(gyroX, gyroY, rotation),
            gyroZ = MotionOrientation.remapZ(gyroZ),
            accelX = MotionOrientation.remapX(accelX, accelY, rotation),
            accelY = MotionOrientation.remapY(accelX, accelY, rotation),
            accelZ = MotionOrientation.remapZ(accelZ),
            timestampMs = (SystemClock.elapsedRealtime() and 0xFFFF).toInt(),
            valid = true,
        )
    }

    /**
     * Screen rotation, cached. sample() runs at the 125 Hz report cadence and the
     * display query is a framework call, so it is refreshed on a slow cadence
     * instead -- a rotation takes far longer than this to complete and the user
     * cannot perceive the difference.
     */
    private fun currentRotationDegrees(): Int {
        val now = SystemClock.elapsedRealtime()
        if (now - rotationCheckedAtMs >= ROTATION_REFRESH_MS) {
            rotationCheckedAtMs = now
            cachedRotationDegrees = runCatching {
                val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    appContext.display
                } else {
                    @Suppress("DEPRECATION")
                    appContext.getSystemService(WindowManager::class.java)?.defaultDisplay
                }
                when (display?.rotation) {
                    Surface.ROTATION_90 -> 90
                    Surface.ROTATION_180 -> 180
                    Surface.ROTATION_270 -> 270
                    else -> 0
                }
            }.getOrDefault(cachedRotationDegrees)
        }
        return cachedRotationDegrees
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor?.type) {
            Sensor.TYPE_GYROSCOPE -> {
                gyroX = MotionScale.gyroCounts(event.values[0])
                gyroY = MotionScale.gyroCounts(event.values[1])
                gyroZ = MotionScale.gyroCounts(event.values[2])
                sawGyro = true
            }
            Sensor.TYPE_ACCELEROMETER -> {
                accelX = MotionScale.accelCounts(event.values[0])
                accelY = MotionScale.accelCounts(event.values[1])
                accelZ = MotionScale.accelCounts(event.values[2])
                sawAccel = true
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    private companion object {
        // ~200 Hz requested; Android may deliver slower. The report cadence is
        // 125 Hz, so this keeps a fresh sample available for every report.
        const val SAMPLING_PERIOD_US = 5_000
        const val ROTATION_REFRESH_MS = 500L
    }
}

/**
 * Handheld battery level, forwarded so the console shows a real controller
 * battery instead of nothing. Read from the sticky ACTION_BATTERY_CHANGED
 * broadcast, which needs no receiver lifecycle and no permission.
 */
class BatterySource(context: Context) {
    private val appContext = context.applicationContext

    fun read(): ControllerBattery {
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

/**
 * Console rumble -> handheld vibration.
 *
 * A phone has one general-purpose actuator, so the two motor amplitudes are
 * combined. Each update issues a bounded one-shot slightly longer than the
 * adapter's update interval and the next update replaces it: continuous rumble
 * stays continuous while it is being refreshed, and it stops on its own if the
 * link dies mid-effect rather than buzzing forever.
 */
class HandheldHaptics(context: Context) {
    private val vibrator: Vibrator? = run {
        val app = context.applicationContext
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            app.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            app.getSystemService(Vibrator::class.java)
        }
    }

    val available: Boolean get() = vibrator?.hasVibrator() == true

    private var lastAmplitude = -1

    fun apply(amplitude: Int) {
        val clamped = amplitude.coerceIn(0, 255)
        if (clamped == lastAmplitude) return
        lastAmplitude = clamped
        val device = vibrator ?: return
        runCatching {
            if (clamped == 0) {
                device.cancel()
            } else if (device.hasAmplitudeControl()) {
                device.vibrate(VibrationEffect.createOneShot(EFFECT_MS, clamped))
            } else {
                // No amplitude control: fall back to on/off so rumble is still felt.
                device.vibrate(VibrationEffect.createOneShot(EFFECT_MS, VibrationEffect.DEFAULT_AMPLITUDE))
            }
        }
    }

    fun stop() {
        lastAmplitude = -1
        runCatching { vibrator?.cancel() }
    }

    private companion object {
        /** Comfortably longer than the adapter's feedback cadence, short enough
         *  that a lost link stops the motor quickly. */
        const val EFFECT_MS = 350L
    }
}
