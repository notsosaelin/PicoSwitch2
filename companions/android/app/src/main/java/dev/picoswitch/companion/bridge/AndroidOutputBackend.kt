package dev.picoswitch.companion.bridge

import android.content.Context
import android.media.AudioAttributes
import android.os.Build
import android.os.CombinedVibration
import android.os.SystemClock
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.Settings
import android.view.InputDevice
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.core.ControllerSourceIdentity
import dev.picoswitch.bridge.core.RumbleRequest
import dev.picoswitch.bridge.core.RumbleShaping
import dev.picoswitch.bridge.session.OutputBackend
import dev.picoswitch.bridge.session.OutputDiagnostics

/**
 * Which vibrator the app decided to drive, and why.
 *
 * Pure data so the decision can be asserted in a JVM test with no device.
 */
enum class HapticStage {
    /** InputDevice-scoped VibratorManager with at least one id (API 31+). */
    DeviceMulti,

    /** InputDevice-scoped single Vibrator (any API). */
    DeviceSingle,

    /** The phone's system vibrator. Only legitimate for BUILT-IN controls. */
    System,

    /** Nothing can be driven. */
    None,
}

/** What the app observed when it probed a controller for actuators. */
data class HapticProbe(
    val deviceVibratorIds: List<Int> = emptyList(),
    val deviceHasVibrator: Boolean = false,
    val deviceIsExternal: Boolean = false,
    val systemHasVibrator: Boolean = false,
    val devicePresent: Boolean = false,
    /**
     * `Settings.System.VIBRATE_ON`. Diagnostic ONLY -- it deliberately does not
     * affect routing, because the user can change it at any moment and a stale
     * read must never permanently disable output.
     *
     * It is surfaced because it is the single most likely reason a perfectly
     * routed effect produces no movement: AOSP discards every vibration from
     * every app when this is 0, for every usage except ACCESSIBILITY
     * (`VibrationSettings`: `if (!mVibrateOn && usage != USAGE_ACCESSIBILITY)
     * return IGNORED_FOR_SETTINGS`). Measured on the AYN Thor 2026-08-14: it was
     * **0**, which accounts for the entire "zero rumble, ever" history and for
     * every `ignored_for_settings` entry in `dumpsys vibrator_manager`.
     */
    val systemVibrationSettingOn: Boolean = true,
)

/**
 * Vibrator selection, kept pure.
 *
 * THE BUG THIS EXISTS TO PREVENT: the app used to call
 * `VibratorManager.getDefaultVibrator()` on the application context, which is the
 * *system* vibrator. A gamepad's motors are not there. AOSP implements the two as
 * disjoint stacks that share no lookup: `SystemVibratorManager` resolves ids
 * through `IVibratorManagerService`, while `InputDeviceVibratorManager` resolves
 * them through `InputManagerGlobal.getVibratorIds(deviceId)`. An input-device
 * actuator can never appear in `getDefaultVibrator()`, so a controller's motors
 * were unreachable by construction. `InputDevice.getVibrator()`'s own javadoc says
 * as much: "the vibrator associated with the device may be different from the
 * system vibrator".
 *
 * That also explains why the effects were silent rather than weak: a wrong-object
 * bug is binary. And it explains why the earlier USAGE_MEDIA fix, which was
 * correct on its own terms, did not help — `InputDeviceVibrator.vibrate()` calls
 * `InputManagerGlobal.vibrate()` and discards `VibrationAttributes` entirely, so
 * the settings gate that was eating our effects does not even exist on the path
 * we should have been using.
 *
 * The cascade mirrors Moonlight's, which is the most conservative of the mature
 * implementations: try the device, then the device's legacy single vibrator, and
 * fall back to the system vibrator ONLY for a non-external (built-in) controller.
 * Dolphin and current RetroArch are also InputDevice-scoped; RetroArch's old
 * system-vibrator-only implementation has a matching open bug (libretro#10338)
 * whose symptom is ours exactly.
 */
object HapticRouting {
    fun choose(probe: HapticProbe, sdkInt: Int): HapticStage = when {
        // getVibratorIds() is the existence test on this path. InputDeviceVibrator
        // hardcodes hasVibrator() to true, so it carries no capability signal.
        sdkInt >= Build.VERSION_CODES.S && probe.deviceVibratorIds.isNotEmpty() ->
            HapticStage.DeviceMulti
        // InputDevice.getVibrator() returns a NullVibrator when absent, so here
        // hasVibrator() IS meaningful.
        probe.deviceHasVibrator -> HapticStage.DeviceSingle
        // Otherwise the system vibrator, which on a handheld IS the grip motor.
        //
        // DO NOT gate this on isExternal. Moonlight does, and copying that was a
        // real regression here: measured on the AYN Thor 2026-08-14, its built-in
        // "Odin Controller" reports `Classes: KEYBOARD | GAMEPAD | JOYSTICK |
        // EXTERNAL` with `ids=[] hasVibrator=false`, so an isExternal veto
        // resolved to None and cut off the only actuator on the device. The
        // project's own ADB audit already recorded this rule --
        // docs/experiments/android-controller-ayn-thor-adb-audit-2026-08-12.md:
        // "do not reject isExternal == true or a virtual origin". Android's
        // EXTERNAL class means "not on the main board", not "not part of this
        // handheld".
        //
        // The theoretical cost of dropping the veto is buzzing the phone for a
        // genuinely detached pad that has no motor. That is a far smaller harm
        // than silently having no haptics at all, and it is visible in the
        // diagnostic line rather than silent.
        probe.systemHasVibrator -> HapticStage.System
        else -> HapticStage.None
    }

    /** One line that says what was tried and what each stage reported. */
    fun describe(probe: HapticProbe, stage: HapticStage): String = buildString {
        append(stage.name)
        append(" (device=")
        append(if (probe.devicePresent) "yes" else "none")
        append(" ids=")
        append(probe.deviceVibratorIds.joinToString(",", "[", "]"))
        append(" hasVibrator=").append(probe.deviceHasVibrator)
        append(" external=").append(probe.deviceIsExternal)
        append(" system=").append(probe.systemHasVibrator)
        append(" vibrateOnSetting=").append(probe.systemVibrationSettingOn)
        append(")")
    }

    /**
     * The reason a correctly routed effect may still never move anything. Separate
     * from [describe] because it answers a different question, and because it is
     * the answer the user can act on.
     */
    fun warning(probe: HapticProbe, stage: HapticStage): String? = when {
        !probe.systemVibrationSettingOn ->
            "System vibration is off (Settings.System.VIBRATE_ON=0); Android discards every " +
                "effect from every app regardless of routing"
        stage == HapticStage.None -> "No actuator could be bound; console rumble cannot reach hardware"
        else -> null
    }
}

/**
 * Bridge rumble requests -> Android actuators.
 *
 * The bridge asks for two motor amplitudes and nothing else. Everything in this
 * file is the Android-specific answer to "how do you actually produce that", and
 * none of it is protocol:
 *
 * 1. OBJECT. See [HapticRouting] — the controller's vibrator is not the phone's.
 *
 * 2. DURATION. Both the firmware and this app suppress unchanged values, so a game
 *    holding a constant amplitude sends exactly one report. A bounded one-shot
 *    therefore expired mid-effect and left the handheld silent until the console
 *    next changed the value. The effect repeats until explicitly cancelled, and the
 *    session's output watchdog guarantees the cancel.
 *
 * 3. RATE. Requests can change every few milliseconds. Android cancels a playing
 *    vibration before starting the next unless both are flagged pipelined, and
 *    that flag is not public, so every change is an audible stop/start. The
 *    retrigger rate is bounded, always using the newest value.
 *
 * On the system path only, `USAGE_MEDIA` is load bearing (a bare vibrate is
 * classified `USAGE_TOUCH`, which the system touch-feedback setting disables).
 * Note that `USAGE_MEDIA` is NOT in AOSP's `BACKGROUND_PROCESS_USAGE_ALLOWLIST`,
 * so on that path vibration is additionally dropped whenever this app is not
 * foreground. The device-scoped paths have neither restriction.
 */
class AndroidOutputBackend(
    context: Context,
    private val diagnostics: BridgeDiagnostics = BridgeDiagnostics.None,
) : OutputBackend {
    private val appContext = context.applicationContext

    private val systemVibrator: Vibrator? = run {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            appContext.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            appContext.getSystemService(Vibrator::class.java)
        }
    }
    private val systemHasVibrator =
        runCatching { systemVibrator?.hasVibrator() == true }.getOrDefault(false)

    @Volatile private var stage = HapticStage.None
    @Volatile private var deviceManager: VibratorManager? = null
    @Volatile private var deviceIds: IntArray = IntArray(0)
    @Volatile private var deviceVibrator: Vibrator? = null
    @Volatile private var amplitudeControl = false

    /** Which actuator is currently bound. Surfaced for tests and diagnostics. */
    val boundStage: HapticStage get() = stage

    @Volatile private var shapedLeft = 0
    @Volatile private var shapedRight = 0
    @Volatile private var playing = false
    @Volatile private var lastIssuedAtMs = 0L
    @Volatile private var reportedFirstPlay = false
    @Volatile private var lastProbe: HapticProbe? = null

    override fun diagnostics(): OutputDiagnostics {
        val probe = lastProbe ?: return OutputDiagnostics()
        return OutputDiagnostics(
            route = HapticRouting.describe(probe, stage) + " amplitudeControl=$amplitudeControl",
            motors = when (stage) {
                HapticStage.DeviceMulti -> deviceIds.size
                HapticStage.DeviceSingle, HapticStage.System -> 1
                HapticStage.None -> 0
            },
            warning = HapticRouting.warning(probe, stage),
        )
    }

    /**
     * `Settings.System.VIBRATE_ON`, read fresh. Defaults to "on" when unreadable:
     * assuming the user disabled vibration would be a worse failure than trying.
     */
    private fun systemVibrationSettingOn(): Boolean = runCatching {
        Settings.System.getInt(appContext.contentResolver, Settings.System.VIBRATE_ON, 1) != 0
    }.getOrDefault(true)

    /**
     * Bind to the actuators of the source the user selected.
     *
     * Resolves the identity's stable descriptor back to a live `InputDevice`,
     * because the correct actuator is a property of that device, not of the
     * application. See [HapticRouting].
     */
    override fun bindToSource(source: ControllerSourceIdentity?) =
        bind(source?.descriptor?.let(::deviceForDescriptor))

    private fun deviceForDescriptor(descriptor: String): InputDevice? = runCatching {
        InputDevice.getDeviceIds().asList().mapNotNull(InputDevice::getDevice)
            .firstOrNull { it.descriptor == descriptor }
    }.getOrNull()

    @Synchronized
    fun bind(device: InputDevice?) {
        stopLocked()
        deviceManager = null
        deviceIds = IntArray(0)
        deviceVibrator = null
        reportedFirstPlay = false

        var ids: IntArray = IntArray(0)
        var manager: VibratorManager? = null
        var single: Vibrator? = null
        var hasSingle = false
        var external = false

        if (device != null) {
            external = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                runCatching { device.isExternal }.getOrDefault(false)
            } else {
                false
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                runCatching {
                    val vm = device.vibratorManager
                    val vibratorIds = vm.vibratorIds
                    if (vibratorIds.isNotEmpty()) {
                        manager = vm
                        ids = vibratorIds
                    }
                }
            }
            runCatching {
                @Suppress("DEPRECATION")
                val v = device.vibrator
                if (v != null && v.hasVibrator()) {
                    single = v
                    hasSingle = true
                }
            }
        }

        val probe = HapticProbe(
            deviceVibratorIds = ids.toList(),
            deviceHasVibrator = hasSingle,
            deviceIsExternal = external,
            systemHasVibrator = systemHasVibrator,
            devicePresent = device != null,
            systemVibrationSettingOn = systemVibrationSettingOn(),
        )
        stage = HapticRouting.choose(probe, Build.VERSION.SDK_INT)
        lastProbe = probe
        when (stage) {
            // The API guard is redundant with HapticRouting.choose(), which cannot
            // return DeviceMulti below S -- but the invariant lives in another
            // object, so state it here too rather than suppressing the warning.
            HapticStage.DeviceMulti -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                deviceManager = manager
                deviceIds = ids
                amplitudeControl = runCatching {
                    ids.all { manager?.getVibrator(it)?.hasAmplitudeControl() == true }
                }.getOrDefault(false)
            }
            HapticStage.DeviceSingle -> {
                deviceVibrator = single
                amplitudeControl =
                    runCatching { single?.hasAmplitudeControl() == true }.getOrDefault(false)
            }
            HapticStage.System -> {
                deviceVibrator = systemVibrator
                amplitudeControl =
                    runCatching { systemVibrator?.hasAmplitudeControl() == true }
                        .getOrDefault(false)
            }
            HapticStage.None -> amplitudeControl = false
        }

        // One line that answers "which actuator, and what did every other stage
        // report" without a dumpsys round trip.
        diagnostics.event(
            "controller", "haptics bound",
            HapticRouting.describe(probe, stage) + " amplitudeControl=" + amplitudeControl,
        )
    }

    @Synchronized
    override fun apply(request: RumbleRequest) {
        if (stage == HapticStage.None) return
        val nextLeft = RumbleShaping.shape(request.left, shapedLeft)
        val nextRight = RumbleShaping.shape(request.right, shapedRight)
        if (nextLeft == shapedLeft && nextRight == shapedRight) return
        val now = SystemClock.elapsedRealtime()
        val stopping = nextLeft == 0 && nextRight == 0
        // A stop is always immediate; only ramping up or changing level waits.
        if (!stopping && now - lastIssuedAtMs < MIN_RETRIGGER_MS) return
        shapedLeft = nextLeft
        shapedRight = nextRight
        lastIssuedAtMs = now
        issue(nextLeft, nextRight)
    }

    /**
     * The vibration repeats indefinitely, so this exists purely so a bridge that
     * has gone quiet cannot leave the actuator running: the session ticks this
     * while the link is live, and [stop] is still called on every teardown path.
     */
    @Synchronized
    override fun keepAlive() {
        if (!playing) return
        if (SystemClock.elapsedRealtime() - lastIssuedAtMs > WATCHDOG_MS) stop()
    }

    private fun effectFor(amplitude: Int): VibrationEffect {
        val level = if (amplitudeControl) amplitude else VibrationEffect.DEFAULT_AMPLITUDE
        // Repeat from index 0 until cancelled. Consecutive segments carry the same
        // amplitude, so looping is not an audible off/on.
        return VibrationEffect.createWaveform(longArrayOf(SEGMENT_MS), intArrayOf(level), 0)
    }

    private fun issue(left: Int, right: Int) {
        runCatching {
            if (left == 0 && right == 0) {
                cancelAll()
                playing = false
                return@runCatching
            }
            when (stage) {
                HapticStage.DeviceMulti -> {
                    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return@runCatching
                    val manager = deviceManager ?: return@runCatching
                    val combined = if (deviceIds.size >= 2) {
                        // Left motor first, matching the platform's own
                        // multi-channel controller convention.
                        CombinedVibration.startParallel()
                            .addVibrator(deviceIds[0], effectFor(left))
                            .addVibrator(deviceIds[1], effectFor(right))
                            .combine()
                    } else {
                        CombinedVibration.createParallel(effectFor(maxOf(left, right)))
                    }
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        manager.vibrate(
                            combined,
                            VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA),
                        )
                    } else {
                        manager.vibrate(combined)
                    }
                }
                else -> {
                    val device = deviceVibrator ?: return@runCatching
                    vibrateAsMedia(device, effectFor(maxOf(left, right)))
                }
            }
            playing = true
            if (!reportedFirstPlay) {
                reportedFirstPlay = true
                // The call returning without throwing is NOT proof the actuator
                // moved -- vibrate() is fire-and-forget. This marks the last point
                // the app can observe.
                diagnostics.event(
                    "controller", "vibration issued",
                    "stage=$stage L=$left R=$right amplitudeControl=$amplitudeControl",
                )
            }
        }.onFailure { error ->
            diagnostics.error("controller", "vibration", error)
        }
    }

    /**
     * Classify as media vibration: "game, or any interactive media that isn't touch
     * feedback specifically" is exactly this. Without it the effect is treated as
     * touch feedback and silently discarded on any device where the user has that
     * turned off. Load bearing on the system path only.
     */
    private fun vibrateAsMedia(device: Vibrator, effect: VibrationEffect) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            device.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA))
        } else {
            @Suppress("DEPRECATION")
            // No CONTENT_TYPE_SONIFICATION here: VibrationAttributes.Builder
            // (AudioAttributes) maps sonification to USAGE_TOUCH, which is the
            // exact classification this branch exists to avoid.
            device.vibrate(
                effect,
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .build(),
            )
        }
    }

    private fun cancelAll() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            runCatching { deviceManager?.cancel() }
        }
        runCatching { deviceVibrator?.cancel() }
    }

    @Synchronized
    override fun stop() = stopLocked()

    private fun stopLocked() {
        shapedLeft = 0
        shapedRight = 0
        playing = false
        lastIssuedAtMs = 0L
        cancelAll()
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
