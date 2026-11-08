package dev.picoswitch.management.audio

import dev.picoswitch.management.CompanionAudioState
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

enum class CompanionAudioPhase { Unavailable, Off, Starting, Playing, Refused, Ended, Error }

/**
 * @property controllerLink whether this device is the console's controller right
 * now. Companion audio is that controller's headset jack, so it turns on only
 * while it is.
 * @property awaitingConsole playing, but the console has not opened its speaker
 * stream yet. It does once it has registered the controller, which takes one
 * input: a button press or a stick movement.
 * @property microphone this device's microphone is live: the adapter shows the
 * console a headset (not headphones) and receives the uplink.
 * @property microphoneDetail why the microphone the player asked for is not live.
 */
data class CompanionAudioView(
    val phase: CompanionAudioPhase,
    val detail: String? = null,
    val controllerLink: Boolean = true,
    val awaitingConsole: Boolean = false,
    val microphone: Boolean = false,
    val microphoneDetail: String? = null,
    val muted: Boolean = false,
) {
    val on: Boolean get() = phase == CompanionAudioPhase.Starting || phase == CompanionAudioPhase.Playing

    /** Turning off is always allowed; turning on needs Controller Link. */
    val canToggle: Boolean
        get() = phase != CompanionAudioPhase.Unavailable && phase != CompanionAudioPhase.Starting &&
            (on || controllerLink)
}

/** The management commands companion audio uses. */
interface CompanionAudioControl {
    /**
     * `caudio start` or `caudio start mic`. On a running session it changes the
     * accessory in place (headphones/headset re-plug).
     */
    suspend fun start(microphone: Boolean): CompanionAudioState
    suspend fun stop(): CompanionAudioState
    suspend fun status(): CompanionAudioState
}

/** The audio notification characteristic on the management link. */
interface CompanionAudioDownlink {
    val attMtu: Int

    /** Subscribe; [onFrame] runs on the transport's thread for each notification. */
    suspend fun open(onFrame: (ByteArray) -> Unit): Boolean

    suspend fun close()
}

/** Local playback. Owns a bounded buffer driven by [CompanionAudioPlayout]. */
interface CompanionAudioSink {
    fun start(sampleRate: Int, channels: Int)

    /** Queue one block of [samples] interleaved samples; false when dropped. */
    fun write(pcm: ShortArray, samples: Int): Boolean

    fun stop()

    val underruns: Long
    val prebufferMillis: Int

    /** True once after the playout asked for a concealment block. */
    fun takeConcealRequest(): Boolean
}

data class CompanionAudioMetrics(
    val framesDecoded: Long = 0,
    val framesLost: Long = 0,
    val framesConcealed: Long = 0,
    val framesStale: Long = 0,
    val framesMalformed: Long = 0,
    val decodeErrors: Long = 0,
    val sinkDrops: Long = 0,
    val sinkUnderruns: Long = 0,
    val prebufferMillis: Int = 0,
    val bufferGrowths: Long = 0,
)

/**
 * Companion audio (EXPERIMENTAL) on Android: plays the console's audio on this
 * device, as the headset of the controller this device IS. The Kotlin twin of
 * the Windows `CompanionAudioService`; the same rules:
 *
 * - On asks the adapter to advertise a headphone jack, so the console routes its
 *   audio to the adapter's USB speaker stream; the adapter encodes it as Opus and
 *   notifies it here over the management link.
 * - It starts only while Controller Link is up and ends when it stops; the
 *   adapter enforces the same rule (`ns2_companion_audio.h`, SCOPE).
 * - Management loss tears the local half down; the adapter ends its half itself.
 * - The data plane has no end-of-stream message, so after [SILENCE_BEFORE_CHECK_MILLIS]
 *   without a frame the session asks `caudio status` once; an inactive session is
 *   ended locally with the adapter's reason. Silence alone ends nothing.
 * - Microphone: with [setMicrophone] on, the session starts as `caudio start mic`
 *   (a headset) and a [CompanionMicStream] sends this device's microphone up.
 *   Changing it while playing re-plugs the accessory without ending the session.
 *   Any microphone failure falls back to headphones and says why in
 *   [CompanionAudioView.microphoneDetail]: the microphone never costs the speaker.
 */
class CompanionAudioSession(
    private val scope: CoroutineScope,
    private val control: CompanionAudioControl,
    private val downlinks: () -> CompanionAudioDownlink?,
    private val sinks: () -> CompanionAudioSink?,
    private val decoders: () -> CompanionAudioDecoder,
    private val managementReady: StateFlow<Boolean>,
    private val controllerLink: StateFlow<Boolean>,
    private val log: (String) -> Unit = {},
    private val clockMillis: () -> Long = System::currentTimeMillis,
    private val runWatchdog: Boolean = true,
    private val micUplinks: () -> CompanionMicUplink? = { null },
    private val micCaptures: () -> CompanionMicCapture? = { null },
    private val micEncoders: () -> CompanionMicEncoder? = { null },
) {
    private val lifecycle = Mutex()
    private val receiveGate = Any()
    private val mutableView = MutableStateFlow(initialView())

    private var downlink: CompanionAudioDownlink? = null
    private var sink: CompanionAudioSink? = null
    private var receiver: CompanionAudioReceiver? = null
    private var sinkDrops = 0L
    private var watchdog: Job? = null

    @Volatile private var lastFrameMillis = 0L
    @Volatile private var awaitingConsole = false
    @Volatile private var finalMetrics = CompanionAudioMetrics()
    @Volatile private var micStream: CompanionMicStream? = null
    @Volatile private var finalMicMetrics = CompanionMicMetrics()
    @Volatile private var microphoneEnabled = false
    @Volatile private var microphoneMuted = false

    /**
     * The player's intent, apart from whether a session runs right now: a
     * headset stays plugged in when Controller Link or management drops, so
     * the session re-plugs by itself when both are back. Only [stop] (the
     * player unplugging) or an adapter-side end for another reason clears it.
     */
    @Volatile private var pluggedIn = false
    private var replugJob: Job? = null
    private var microphoneDetail: String? = null

    val view: StateFlow<CompanionAudioView> = mutableView.asStateFlow()

    val metrics: CompanionAudioMetrics get() = synchronized(receiveGate) { receiver?.let { snapshot(it) } ?: finalMetrics }

    init {
        scope.launch {
            combine(managementReady, controllerLink) { ready, link -> ready to link }
                .distinctUntilChanged()
                .collect { (ready, link) -> onConditionsChanged(ready, link) }
        }
    }

    val micMetrics: CompanionMicMetrics get() = micStream?.metrics ?: finalMicMetrics

    /** Whether the player wants this device's microphone in the headset. */
    val microphoneWanted: Boolean get() = microphoneEnabled

    suspend fun toggle() = if (mutableView.value.on) stop() else start()

    /**
     * Mute or unmute the headset's microphone without touching the accessory:
     * no adapter command, no re-plug, so the speaker never drops. Applies to the
     * next session too.
     */
    fun setMuted(muted: Boolean) {
        microphoneMuted = muted
        micStream?.muted = muted
        val current = mutableView.value
        if (current.phase == CompanionAudioPhase.Playing) publishPlaying()
        log("microphone: ${if (muted) "muted" else "unmuted"}")
    }

    /**
     * Set the microphone preference. While playing, re-plugs the accessory on the
     * adapter and opens or closes the uplink; otherwise it applies to the next start.
     */
    suspend fun setMicrophone(enabled: Boolean) = lifecycle.withLock {
        microphoneEnabled = enabled
        microphoneDetail = null
        if (downlink == null) {
            mutableView.value = mutableView.value.copy(microphoneDetail = null)
            return@withLock
        }
        if (enabled == (micStream != null)) {
            publishPlaying()
            return@withLock
        }
        if (!enabled) {
            closeMic()
            restartAccessory(microphone = false)
            publishPlaying()
            return@withLock
        }
        val state = try {
            control.start(microphone = true)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            microphoneDetail = "The adapter did not answer the microphone request: ${error.message}"
            publishPlaying()
            return@withLock
        }
        if (state.active && state.mic) {
            openMic()
        } else if (state.active) {
            microphoneDetail = refusalText(state)
        }
        publishPlaying()
    }

    /** Plug the headset in: start now, and keep it plugged in across link drops. */
    suspend fun start() {
        pluggedIn = true
        plugIn()
    }

    /**
     * Re-plug after Controller Link (and management) came back. Bounded: the
     * adapter can refuse for a moment while it binds the returning link
     * (admit=no_controller_link), so a refusal is retried a few times.
     */
    private suspend fun replug() {
        repeat(REPLUG_ATTEMPTS) { attempt ->
            if (!pluggedIn || !managementReady.value || !controllerLink.value) return
            if (downlink != null) return
            if (attempt > 0) delay(REPLUG_RETRY_MILLIS)
            log("re-plug: attempt=${attempt + 1}")
            plugIn()
            if (mutableView.value.phase != CompanionAudioPhase.Refused) return
        }
    }

    private suspend fun plugIn() = lifecycle.withLock {
        if (downlink != null) return@withLock
        if (!managementReady.value) {
            mutableView.value = initialView()
            return@withLock
        }
        if (!controllerLink.value) {
            mutableView.value = initialView()
            return@withLock
        }
        mutableView.value = CompanionAudioView(CompanionAudioPhase.Starting)
        microphoneDetail = null

        val state = try {
            val wantMic = microphoneEnabled
            val first = control.start(wantMic)
            if (wantMic && !first.active && first.admit == "mic_unsupported") {
                // The speaker is not the microphone's hostage: play without it.
                microphoneDetail = refusalText(first)
                control.start(microphone = false)
            } else {
                first
            }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            fail("The adapter did not answer the audio request: ${error.message}")
            return@withLock
        }
        if (!state.active) {
            mutableView.value = CompanionAudioView(CompanionAudioPhase.Refused, refusalText(state), controllerLink.value)
            log("refused: admit=${state.admit}")
            return@withLock
        }
        geometryProblem(state)?.let {
            tellAdapterStop()
            fail(it)
            return@withLock
        }

        val output = sinks()
        if (output == null) {
            tellAdapterStop()
            fail("No audio output is available on this device.")
            return@withLock
        }
        try {
            output.start(CompanionAudioDataPlane.SAMPLE_RATE, CompanionAudioDataPlane.CHANNELS)
        } catch (error: Exception) {
            tellAdapterStop()
            fail("Could not open the audio output: ${error.message}")
            return@withLock
        }
        synchronized(receiveGate) {
            sink = output
            sinkDrops = 0
            receiver = CompanionAudioReceiver(decoders(), ::emit, state.frameBytes)
        }

        lastFrameMillis = clockMillis()
        awaitingConsole = !state.usbSpeakerActive
        val plane = downlinks()
        val opened = try {
            plane?.open(::onFrame) == true
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            false
        }
        if (plane == null || !opened) {
            plane?.close()
            tearDownSink()
            tellAdapterStop()
            fail("This adapter's firmware does not expose the audio channel.")
            return@withLock
        }
        downlink = plane
        if (runWatchdog) {
            watchdog = scope.launch {
                while (isActive) {
                    delay(WATCHDOG_PERIOD_MILLIS)
                    checkAdapterSession()
                }
            }
        }
        if (state.mic) openMic()
        publishPlaying()
        log(
            "playing: att_mtu=${plane.attMtu} frame_bytes=${state.frameBytes} " +
                "kbps=${CompanionAudioDataPlane.kbps(state.frameBytes)} usb_speaker=${state.usbSpeakerActive} " +
                "mic=${micStream != null}",
        )
    }

    suspend fun stop() = lifecycle.withLock {
        pluggedIn = false
        replugJob?.cancel()
        replugJob = null
        val hadSession = tearDownLocal()
        if (hadSession || mutableView.value.phase == CompanionAudioPhase.Error) tellAdapterStop()
        mutableView.value = initialView()
    }

    /** The silence watchdog's check; the loop runs it every second while playing. */
    suspend fun checkAdapterSession() {
        if (downlink == null || clockMillis() - lastFrameMillis < SILENCE_BEFORE_CHECK_MILLIS) return
        val state = try {
            control.status()
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            return // a busy or lost channel is handled by its own lifecycle
        }
        lastFrameMillis = clockMillis()
        // Silence with the session still open: say which side is quiet. The
        // adapter's counters (subscribed, sent, busy, failed) against what
        // arrived here separate "not subscribed / not sending" from "sent but
        // not delivered" without a firmware build.
        val m = metrics
        log(
            "silence: adapter active=${state.active} subscribed=${state.subscribed} " +
                "usb=${state.usbSpeakerActive} sent=${state.framesSent} busy=${state.framesBusy} " +
                "failed=${state.framesFailed} rejected=${state.framesRejected} mtu=${state.attMtu} " +
                "mic=${state.mic}; here decoded=${m.framesDecoded} lost=${m.framesLost} " +
                "malformed=${m.framesMalformed} stale=${m.framesStale}",
        )
        if (!state.active) endForAdapterStop(state.lastStop)
    }

    private suspend fun endForAdapterStop(reason: String) = lifecycle.withLock {
        if (!tearDownLocal()) return@withLock
        // The link ending is not the player unplugging; anything else is.
        if (reason != "controller_link_ended") pluggedIn = false
        log("ended by adapter: last_stop=$reason")
        mutableView.value = CompanionAudioView(CompanionAudioPhase.Ended, endedText(reason), controllerLink.value)
    }

    private suspend fun onConditionsChanged(ready: Boolean, link: Boolean) {
        replugJob?.cancel()
        replugJob = null
        applyConditions(ready, link)
        if (ready && link && pluggedIn) replugJob = scope.launch { replug() }
    }

    private suspend fun applyConditions(ready: Boolean, link: Boolean) {
        if (!ready) {
            lifecycle.withLock {
                // The adapter ends its half on management loss by itself.
                if (tearDownLocal()) log("stopped: management lost")
                mutableView.value = initialView()
            }
            return
        }
        if (!link) {
            lifecycle.withLock {
                if (tearDownLocal()) {
                    tellAdapterStop()
                    log("stopped: controller link ended")
                    mutableView.value = CompanionAudioView(
                        CompanionAudioPhase.Ended, endedText("controller_link_ended"), controllerLink = false,
                    )
                } else {
                    val current = mutableView.value
                    mutableView.value = if (current.phase == CompanionAudioPhase.Unavailable ||
                        current.phase == CompanionAudioPhase.Off
                    ) initialView() else current.copy(controllerLink = false)
                }
            }
            return
        }
        lifecycle.withLock {
            val current = mutableView.value
            mutableView.value = when {
                current.phase == CompanionAudioPhase.Unavailable -> initialView()
                !current.on -> current.copy(controllerLink = true)
                else -> current
            }
        }
    }

    private fun onFrame(notification: ByteArray) {
        lastFrameMillis = clockMillis()
        synchronized(receiveGate) {
            receiver?.accept(notification)
            if (sink?.takeConcealRequest() == true) receiver?.concealOne()
        }
        if (awaitingConsole) {
            awaitingConsole = false
            val current = mutableView.value
            if (current.phase == CompanionAudioPhase.Playing) {
                mutableView.value = current.copy(awaitingConsole = false)
            }
        }
    }

    // Called under receiveGate from the receiver.
    private fun emit(pcm: ShortArray, samples: Int) {
        val output = sink ?: return
        if (!output.write(pcm, samples)) sinkDrops++
    }

    /**
     * Open the uplink for a session the adapter started with the microphone. On
     * failure the adapter is re-plugged as headphones so the console does not
     * wait on a microphone that will never speak.
     */
    private suspend fun openMic() {
        val encoder = micEncoders()
        val (stream, problem) = if (encoder == null) {
            null to "This app build has no microphone encoder."
        } else {
            CompanionMicStream.open(scope, micUplinks(), micCaptures(), encoder)
        }
        if (stream != null) {
            stream.muted = microphoneMuted
            micStream = stream
            log("microphone: live")
            return
        }
        microphoneDetail = problem
        log("microphone: $problem")
        restartAccessory(microphone = false)
    }

    private suspend fun restartAccessory(microphone: Boolean) {
        try {
            control.start(microphone)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            log("re-plug not acknowledged: ${error.message}")
        }
    }

    private fun closeMic() {
        val stream = micStream ?: return
        micStream = null
        finalMicMetrics = stream.metrics
        stream.close()
        val m = finalMicMetrics
        log(
            "microphone end: captured=${m.blocksCaptured} encoded=${m.framesEncoded} rejected=${m.encodeRejected} " +
                "written=${m.framesWritten} displaced=${m.framesDisplaced} failed=${m.writeFailures}",
        )
    }

    private fun publishPlaying() {
        mutableView.value = CompanionAudioView(
            CompanionAudioPhase.Playing,
            awaitingConsole = awaitingConsole,
            microphone = micStream != null,
            microphoneDetail = microphoneDetail,
            muted = microphoneMuted,
        )
    }

    private suspend fun tearDownLocal(): Boolean {
        awaitingConsole = false
        closeMic()
        watchdog?.cancel()
        watchdog = null
        val plane = downlink
        downlink = null
        plane?.close()
        tearDownSink()
        if (plane != null) {
            val m = finalMetrics
            log(
                "session end: decoded=${m.framesDecoded} lost=${m.framesLost} concealed=${m.framesConcealed} " +
                    "stale=${m.framesStale} malformed=${m.framesMalformed} decode_errors=${m.decodeErrors} " +
                    "sink_drops=${m.sinkDrops} underruns=${m.sinkUnderruns} prebuffer_ms=${m.prebufferMillis} " +
                    "buffer_growths=${m.bufferGrowths}",
            )
        }
        return plane != null
    }

    private fun tearDownSink() {
        val output: CompanionAudioSink?
        synchronized(receiveGate) {
            receiver?.let { finalMetrics = snapshot(it) }
            output = sink
            sink = null
            receiver = null
        }
        output?.stop()
    }

    private suspend fun tellAdapterStop() {
        if (!managementReady.value) return
        try {
            control.stop()
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            log("stop not acknowledged: ${error.message}")
        }
    }

    // Called under receiveGate.
    private fun snapshot(r: CompanionAudioReceiver) = CompanionAudioMetrics(
        framesDecoded = r.framesDecoded,
        framesLost = r.framesLost,
        framesConcealed = r.framesConcealed,
        framesStale = r.framesStale,
        framesMalformed = r.framesMalformed,
        decodeErrors = r.decodeErrors,
        sinkDrops = sinkDrops,
        sinkUnderruns = sink?.underruns ?: 0,
        prebufferMillis = sink?.prebufferMillis ?: 0,
        bufferGrowths = r.framesGrown,
    )

    private fun fail(detail: String) {
        mutableView.value = CompanionAudioView(CompanionAudioPhase.Error, detail, controllerLink.value)
        log("error: $detail")
    }

    private fun initialView() = if (managementReady.value) {
        CompanionAudioView(CompanionAudioPhase.Off, controllerLink = controllerLink.value)
    } else {
        CompanionAudioView(CompanionAudioPhase.Unavailable, "The adapter is not connected.", controllerLink.value)
    }

    companion object {
        /** Re-plug tries after Controller Link returns, [REPLUG_RETRY_MILLIS] apart. */
        const val REPLUG_ATTEMPTS = 5
        const val REPLUG_RETRY_MILLIS = 600L
        const val SILENCE_BEFORE_CHECK_MILLIS = 2_000L
        const val WATCHDOG_PERIOD_MILLIS = 1_000L

        /** The packets carry no container: refuse a geometry this decoder was not built for. */
        fun geometryProblem(state: CompanionAudioState): String? =
            if (state.version == CompanionAudioDataPlane.VERSION &&
                state.codec == "opus" &&
                state.sampleRate == CompanionAudioDataPlane.SAMPLE_RATE &&
                state.channels == CompanionAudioDataPlane.CHANNELS &&
                state.frameMillis == CompanionAudioDataPlane.FRAME_MILLIS &&
                CompanionAudioDataPlane.frameBytesSupported(state.frameBytes)
            ) {
                null
            } else {
                "The adapter's audio format (v${state.version} ${state.codec} ${state.sampleRate} Hz, " +
                    "${state.channels} ch, ${state.frameMillis} ms, ${state.frameBytes} B) is not one this app can play."
            }

        fun refusalText(state: CompanionAudioState): String = when (state.admit) {
            "unsupported" -> "This adapter board cannot stream audio. Audio needs a Pico 2 W."
            "mtu" -> "The Bluetooth link is too small for audio (MTU ${state.attMtu}, needs ${state.minimumAttMtu})."
            "encoder_busy" -> "A controller headset is already using the adapter's audio."
            "controller_headset" ->
                "A headset is plugged into your controller, so the console's audio plays there. " +
                    "Unplug it to play audio on this device."
            "rate" -> "The adapter offered an audio bitrate this app does not play. Update the app or the adapter."
            "no_controller_link" ->
                "Connect this device as the controller first: the headset belongs to the controller you are using."
            "other_controller" ->
                "Another controller is driving the console. The headset works only while this device is the controller."
            "untrusted" -> "The adapter connection is not trusted yet."
            "no_link" -> "The adapter is not connected."
            "mic_unsupported" -> "This adapter build cannot take a microphone, so the headset is speaker only."
            else -> if (!state.supported) "This adapter cannot stream audio." else "The adapter refused audio (${state.admit})."
        }

        fun endedText(lastStop: String): String = when (lastStop) {
            "preempted_controller_headset", "preempted_pro2" -> "Audio moved to the headset plugged into your controller."
            "stopped" -> "Audio was turned off on the adapter."
            "controller_link_ended" -> "This device stopped being the controller, so console audio is back on the console."
            "source_changed" -> "Another controller took over, so console audio is back on the console."
            else -> "The adapter stopped audio ($lastStop)."
        }
    }
}
