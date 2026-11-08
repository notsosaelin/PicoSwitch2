package dev.picoswitch.management.audio

/**
 * The Opus decoder seam. [opus] null asks for packet-loss concealment. Returns
 * samples per channel written into the interleaved [pcm] buffer.
 */
interface CompanionAudioDecoder {
    fun decode(opus: ByteArray?, pcm: ShortArray): Int
    fun reset()
}

/**
 * Turns companion audio notifications into PCM blocks, with gap concealment:
 * the Kotlin twin of the Windows `CompanionAudioReceiver`.
 *
 * - A malformed notification is counted and ignored.
 * - A duplicate or older sequence is dropped; playing it would rewind.
 * - A forward gap is concealed with the decoder's packet-loss concealment, one
 *   20 ms block per missing frame, capped at [MAX_CONCEALED_FRAMES].
 */
class CompanionAudioReceiver(
    private val decoder: CompanionAudioDecoder,
    private val emit: (ShortArray, Int) -> Unit,
    private val frameBytes: Int,
) {
    private val pcm = ShortArray(CompanionAudioDataPlane.SAMPLES_PER_CHANNEL * CompanionAudioDataPlane.CHANNELS)
    private var haveLast = false
    private var lastSequence = 0

    var framesDecoded = 0L; private set
    var framesMalformed = 0L; private set
    var framesStale = 0L; private set
    var framesLost = 0L; private set
    var framesConcealed = 0L; private set
    var decodeErrors = 0L; private set
    /** Concealment blocks emitted to deepen the sink's buffer ([concealOne]). */
    var framesGrown = 0L; private set

    fun accept(notification: ByteArray) {
        val frame = CompanionAudioDataPlane.decode(notification, frameBytes)
        if (frame == null) {
            framesMalformed++
            return
        }
        if (haveLast) {
            val gap = CompanionAudioDataPlane.gap(lastSequence, frame.sequence)
            if (gap < 0) {
                framesStale++
                return
            }
            if (gap > 0) {
                framesLost += gap
                repeat(minOf(gap, MAX_CONCEALED_FRAMES)) {
                    if (tryDecode(null)) {
                        framesConcealed++
                        emit(pcm, pcm.size)
                    }
                }
            }
        }
        haveLast = true
        lastSequence = frame.sequence
        if (tryDecode(frame.opus)) {
            framesDecoded++
            emit(pcm, pcm.size)
        }
    }

    /** One concealment block after the last frame, so the sink grows without a gap. */
    fun concealOne() {
        if (haveLast && tryDecode(null)) {
            framesGrown++
            emit(pcm, pcm.size)
        }
    }

    fun reset() {
        haveLast = false
        decoder.reset()
    }

    private fun tryDecode(opus: ByteArray?): Boolean {
        val samples = try {
            decoder.decode(opus, pcm)
        } catch (_: Exception) {
            decodeErrors++
            return false
        }
        if (samples != CompanionAudioDataPlane.SAMPLES_PER_CHANNEL) {
            decodeErrors++
            return false
        }
        return true
    }

    companion object {
        const val MAX_CONCEALED_FRAMES = 3
    }
}
