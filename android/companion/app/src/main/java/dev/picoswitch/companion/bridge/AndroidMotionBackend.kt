package dev.picoswitch.companion.bridge

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.view.Display
import android.view.Surface
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.MotionScale
import dev.picoswitch.bridge.core.ScreenOrientation
import dev.picoswitch.bridge.session.MotionBackend
import dev.picoswitch.bridge.session.MotionDiagnostics

/**
 * Android sensors -> the bridge's canonical motion convention.
 *
 * The conversion contract this must satisfy is defined once, in
 * `dev.picoswitch.bridge.core.MotionConvention`: axes, signs, units, the
 * held-orientation frame, and the timestamp base. This class does the two things
 * only Android can do — read the sensors, and read the display rotation — and
 * hands over a sample that needs no further interpretation.
 *
 * Registration is on demand: the adapter tells us whether the console is actually
 * consuming motion, and we only hold sensor registrations while it is. Streaming
 * an IMU that nothing reads is a pure battery cost on a phone.
 */
class AndroidMotionBackend(context: Context) : MotionBackend, SensorEventListener {
    private val appContext = context.applicationContext
    private val sensorManager = appContext.getSystemService(SensorManager::class.java)
    private val displayManager = appContext.getSystemService(DisplayManager::class.java)
    @Volatile private var cachedRotationDegrees = 0
    @Volatile private var rotationCheckedAtMs = 0L
    @Volatile private var rotationReadable = false
    private val gyroscope = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val accelerometer = sensorManager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

    /** False on a handheld with no IMU; the bridge then simply never sends motion. */
    override val available: Boolean get() = gyroscope != null && accelerometer != null

    /**
     * Newest gyro sample, deliberately NOT averaged here.
     *
     * The adapter routes this source through the same motion translator the
     * DualSense uses, which applies its own low-pass, bias tracking and stillness
     * gate. Those are tuned for raw sensor input, so smoothing in the app would
     * stack a second filter ahead of them and only add lag.
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
     * Raw SI values, retained ONLY for the platform-raw diagnostic line.
     *
     * Scalars rather than arrays: this is written twice per sensor sample at
     * ~200 Hz, and allocating two float arrays per sample to service a string
     * that is read only when somebody opens the diagnostics screen is pure
     * garbage pressure on the sensor thread.
     */
    @Volatile private var rawGyroX = 0f
    @Volatile private var rawGyroY = 0f
    @Volatile private var rawGyroZ = 0f
    @Volatile private var rawAccelX = 0f
    @Volatile private var rawAccelY = 0f
    @Volatile private var rawAccelZ = 0f

    /**
     * Sensor callbacks are delivered here, NOT on the main thread.
     *
     * `SensorManager.registerListener(listener, sensor, periodUs)` — the overload
     * without a Handler — posts every event to the MAIN looper. That makes the IMU
     * a hostage of whatever else the UI is doing: any main-thread stall delays or
     * coalesces sensor samples, and motion degrades in a way that looks like a
     * protocol bug rather than a scheduling one. It bit this bridge exactly once,
     * when the session published diagnostics at report cadence and the resulting
     * recomposition storm starved the sensors it was reporting on.
     *
     * A dedicated thread removes the coupling entirely: motion now depends only on
     * the sensor stack, whatever the UI is doing.
     */
    private val sensorThread = HandlerThread("picoswitch-motion").apply { start() }
    private val sensorHandler = Handler(sensorThread.looper)

    /**
     * Wire timestamp of the newest gyroscope sample, in 100 us ticks.
     *
     * The gyroscope is the reference because it is what aim integrates, and both
     * sensors come from the same physical IMU at the same requested rate. Keying
     * off "newest of either sensor" would advance the sequence twice per frame,
     * since the two callbacks never carry an identical timestamp. See
     * `MotionConvention` for why it must not be stamped at send time.
     */
    @Volatile private var gyroTimestampTicks = 0

    @Synchronized
    override fun start() {
        if (running || !available) return
        val manager = sensorManager ?: return
        manager.registerListener(this, gyroscope, SAMPLING_PERIOD_US, sensorHandler)
        manager.registerListener(this, accelerometer, SAMPLING_PERIOD_US, sensorHandler)
        running = true
    }

    @Synchronized
    override fun stop() {
        if (!running) return
        sensorManager?.unregisterListener(this)
        running = false
        sawGyro = false
        sawAccel = false
        // Never carry a previous session's last rate into the next one.
        gyroX = 0; gyroY = 0; gyroZ = 0
    }

    /**
     * Latest sample in canonical units, rotated into the orientation the user is
     * actually holding. [ControllerMotion.valid] is false until both sensors have
     * reported at least once, so a half-populated first frame is never published
     * as motion.
     */
    override fun sample(): ControllerMotion {
        if (!running || !sawGyro || !sawAccel) return ControllerMotion.None
        return ScreenOrientation.apply(
            ControllerMotion(
                gyroX = gyroX, gyroY = gyroY, gyroZ = gyroZ,
                accelX = accelX, accelY = accelY, accelZ = accelZ,
                timestampTicks = gyroTimestampTicks,
                valid = true,
            ),
            currentRotationDegrees(),
        )
    }

    /**
     * The three layers separated: what Android reported, what the bridge made of
     * it, and which frame correction was applied. An axis complaint is only
     * interpretable with all three, and a rotation that could not be read at all
     * is itself the defect rather than a value worth trusting.
     */
    override fun diagnostics(): MotionDiagnostics {
        val rotation = currentRotationDegrees()
        if (!running) {
            return MotionDiagnostics(
                platformRaw = if (available) "idle" else "no IMU",
                canonical = if (available) "idle" else "no IMU",
                frameRotationDegrees = rotation,
                frameRotationMeasured = rotationReadable,
            )
        }
        return MotionDiagnostics(
            platformRaw = "gyro %.3f,%.3f,%.3f rad/s accel %.2f,%.2f,%.2f m/s2"
                .format(rawGyroX, rawGyroY, rawGyroZ, rawAccelX, rawAccelY, rawAccelZ),
            canonical = "gyro $gyroX,$gyroY,$gyroZ accel $accelX,$accelY,$accelZ t=$gyroTimestampTicks",
            frameRotationDegrees = rotation,
            frameRotationMeasured = rotationReadable,
        )
    }

    /**
     * Screen rotation, cached. sample() runs at the 125 Hz report cadence and the
     * display query is a framework call, so it is refreshed on a slow cadence
     * instead -- a rotation takes far longer than this to complete and the user
     * cannot perceive the difference.
     *
     * READ THROUGH DisplayManager, NOT Context.getDisplay(). This class is
     * constructed with the application context, and on API 30+
     * ContextImpl.getDisplay() THROWS UnsupportedOperationException for any
     * non-visual context. The previous implementation swallowed that with
     * runCatching and fell back to the cached value, which is 0 forever -- so the
     * rotation correction silently never ran on any modern Android device, and the
     * sensor frame was published in the device's NATURAL orientation regardless of
     * how the handheld was being held.
     *
     * On a handheld whose natural orientation is portrait, that is a 90 degree
     * error about the screen normal: yaw survives it (it is rotation about the
     * axis that did not move) while pitch and roll are exchanged and inverted --
     * the exact reported AYN Thor symptom. DisplayManager.getDisplay() is valid
     * from any context and needs no permission.
     */
    private fun currentRotationDegrees(): Int {
        val now = SystemClock.elapsedRealtime()
        if (now - rotationCheckedAtMs >= ROTATION_REFRESH_MS) {
            rotationCheckedAtMs = now
            val measured = runCatching {
                displayManager?.getDisplay(Display.DEFAULT_DISPLAY)?.rotation
            }.getOrNull()
            if (measured != null) {
                rotationReadable = true
                cachedRotationDegrees = when (measured) {
                    Surface.ROTATION_90 -> 90
                    Surface.ROTATION_180 -> 180
                    Surface.ROTATION_270 -> 270
                    else -> 0
                }
            }
        }
        return cachedRotationDegrees
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor?.type) {
            Sensor.TYPE_GYROSCOPE -> {
                // TYPE_GYROSCOPE is rad/s in the device's natural frame, which is
                // exactly what MotionScale.gyroCounts expects.
                gyroX = MotionScale.gyroCounts(event.values[0])
                gyroY = MotionScale.gyroCounts(event.values[1])
                gyroZ = MotionScale.gyroCounts(event.values[2])
                rawGyroX = event.values[0]; rawGyroY = event.values[1]; rawGyroZ = event.values[2]
                // SensorEvent.timestamp is nanoseconds on the elapsed-realtime base.
                gyroTimestampTicks = MotionScale.timestampTicks(event.timestamp)
                sawGyro = true
            }
            Sensor.TYPE_ACCELEROMETER -> {
                // TYPE_ACCELEROMETER is m/s^2 and gravity-inclusive, which is what
                // the canonical convention requires.
                accelX = MotionScale.accelCounts(event.values[0])
                accelY = MotionScale.accelCounts(event.values[1])
                accelZ = MotionScale.accelCounts(event.values[2])
                rawAccelX = event.values[0]; rawAccelY = event.values[1]; rawAccelZ = event.values[2]
                sawAccel = true
            }
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    /** Release the sensor thread. The bridge owns this backend for the process lifetime. */
    fun close() {
        stop()
        sensorThread.quitSafely()
    }

    private companion object {
        // ~200 Hz requested; Android may deliver slower. The report cadence is
        // 125 Hz, so this keeps a fresh sample available for every report.
        const val SAMPLING_PERIOD_US = 5_000
        const val ROTATION_REFRESH_MS = 500L
    }
}
