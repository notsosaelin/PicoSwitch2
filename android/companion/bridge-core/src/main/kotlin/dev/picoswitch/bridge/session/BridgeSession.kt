package dev.picoswitch.bridge.session

import dev.picoswitch.bridge.core.BridgeCounters
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.core.BridgeOutput
import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.ControllerSourceIdentity
import dev.picoswitch.bridge.core.ControllerState
import dev.picoswitch.bridge.core.DeviceCapabilities
import dev.picoswitch.bridge.core.LatestReportMailbox
import dev.picoswitch.bridge.protocol.BridgeOutputCodec
import dev.picoswitch.bridge.protocol.ControllerReportEncoder
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * The reusable center of the PicoSwitch Bridge.
 *
 * Everything here is the same on every host platform, which is the test used to
 * decide what belongs:
 *
 * - composing a complete report from live input plus current motion and battery;
 * - the send cadence, and the rule that it is time-driven while motion streams
 *   and change-driven otherwise;
 * - gating the IMU on the console's real demand for it;
 * - applying rumble and the player indicator;
 * - polling battery on a slow timer;
 * - neutralizing on every boundary;
 * - report accounting and the observable [BridgeState].
 *
 * Nothing here knows how the host obtains input, motion or output, or how the
 * link is carried. Those are [BridgeTransport], [MotionBackend], [BatteryBackend]
 * and [OutputBackend].
 *
 * @param clock injected so the cadence and battery timers are testable without
 *   real time.
 */
class BridgeSession(
    private val transport: BridgeTransport,
    private val input: ControllerInputState,
    private val motion: MotionBackend = MotionBackend.None,
    private val battery: BatteryBackend = BatteryBackend.None,
    private val output: OutputBackend = OutputBackend.None,
    private val diagnostics: BridgeDiagnostics = BridgeDiagnostics.None,
    dispatcher: CoroutineDispatcher = Dispatchers.Default,
    private val clock: () -> Long = { System.currentTimeMillis() },
    /**
     * Boundary counters, INJECTED so the transport can count its own callbacks
     * into the same set -- one ordered picture of the whole path. Constructing a
     * second set here instead would produce two half-pictures that never
     * disagree visibly, which is precisely the class of bug these exist to find.
     */
    val counters: BridgeCounters = BridgeCounters(),
) : BridgeTransport.Listener {

    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    private val _state = MutableStateFlow(BridgeState())
    val state: StateFlow<BridgeState> = _state.asStateFlow()

    /** One latest-state mailbox prevents old motion reports surviving behind newer input. */
    private val outgoing = LatestReportMailbox<ControllerState>()

    @Volatile private var feedback = BridgeOutput.None
    @Volatile private var batteryReading = ControllerBattery.Unknown
    @Volatile private var linked = false
    @Volatile private var stopped = false
    @Volatile private var sourceCapabilities = DeviceCapabilities.None

    private var sender: Job? = null
    private var inputCollector: Job? = null
    private var outputWatchdog: Job? = null

    /** Whatever the transport actually holds as its listener, for the audit above. */
    private var attachedListener: Any? = null

    init {
        transport.attach(this)
        attachedListener = transport.attachedListener()
        // Host capabilities are knowable before any source is chosen, and the UI
        // needs them then: "this device has no gyroscope" is exactly the thing to
        // say on an empty controller screen.
        sourceCapabilities = withHostCapabilities(DeviceCapabilities.None)
        _state.value = BridgeState(
            capabilities = sourceCapabilities,
            output = output.diagnostics(),
        )
    }

    /**
     * Fill in the half of the capability model the SESSION owns.
     *
     * The input backend knows what the selected source has; only the session's own
     * backends know what the host has. Merging in one place stops the two halves
     * being answered inconsistently.
     */
    private fun withHostCapabilities(source: DeviceCapabilities) = source.copy(
        gyroscope = motion.available,
        accelerometer = motion.available,
        rumbleMotors = output.diagnostics().motors,
        battery = battery.available,
    )

    // ---------------------------------------------------------------- lifecycle

    fun knownHosts(): List<BridgeHost> = transport.knownHosts()

    fun start(preferredHost: BridgeHost? = null) {
        stopped = false
        transport.start(preferredHost)
    }

    fun connect(host: BridgeHost) {
        stopped = false
        transport.connect(host)
    }

    fun stop() {
        stopped = true
        // Best effort neutral BEFORE teardown: if the link is still up, this is
        // the last chance to clear a held input from the console.
        if (linked) {
            transport.send(
                ControllerReportEncoder.REPORT_ID,
                ControllerReportEncoder.encode(ControllerState.Neutral),
            )
        }
        transport.stop()
        endSession()
        _state.value = BridgeState(capabilities = sourceCapabilities)
        diagnostics.event("bridge", "session stopped")
    }

    fun close() {
        stop()
        transport.close()
        scope.cancel()
    }

    /** Clear held input and push one neutral report. Used on focus loss. */
    fun neutralize() {
        input.neutralize()
        outgoing.drain()
        if (!linked) return
        recordReport(
            transport.send(
                ControllerReportEncoder.REPORT_ID,
                ControllerReportEncoder.encode(ControllerState.Neutral),
            ),
        )
    }

    // ---------------------------------------------------------------- app-facing

    /**
     * Tell the session what the selected source can do, and point the output
     * backend at that source's actuators.
     *
     * One call rather than two because they must never disagree: binding output
     * to a device while publishing another device's capabilities is exactly the
     * state where rumble silently goes nowhere.
     */
    fun bindSource(identity: ControllerSourceIdentity?, capabilities: DeviceCapabilities) {
        // Bind output FIRST: withHostCapabilities reads the actuator count the bind
        // just resolved, and publishing the previous device's motor count is the
        // state where rumble silently goes nowhere.
        output.bindToSource(identity)
        sourceCapabilities = withHostCapabilities(capabilities)
        _state.value = _state.value.copy(
            capabilities = sourceCapabilities,
            output = output.diagnostics(),
        )
        diagnostics.event(
            "bridge", "source bound",
            "${identity?.name ?: "none"} caps=$sourceCapabilities",
        )
    }

    /**
     * Pull the current layered motion sample text.
     *
     * On demand ONLY — call it when a diagnostics surface is actually open or an
     * export is being written, never on the report path. Rendering live sensor
     * values costs a formatted string per call, and doing that at report cadence
     * is what previously starved motion. See [publishMotionDiagnostics].
     */
    fun refreshMotionDiagnostics() {
        _state.value = _state.value.copy(motion = motion.diagnostics())
    }

    /**
     * Identity of every collaborator THIS session actually holds.
     *
     * The assembler logs its own view of the graph and this one side by side. If
     * they disagree, something was wired to a second instance — a refactor can
     * compile perfectly while attaching half the application to an object nothing
     * else uses, and that failure is invisible to unit tests that construct the
     * graph themselves.
     */
    fun wiring(): String =
        "session=${id(this)} transport=${id(transport)} input=${id(input)} " +
            "motion=${id(motion)} battery=${id(battery)} output=${id(output)} " +
            "counters=${id(counters)} listenerAttached=${id(attachedListener ?: "none")}"

    private fun id(value: Any) = Integer.toHexString(System.identityHashCode(value))

    /** Republish the platform's own view of where output is going. */
    fun refreshOutputStatus() {
        sourceCapabilities = withHostCapabilities(sourceCapabilities)
        _state.value = _state.value.copy(
            capabilities = sourceCapabilities,
            output = output.diagnostics(),
        )
    }

    // ------------------------------------------------------- transport listener

    override fun onPhase(phase: BridgeLinkPhase, hostName: String?, message: String?, registered: Boolean) {
        if (stopped && phase != BridgeLinkPhase.Idle) return
        // Preparing is the start of a fresh acquisition, so per-link counters reset
        // there rather than accumulating across reconnects. Every other phase is a
        // transition within one attempt and keeps them.
        val base = if (phase == BridgeLinkPhase.Preparing || phase == BridgeLinkPhase.Unsupported) {
            BridgeState(capabilities = sourceCapabilities, output = output.diagnostics())
        } else {
            _state.value
        }
        _state.value = base.copy(
            phase = phase,
            hostName = hostName ?: base.hostName,
            message = message,
            registered = registered,
        )
    }

    override fun onLinkUp(hostName: String?) {
        if (stopped) return
        linked = true
        _state.value = _state.value.copy(
            phase = BridgeLinkPhase.Playing,
            hostName = hostName,
            message = "Input is streaming",
            registered = true,
            capabilities = sourceCapabilities,
        )
        diagnostics.event("bridge", "link up", "interrupt link ready")
        startSender()
    }

    override fun onLinkDown(message: String?) {
        val wasLinked = linked
        endSession()
        _state.value = BridgeState(
            phase = BridgeLinkPhase.Idle,
            message = message ?: "Controller link disconnected",
            capabilities = sourceCapabilities,
            output = output.diagnostics(),
        )
        if (wasLinked) diagnostics.event("bridge", "link down", message ?: "")
    }

    override fun onOutputReport(data: ByteArray?, reportId: Int?) {
        val decoded = BridgeOutputCodec.decode(data, reportId)
        if (decoded == null) counters.outputFramesRejected.incrementAndGet()
        else counters.outputFramesDecoded.incrementAndGet()
        applyOutput(decoded)
    }

    override fun currentReport(): ByteArray = buildReport(input.state.value)

    // ------------------------------------------------------------------ internals

    /** Apply one decoded output report: rumble, player indicator, motion gate. */
    private fun applyOutput(decoded: BridgeOutput?) {
        if (decoded == null || stopped) return
        counters.sessionOutputApplied.incrementAndGet()
        val previous = feedback
        feedback = decoded
        if (!decoded.rumble.silent) counters.rumbleRequestsProduced.incrementAndGet()

        // Log the zero/non-zero edges only. This is the bridge's proof that the
        // adapter's rumble actually crossed the link -- separate from whether the
        // host's actuator then moved, which the output backend reports.
        if (decoded.rumble.silent != previous.rumble.silent) {
            diagnostics.event(
                "bridge", "rumble received",
                "L=${decoded.rumble.left} R=${decoded.rumble.right}",
            )
        }
        output.apply(decoded.rumble)

        if (decoded.motionRequested != previous.motionRequested) {
            // On demand only: the console tells us when motion is actually being
            // consumed, so an idle host is not draining its battery on sensors.
            counters.motionWantedTransitions.incrementAndGet()
            if (decoded.motionRequested) motion.start() else motion.stop()
            diagnostics.event(
                "bridge", "motion",
                if (decoded.motionRequested) "console requested motion" else "console stopped motion",
            )
        }
        if (decoded.playerIndicator != previous.playerIndicator) {
            diagnostics.event("bridge", "player indicator", decoded.playerIndicator.toString())
        }
        _state.value = _state.value.copy(
            playerIndicator = decoded.playerIndicator,
            rumbleAmplitude = decoded.rumble.strongest,
            motionActive = decoded.motionRequested && motion.available,
        )
    }

    /** Compose the wire report: live input plus the current motion and battery. */
    private fun buildReport(state: ControllerState): ByteArray {
        val sample = if (feedback.motionRequested) motion.sample() else ControllerMotion.None
        if (sample.valid) counters.motionSamplesValid.incrementAndGet()
        val reading = batteryReading
        if (sample.valid) counters.reportsWithMotionBlock.incrementAndGet()
        if (reading.valid) counters.reportsWithBatteryBlock.incrementAndGet()
        return ControllerReportEncoder.encode(state.copy(motion = sample, battery = reading))
    }

    private fun startSender() {
        startOutputWatchdog()
        sender?.cancel()
        inputCollector?.cancel()
        inputCollector = scope.launch {
            input.state.collect { snapshot -> outgoing.offer(snapshot) }
        }
        sender = scope.launch {
            var previous: ControllerState? = null
            // Null, not 0: the first iteration must always poll. Seeding with 0
            // only worked because the wall clock happens to be a large number,
            // which is an accident of the epoch rather than a stated rule.
            var lastBatteryPoll: Long? = null
            while (isActive) {
                // While motion is live the report carries a fresh IMU sample every
                // interval, so the loop is time-driven; with motion off it stays
                // change-driven exactly as the validated v1 path was.
                //
                // Both the fetch and the send decision use this ONE value: if the
                // console asked for motion but the host has no IMU, a non-blocking
                // fetch with a change-driven send would spin without ever
                // suspending or delaying.
                val timeDriven = feedback.motionRequested && motion.available
                val snapshot = if (timeDriven) {
                    outgoing.poll() ?: input.state.value
                } else {
                    outgoing.receive()
                }

                if (timeDriven) publishMotionDiagnostics()

                val now = clock()
                if (lastBatteryPoll == null || now - lastBatteryPoll >= BATTERY_POLL_MS) {
                    lastBatteryPoll = now
                    batteryReading = battery.read()
                    if (batteryReading.valid) counters.batterySamplesValid.incrementAndGet()
                    if (batteryReading.valid && batteryReading.levelPercent != _state.value.batteryPercent) {
                        _state.value = _state.value.copy(batteryPercent = batteryReading.levelPercent)
                    }
                }

                if (!timeDriven && snapshot == previous) continue
                previous = snapshot
                if (!linked) continue
                recordReport(
                    transport.send(ControllerReportEncoder.REPORT_ID, buildReport(snapshot)),
                )
                // Coalesce axis motion to the documented 125 Hz ceiling. Button edges
                // can wait at most one interval and the conflated mailbox always
                // retains the newest state.
                delay(REPORT_INTERVAL_MS)
            }
        }
    }

    /**
     * Rumble may repeat until cancelled on some hosts, so something has to notice
     * if this session stops servicing output. Deliberately its own low-rate timer
     * rather than a hook in the sender loop: with motion off that loop blocks
     * waiting for input, so it would false-trip during ordinary idle.
     */
    private fun startOutputWatchdog() {
        outputWatchdog?.cancel()
        outputWatchdog = scope.launch {
            var tick = 0L
            var lastDivergence: String? = null
            while (isActive) {
                delay(OUTPUT_WATCHDOG_TICK_MS)
                if (!linked) output.stop() else output.keepAlive()
                // Periodic counter line so a missing feature is diagnosable over
                // ADB alone, without asking anyone to export anything. Low rate on
                // purpose: this is a progression check, not a trace.
                if (!linked) continue
                tick++
                // Divergence is checked often and reported the moment it appears;
                // the healthy summary is rare. Logging a healthy snapshot every
                // 10 s is what buried the evidence last time.
                val divergence = counters.firstDivergence()
                if (divergence != null && divergence != lastDivergence) {
                    lastDivergence = divergence
                    diagnostics.event("bridge", "FIRST DIVERGENCE", divergence)
                    diagnostics.event("bridge", "counters", counters.snapshot())
                } else if (tick % COUNTER_SUMMARY_TICKS == 0L) {
                    diagnostics.event("bridge", "counters", counters.snapshot())
                }
            }
        }
    }

    /**
     * Publish the layered motion view.
     *
     * Cheap (backends cache the frame on a slow timer) and only written through
     * when it changes, so this costs nothing at the report cadence. It exists
     * because "aim is rotated" is uninterpretable without knowing which frame
     * correction was applied -- and because a correction that cannot be read at
     * all is itself the defect.
     */
    private fun publishMotionDiagnostics() {
        val next = motion.diagnostics()
        val current = _state.value
        // ONLY the cheap fields, and ONLY on change.
        //
        // This runs at the 125 Hz report cadence. An earlier version compared the
        // whole MotionDiagnostics, whose sample text changes every single frame, so
        // it wrote the observable state 125 times a second. That is not a cosmetic
        // cost: the state drives the application's UI, and on Android sensor
        // callbacks are delivered on the same main thread the resulting
        // recomposition storm saturates -- so publishing diagnostics starved the
        // very motion it was reporting on. Sample text is pulled on demand instead,
        // by whoever is actually looking (see refreshMotionDiagnostics).
        if (next.frameRotationDegrees == current.motion.frameRotationDegrees &&
            next.frameRotationMeasured == current.motion.frameRotationMeasured
        ) {
            return
        }
        _state.value = current.copy(motion = next)
        diagnostics.event(
            "bridge", "motion frame",
            if (next.frameRotationMeasured) "rotation ${next.frameRotationDegrees}deg"
            else "rotation unreadable; assuming 0",
        )
    }

    /**
     * Stop the actuators, unregister the sensors, and forget the adapter's last
     * request. Called on every teardown path so a dropped link, a stop, or a
     * transport loss can never leave the host vibrating or streaming an IMU
     * nothing is reading.
     */
    private fun endSession() {
        linked = false
        sender?.cancel(); sender = null
        inputCollector?.cancel(); inputCollector = null
        outputWatchdog?.cancel(); outputWatchdog = null
        outgoing.drain()
        feedback = BridgeOutput.None
        output.stop()
        motion.stop()
        input.neutralize()
    }

    private fun recordReport(ok: Boolean) {
        if (!ok) {
            _state.value = _state.value.copy(message = "The host rejected the latest input report")
            diagnostics.event("bridge", "report rejected")
            return
        }
        counters.reportsSent.incrementAndGet()
        val count = _state.value.reportCount + 1
        _state.value = _state.value.copy(reportCount = count, lastReportAtMillis = clock())
        // First report only. The per-100 line was ~1/s at the report cadence and
        // rolled the logcat ring out from under the last investigation; the
        // periodic counters line below carries the same information at 1/10 s.
        if (count == 1L) diagnostics.event("bridge", "reports sent", "streaming started")
    }

    companion object {
        /** 125 Hz ceiling; see the sender loop. */
        const val REPORT_INTERVAL_MS = 8L
        const val BATTERY_POLL_MS = 30_000L
        const val OUTPUT_WATCHDOG_TICK_MS = 250L

        /** 480 x 250 ms = one healthy summary every 2 minutes. Divergence is immediate. */
        private const val COUNTER_SUMMARY_TICKS = 480L
    }
}
