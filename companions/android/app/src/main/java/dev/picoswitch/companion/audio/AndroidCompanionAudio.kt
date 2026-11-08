package dev.picoswitch.companion.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import dev.picoswitch.companion.protocol.ManagementTransport
import dev.picoswitch.management.audio.CompanionAudioDataPlane
import dev.picoswitch.management.audio.CompanionAudioDecoder
import dev.picoswitch.management.audio.CompanionAudioDownlink
import dev.picoswitch.management.audio.CompanionAudioDriftControl
import dev.picoswitch.management.audio.CompanionAudioJitterPolicy
import dev.picoswitch.management.audio.CompanionAudioPlayout
import dev.picoswitch.management.audio.CompanionAudioSink
import io.github.jaredmdobson.concentus.OpusDecoder

/**
 * Opus decode for companion audio with Concentus, the pure-Java port of libopus
 * that the Windows companion uses in its .NET form. A pure-Java decoder rather
 * than MediaCodec because the receiver needs the decoder's packet-loss
 * concealment (a null packet), which MediaCodec's Opus decoder does not offer.
 */
class ConcentusCompanionAudioDecoder : CompanionAudioDecoder {
    private var decoder = OpusDecoder(CompanionAudioDataPlane.SAMPLE_RATE, CompanionAudioDataPlane.CHANNELS)

    override fun decode(opus: ByteArray?, pcm: ShortArray): Int =
        decoder.decode(opus, 0, opus?.size ?: 0, pcm, 0, CompanionAudioDataPlane.SAMPLES_PER_CHANNEL, false)

    override fun reset() {
        decoder.resetState()
    }
}

/**
 * Plays companion audio through an [AudioTrack] in stream mode. Every buffering
 * decision is [CompanionAudioPlayout]'s, the same policy the Windows sink uses and
 * the pipeline simulation validated, given the exact queue depth: frames written
 * minus `playbackHeadPosition`.
 *
 * The AudioTrack buffer IS the playout queue, sized for the policy's ceiling, so
 * a block that does not fit is a drop, never a blocking write on the Bluetooth
 * thread. Starts at [CompanionAudioPlayout.CLASSIC_SHARED_PREBUFFER_BLOCKS]: the
 * Android companion's own Controller Link is Bluetooth Classic HID on the
 * adapter's radio.
 */
class AudioTrackCompanionAudioSink(
    initialPrebufferBlocks: Int = CompanionAudioPlayout.CLASSIC_SHARED_PREBUFFER_BLOCKS,
    private val log: (String) -> Unit = {},
    private val clockMillis: () -> Long = System::currentTimeMillis,
) : CompanionAudioSink {
    private val gate = Any()
    private val playout = CompanionAudioPlayout(initialPrebufferBlocks)
    private var track: AudioTrack? = null
    private var channels = CompanionAudioDataPlane.CHANNELS
    private var writtenFrames = 0L
    private var concealRequested = false
    private val stretched = ShortArray(MAX_BLOCK_FRAMES * CompanionAudioDataPlane.CHANNELS)

    // Device-plane counters, logged every REPORT_MILLIS while blocks arrive:
    // decoded audio that never reaches the speaker is otherwise invisible.
    private var blocksIn = 0L
    private var blocksAccepted = 0L
    private var blocksDropped = 0L
    private var writeErrors = 0L
    private var lastWriteError = 0
    private var playCalls = 0L
    private var pauseCalls = 0L
    private var peak = 0
    private var lastReportMillis = 0L
    private var capacityFrames = 0
    private var shrunkForStart = false
    private var sampleRate = CompanionAudioDataPlane.SAMPLE_RATE
    @Volatile private var routedType: Int? = null
    @Volatile private var rerouted = false
    private var reroutes = 0L
    private val routingListener = AudioTrack.OnRoutingChangedListener { router ->
        // Wired or Bluetooth headphones plugged or unplugged: Android moves the
        // track to the new route by itself, but may discard what was queued on
        // the old one. The queue depth is written minus played, so lost frames
        // would read as depth forever and the policy would run the real buffer
        // dry. Rebuild on the next block: one prebuffer, exact accounting.
        val type = router.routedDevice?.type
        if (routedType != null && type != routedType) rerouted = true
        routedType = type
    }

    override val underruns: Long get() = synchronized(gate) { playout.underruns }
    override val prebufferMillis: Int get() = synchronized(gate) { playout.prebufferMillis }

    override fun start(sampleRate: Int, channels: Int) = synchronized(gate) {
        if (track != null) return@synchronized
        this.sampleRate = sampleRate
        openTrack(channels)
    }

    private fun openTrack(channels: Int) {
        this.channels = channels
        val mask = if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val frameBytes = channels * 2
        val wanted = (CompanionAudioJitterPolicy.MAX_QUEUED_BLOCKS_CEILING + 2) * MAX_BLOCK_FRAMES * frameBytes
        val minimum = AudioTrack.getMinBufferSize(sampleRate, mask, AudioFormat.ENCODING_PCM_16BIT)
        track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setChannelMask(mask)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build(),
            )
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(maxOf(wanted, minimum))
            .build()
            .also { built ->
                capacityFrames = built.bufferSizeInFrames
                // Android starts consuming a streaming track only once its
                // buffer reaches the start threshold, which defaults to the
                // WHOLE buffer. The playout policy caps the queue below that on
                // purpose, so play() was a no-op: head stuck at 0, every block
                // after the prebuffer dropped, silence (Odin 2 Mini, Android 13,
                // 2026-11-07). The policy's prebuffer is the start condition;
                // the track must not add its own.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    built.setStartThresholdInFrames(CompanionAudioDriftControl.BLOCK_FRAMES)
                }
            }
        writtenFrames = 0
        shrunkForStart = false
        routedType = null
        rerouted = false
        track?.addOnRoutingChangedListener(routingListener, null)
    }

    private fun closeTrack() {
        val output = track ?: return
        track = null
        output.removeOnRoutingChangedListener(routingListener)
        runCatching {
            output.pause()
            output.flush()
        }
        output.release()
    }

    override fun write(pcm: ShortArray, samples: Int): Boolean = synchronized(gate) {
        if (rerouted && track != null) {
            reroutes++
            closeTrack()
            playout.stopped()
            openTrack(channels)
            log("sink: rerouted (#$reroutes); track rebuilt, re-prebuffering")
        }
        val output = track ?: return@synchronized false
        blocksIn++
        for (i in 0 until samples) { // interleaved total (CompanionAudioSink.write)
            val v = kotlin.math.abs(pcm[i].toInt())
            if (v > peak) peak = v
        }
        report(output)
        val queuedFrames = queuedFrames(output)
        val queuedBlocks = ((queuedFrames + CompanionAudioDriftControl.BLOCK_FRAMES - 1) /
            CompanionAudioDriftControl.BLOCK_FRAMES).toInt()
        val decision = playout.accept(queuedBlocks, queuedFrames / 48.0)
        if (decision.underrun) {
            output.pause()
            pauseCalls++
        }
        if (decision.drop) {
            blocksDropped++
            return@synchronized false
        }

        val frames = minOf(decision.frames, MAX_BLOCK_FRAMES)
        val count = CompanionAudioDriftControl.stretch(pcm, samples, channels, frames, stretched)
        if (count == 0) return@synchronized false
        val written = output.write(stretched, 0, count, AudioTrack.WRITE_NON_BLOCKING)
        if (written < 0) {
            // An AudioTrack error code (ERROR_DEAD_OBJECT after a routing change,
            // ERROR_INVALID_OPERATION on an uninitialized track), not a full buffer.
            writeErrors++
            lastWriteError = written
            return@synchronized false
        }
        if (written < count) {
            // The track's buffer is the bound; a partial write would desync the frame count.
            if (written > 0) writtenFrames += written / channels
            blocksDropped++
            return@synchronized false
        }
        writtenFrames += count / channels
        blocksAccepted++
        concealRequested = concealRequested || decision.extend
        if (shrunkForStart && output.playbackHeadPosition != 0) {
            // Started: give the policy its full ceiling back.
            output.bufferSizeInFrames = capacityFrames
            shrunkForStart = false
        }
        if (playout.queued(queuedBlocks + 1)) {
            if (output.playState != AudioTrack.PLAYSTATE_PLAYING) {
                playCalls++
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                    // No start-threshold API before Android 12: a buffer that
                    // is exactly as large as what is queued counts as full.
                    output.bufferSizeInFrames = queuedFrames(output).toInt().coerceAtLeast(1)
                    shrunkForStart = true
                }
            }
            output.play()
        }
        true
    }

    override fun takeConcealRequest(): Boolean = synchronized(gate) {
        val requested = concealRequested
        concealRequested = false
        requested
    }

    override fun stop() = synchronized(gate) {
        if (track == null) return@synchronized
        closeTrack()
        playout.stopped()
    }

    private fun report(output: AudioTrack) {
        val now = clockMillis()
        if (now - lastReportMillis < REPORT_MILLIS) return
        lastReportMillis = now
        log(
            "sink: state=${output.state} play=${output.playState} head=${output.playbackHeadPosition} " +
                "written=$writtenFrames in=$blocksIn accepted=$blocksAccepted dropped=$blocksDropped " +
                "errors=$writeErrors last_error=$lastWriteError play_calls=$playCalls pauses=$pauseCalls " +
                "underruns=${playout.underruns} prebuffer_ms=${playout.prebufferMillis} peak=$peak " +
                "rate=${output.sampleRate} routed=${output.routedDevice?.type} reroutes=$reroutes",
        )
        peak = 0
    }

    // Written minus played (the head does not move while paused).
    // playbackHeadPosition is an unsigned 32-bit frame count that wraps.
    private fun queuedFrames(output: AudioTrack): Long {
        val played = output.playbackHeadPosition.toLong() and 0xFFFF_FFFFL
        return ((writtenFrames and 0xFFFF_FFFFL) - played) and 0xFFFF_FFFFL
    }

    private companion object {
        const val MAX_BLOCK_FRAMES = CompanionAudioDriftControl.BLOCK_FRAMES + 8
        const val REPORT_MILLIS = 5_000L
    }
}

/** The audio notification characteristic on this app's management link. */
class TransportCompanionAudioDownlink(private val transport: ManagementTransport) : CompanionAudioDownlink {
    override val attMtu: Int get() = transport.attMtu

    override suspend fun open(onFrame: (ByteArray) -> Unit): Boolean = transport.openCompanionAudio(onFrame)

    override suspend fun close() = transport.closeCompanionAudio()
}
