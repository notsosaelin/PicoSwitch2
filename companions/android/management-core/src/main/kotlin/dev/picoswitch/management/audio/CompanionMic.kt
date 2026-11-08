package dev.picoswitch.management.audio

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Companion microphone uplink framing (EXPERIMENTAL): the companion half of
 * `firmware/src/management/ns2_companion_mic.h`, and the Kotlin twin of the
 * Windows `CompanionMicDataPlane`.
 *
 * One GATT write-without-response carries one complete Opus packet:
 * `[version][opcode][seq lo][seq hi]` followed by the packet, TOC first. The
 * packet is CELT-only, full band, MONO, 20 ms (TOC `0xF8`), 64 kbit/s CBR:
 * exactly 160 bytes. The adapter decodes it with libopus's CELT decoder and
 * refuses any other shape.
 *
 * Mono at 64 kbit/s is not a reduction: a headset microphone is one capsule,
 * and 64 kbit/s is above the DualSense's own microphone stream (56.8 kbit/s)
 * and, per channel, above the 96 kbit/s stereo downlink.
 */
object CompanionMicDataPlane {
    const val VERSION: Int = 1
    const val OPCODE_OPUS: Int = 0x91
    const val HEADER_BYTES = 4
    const val KBPS = 64
    const val FRAME_MILLIS = 20
    const val OPUS_BYTES = KBPS * 1000 / 8 / (1000 / FRAME_MILLIS)
    const val FRAME_BYTES = HEADER_BYTES + OPUS_BYTES

    /** Config 31 (CELT-only, full band, 20 ms), mono, one frame. */
    const val TOC: Int = 0xF8
    const val SAMPLE_RATE = 48_000
    const val CHANNELS = 1
    const val SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MILLIS / 1000

    /** One write carries one frame: value <= MTU - 3. */
    const val MINIMUM_ATT_MTU = FRAME_BYTES + 3

    fun isValidPacket(opus: ByteArray, length: Int = opus.size): Boolean =
        length == OPUS_BYTES && opus.size >= length && (opus[0].toInt() and 0xFF) == TOC

    /** Frame one packet, or null when it is not exactly the uplink's packet. */
    fun encode(sequence: Int, opus: ByteArray, length: Int = opus.size): ByteArray? {
        if (!isValidPacket(opus, length)) return null
        val frame = ByteArray(FRAME_BYTES)
        frame[0] = VERSION.toByte()
        frame[1] = OPCODE_OPUS.toByte()
        frame[2] = (sequence and 0xFF).toByte()
        frame[3] = ((sequence shr 8) and 0xFF).toByte()
        opus.copyInto(frame, HEADER_BYTES, 0, OPUS_BYTES)
        return frame
    }
}

/** One 20 ms mono block to one uplink packet; null when the encoder's output is not exactly one. */
fun interface CompanionMicEncoder {
    fun encode(pcm: ShortArray): ByteArray?
}

/** The local microphone: 48 kHz mono 16-bit, whole 20 ms blocks on a capture thread. */
interface CompanionMicCapture {
    /** Open the device and deliver blocks; throws when it cannot. [onBlock] must not block. */
    fun start(onBlock: (ShortArray) -> Unit)

    fun stop()

    val blocksCaptured: Long
}

/** The MIC-UP characteristic on the live management link. */
interface CompanionMicUplink {
    /** False when this adapter does not expose the characteristic. */
    fun available(): Boolean

    /** One write-without-response, completed when the platform stack took it. */
    suspend fun write(frame: ByteArray): Boolean
}

/**
 * Bounded FIFO writer for the microphone uplink: at most [DEPTH] frames (160 ms)
 * waiting, ONE write in flight, the OLDEST waiting frame displaced when the
 * radio falls behind, a failed write never retried (a resent frame would arrive
 * after newer audio; the adapter conceals the gap). The Kotlin twin of the
 * Windows `CompanionMicWriter`; the depth is from the firmware's link
 * simulation (tests/firmware/test_ns2_companion_mic.c): with a fifth of the
 * connection events lost to Bluetooth Classic, four frames shed ~70 frames an
 * hour, eight shed none.
 */
class CompanionMicWriter(
    private val scope: CoroutineScope,
    private val writeFrame: suspend (ByteArray) -> Boolean,
) {
    data class Counters(val queued: Long, val written: Long, val displaced: Long, val failed: Long)

    private val gate = Any()
    private val pending = ArrayDeque<ByteArray>()
    private var pumping = false
    private var closed = false
    private var queued = 0L
    private var written = 0L
    private var displaced = 0L
    private var failed = 0L

    val stats: Counters get() = synchronized(gate) { Counters(queued, written, displaced, failed) }

    val waiting: Int get() = synchronized(gate) { pending.size }

    fun enqueue(frame: ByteArray) {
        synchronized(gate) {
            if (closed) return
            if (pending.size == DEPTH) {
                pending.removeFirst()
                displaced++
            }
            pending.addLast(frame)
            queued++
            if (pumping) return
            pumping = true
        }
        scope.launch { pump() }
    }

    fun close() = synchronized(gate) {
        closed = true
        pending.clear()
    }

    private suspend fun pump() {
        while (true) {
            val frame = synchronized(gate) {
                if (closed || pending.isEmpty()) {
                    pumping = false
                    return
                }
                pending.removeFirst()
            }
            val ok = try {
                writeFrame(frame)
            } catch (error: CancellationException) {
                synchronized(gate) { pumping = false }
                throw error
            } catch (_: Exception) {
                false
            }
            synchronized(gate) { if (ok) written++ else failed++ }
        }
    }

    companion object {
        const val DEPTH = 8
    }
}

data class CompanionMicMetrics(
    val blocksCaptured: Long = 0,
    val framesEncoded: Long = 0,
    val encodeRejected: Long = 0,
    val framesWritten: Long = 0,
    val framesDisplaced: Long = 0,
    val writeFailures: Long = 0,
)

/**
 * One microphone stream: capture -> encode -> frame -> bounded writer. The
 * sequence advances once per captured block, encoded or not, so every block the
 * adapter does not receive is a gap it conceals.
 */
class CompanionMicStream private constructor(
    private val capture: CompanionMicCapture,
    private val encoder: CompanionMicEncoder,
    private val writer: CompanionMicWriter,
) {
    private val gate = Any()
    private val silence = ShortArray(CompanionMicDataPlane.SAMPLES_PER_FRAME)
    private var sequence = 0
    private var encoded = 0L
    private var rejected = 0L
    private var closed = false

    val metrics: CompanionMicMetrics
        get() {
            val writes = writer.stats
            return synchronized(gate) {
                CompanionMicMetrics(
                    capture.blocksCaptured, encoded, rejected, writes.written, writes.displaced, writes.failed,
                )
            }
        }

    /**
     * A headset's mute: the uplink keeps its 20 ms cadence and sequence, carrying
     * encoded silence. Muting never changes the accessory the console sees --
     * re-plugging as headphones withdraws the jack (NS2_CAUDIO_REPLUG_MS) and the
     * console reroutes the speaker away and back, which is the audible stutter
     * mute exists to avoid.
     */
    @Volatile var muted: Boolean = false

    /** One captured block (capture thread). Public for tests. */
    fun onBlock(pcm: ShortArray) {
        val frame = synchronized(gate) {
            if (closed) return
            val seq = sequence
            sequence = (sequence + 1) and 0xFFFF
            val source = if (muted) silence else pcm
            val opus = if (pcm.size == CompanionMicDataPlane.SAMPLES_PER_FRAME) encoder.encode(source) else null
            val framed = opus?.let { CompanionMicDataPlane.encode(seq, it) }
            if (framed == null) {
                rejected++
                return
            }
            encoded++
            framed
        }
        writer.enqueue(frame)
    }

    fun close() {
        synchronized(gate) {
            if (closed) return
            closed = true
        }
        try {
            capture.stop()
        } finally {
            writer.close()
        }
    }

    companion object {
        /** Open the stream, or the reason there is none. Nothing is left open on failure. */
        fun open(
            scope: CoroutineScope,
            uplink: CompanionMicUplink?,
            capture: CompanionMicCapture?,
            encoder: CompanionMicEncoder,
        ): Pair<CompanionMicStream?, String?> {
            if (uplink == null || !uplink.available()) {
                return null to "This adapter's firmware does not accept a microphone."
            }
            if (capture == null) return null to "No microphone is available on this device."
            val writer = CompanionMicWriter(scope) { uplink.write(it) }
            val stream = CompanionMicStream(capture, encoder, writer)
            return try {
                capture.start(stream::onBlock)
                stream to null
            } catch (error: Exception) {
                writer.close()
                runCatching { capture.stop() }
                null to "Could not open the microphone: ${error.message}"
            }
        }
    }
}
