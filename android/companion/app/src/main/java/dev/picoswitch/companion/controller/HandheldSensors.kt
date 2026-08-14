package dev.picoswitch.companion.controller

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.media.AudioAttributes
import android.os.BatteryManager
import android.os.VibrationAttributes
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

    /**
     * Newest gyro sample, deliberately NOT averaged here.
     *
     * The adapter routes this source through the same motion translator the
     * DualSense uses, which applies its own low-pass, bias tracking and stillness
     * gate. Those are tuned for raw sensor input, so smoothing in the app would
     * stack a second filter ahead of them and only add lag. The reference
     * implementation that already ships this format feeds it the newest sample for
     * the same reason.
     */
    @Volatile private var gyroX = 0
    @Volatile private var gyroY = 0
    @Volatile private var gyroZ = 0

    @Volatile private var accelX = 0
    @Volatile private var accelY = 0
    @Volatile private var accelZ = 0
    @Volatile private var sawGyro = false
    @Volatile private var sawAccel = false
    @Volatile private var running = false

    /**
     * Wire timestamp of the newest gyroscope sample, in milliseconds.
     *
     * This MUST come from the sensor event rather than from send time. The report
     * cadence is faster than the IMU actually delivers, so the same physical
     * sample is sent more than once; the adapter de-duplicates on this field and
     * only advances its motion sequence when it genuinely changes. Stamping it at
     * send time made every repeat look like a fresh IMU frame, which the console
     * then integrated as real movement -- aim jitters and over-responds.
     *
     * The gyroscope is the reference because it is what aim integrates, and both
     * sensors come from the same physical IMU at the same requested rate. Keying
     * off "newest of either sensor" would advance the sequence twice per frame,
     * since the two callbacks never carry an identical timestamp.
     */
    @Volatile private var gyroTimestampMs = 0

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
        // Never carry a previous session's last rate into the next one.
        gyroX = 0; gyroY = 0; gyroZ = 0
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
            timestampMs = gyroTimestampMs,
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
                // SensorEvent.timestamp is nanoseconds on the elapsed-realtime base.
                // Narrowed to the contract's 16-bit millisecond field; wrap is
                // expected and harmless, because the adapter only ever compares it
                // to the previous value for equality.
                gyroTimestampMs = ((event.timestamp / 1_000_000L) and 0xFFFF).toInt()
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
 * Amplitude shaping, kept pure so it can be tested without a vibrator.
 *
 * The console sends 0..255. An actuator does not usefully reproduce all of that:
 * below its start threshold an LRA (and an ERM below stiction) makes audible
 * driver noise and no perceptible movement, which is the "buzzes but does
 * nothing" failure. Tiny changes are also not worth an actuator restart, since
 * Android has no public way to alter an effect's amplitude in flight.
 */
object RumbleShaping {
    /** Below this the actuator is silenced entirely. */
    const val GATE_OFF = 8

    /** Rising edge, above GATE_OFF so a value parked on the boundary cannot chatter. */
    const val GATE_ON = 14

    /** Retrigger granularity; finer differences are imperceptible. */
    const val STEP = 16

    /**
     * @param raw newest console amplitude
     * @param previous the last value this function returned, for hysteresis
     */
    fun shape(raw: Int, previous: Int): Int {
        val clamped = raw.coerceIn(0, 255)
        val gated = when {
            clamped <= GATE_OFF -> 0
            clamped >= GATE_ON -> clamped
            // Between the thresholds, hold whatever we were already doing.
            else -> if (previous > 0) clamped else 0
        }
        if (gated == 0) return 0
        // Round to nearest rather than down, so quantisation does not
        // systematically under-drive, and clamp so full scale stays full scale --
        // flooring would cap the console's hardest rumble at 240/255.
        return (((gated + STEP / 2) / STEP) * STEP)
            .coerceAtLeast(GATE_ON)
            .coerceAtMost(255)
    }
}

/**
 * Console rumble -> handheld vibration.
 *
 * A phone has one general-purpose actuator, so the two motor amplitudes arrive
 * already combined. Three things about Android make this less obvious than it
 * looks, all of them verified against a real device's vibrator service:
 *
 * 1. USAGE. A bare vibrate(effect) is classified USAGE_TOUCH, which the system
 *    "touch feedback" setting can disable outright. On the maintainer's handheld
 *    that setting is off, so every rumble was being dropped with
 *    `status: ignored_for_settings, scale: 0.00` while the media intensity was
 *    perfectly enabled. Declaring this as media vibration is what makes rumble
 *    reach the actuator at all -- it is not a cosmetic annotation.
 *
 * 2. DURATION. Both the firmware and this app suppress unchanged values, so a
 *    game holding a constant amplitude sends exactly one report. A bounded
 *    one-shot therefore expired mid-effect and left the handheld silent until
 *    the console next changed the value. The effect now repeats until it is
 *    explicitly cancelled, and a watchdog guarantees the cancel.
 *
 * 3. RATE. Feedback can change every few milliseconds. Android cancels a playing
 *    vibration before starting the next unless both are flagged pipelined, and
 *    that flag is not public, so every change is an audible stop/start. The
 *    retrigger rate is therefore bounded, always using the newest value.
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

    // Probed once. These were previously queried on every update, on the same
    // thread that services the HID callbacks.
    private val hasVibrator = runCatching { vibrator?.hasVibrator() == true }.getOrDefault(false)
    private val hasAmplitudeControl =
        runCatching { vibrator?.hasAmplitudeControl() == true }.getOrDefault(false)

    val available: Boolean get() = hasVibrator

    @Volatile private var shaped = 0
    @Volatile private var playing = false
    @Volatile private var lastIssuedAtMs = 0L

    @Synchronized
    fun apply(amplitude: Int) {
        val next = RumbleShaping.shape(amplitude, shaped)
        if (next == shaped) return
        val now = SystemClock.elapsedRealtime()
        // A stop is always immediate; only ramping up or changing level waits.
        if (next != 0 && now - lastIssuedAtMs < MIN_RETRIGGER_MS) return
        shaped = next
        lastIssuedAtMs = now
        issue(next)
    }

    /**
     * Re-assert the current effect. The vibration repeats indefinitely, so this
     * exists purely so a bridge that has gone quiet cannot leave the actuator
     * running: the caller ticks this while the link is live, and [stop] is still
     * called on every teardown path.
     */
    @Synchronized
    fun keepAlive() {
        if (!playing) return
        if (SystemClock.elapsedRealtime() - lastIssuedAtMs > WATCHDOG_MS) stop()
    }

    private fun issue(amplitude: Int) {
        val device = vibrator ?: return
        if (!hasVibrator) return
        runCatching {
            if (amplitude == 0) {
                device.cancel()
                playing = false
                return@runCatching
            }
            val level = if (hasAmplitudeControl) amplitude else VibrationEffect.DEFAULT_AMPLITUDE
            // Repeat from index 0 until cancelled. Consecutive segments carry the
            // same amplitude, so looping is not an audible off/on.
            val effect = VibrationEffect.createWaveform(
                longArrayOf(SEGMENT_MS), intArrayOf(level), 0,
            )
            vibrateAsMedia(device, effect)
            playing = true
        }
    }

    /**
     * Classify as media vibration: "game, or any interactive media that isn't
     * touch feedback specifically" is exactly this. Without it the effect is
     * treated as touch feedback and silently discarded on any device where the
     * user has that turned off.
     */
    private fun vibrateAsMedia(device: Vibrator, effect: VibrationEffect) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            device.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA))
        } else {
            @Suppress("DEPRECATION")
            device.vibrate(
                effect,
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
        }
    }

    @Synchronized
    fun stop() {
        shaped = 0
        playing = false
        lastIssuedAtMs = 0L
        runCatching { vibrator?.cancel() }
    }

    private companion object {
        /** Loop length. Not perceptually load bearing; the effect repeats. */
        const val SEGMENT_MS = 1000L

        /** ~25 Hz ceiling on actuator restarts. */
        const val MIN_RETRIGGER_MS = 40L

        /** The effect never self-expires, so a stalled bridge must not leave it on. */
        const val WATCHDOG_MS = 1000L
    }
}
