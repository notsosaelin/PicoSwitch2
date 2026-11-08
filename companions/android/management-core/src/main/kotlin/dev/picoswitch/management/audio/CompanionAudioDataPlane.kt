package dev.picoswitch.management.audio

/**
 * Companion audio downlink framing (EXPERIMENTAL): the companion half of
 * `firmware/src/management/ns2_companion_audio.h`, and the Kotlin twin of the
 * Windows `CompanionAudioDataPlane`.
 *
 * One notification carries one complete Opus packet,
 * `[version][opcode][seq lo][seq hi]` followed by the packet, TOC byte first.
 * The packet is CELT-only, full band, stereo, 20 ms (TOC `0xFC`) and constant
 * bitrate, so it is a valid public Opus packet that any standard decoder decodes
 * unchanged. The adapter streams 96 kbit/s (240-byte packets), the console's own
 * Pro Controller 2 headset rate and the quality floor, and reports the session's
 * frame size in `caudio status` (`frame_bytes`). The receiver is sized from that
 * report.
 */
object CompanionAudioDataPlane {
    const val VERSION: Int = 1
    const val OPCODE_OPUS: Int = 0x90
    const val HEADER_BYTES = 4

    /** The largest packet: 96 kbit/s, the encoder's validated ceiling. */
    const val MAX_OPUS_BYTES = 240
    const val MAX_FRAME_BYTES = HEADER_BYTES + MAX_OPUS_BYTES

    const val SAMPLE_RATE = 48_000
    const val CHANNELS = 2
    const val FRAME_MILLIS = 20
    const val SAMPLES_PER_CHANNEL = SAMPLE_RATE * FRAME_MILLIS / 1000

    fun frameBytesSupported(frameBytes: Int): Boolean =
        frameBytes >= HEADER_BYTES + 2 && frameBytes <= MAX_FRAME_BYTES

    /** Session bitrate implied by a CBR frame size, in kbit/s. */
    fun kbps(frameBytes: Int): Int = (frameBytes - HEADER_BYTES) * 8 * (1000 / FRAME_MILLIS) / 1000

    /** One notification carries one frame: value <= MTU - 3. */
    fun minimumAttMtu(frameBytes: Int): Int = frameBytes + 3

    /** One notification of a session with [frameBytes]-long frames, or null when it is not one. */
    fun decode(frame: ByteArray, frameBytes: Int): CompanionAudioFrame? {
        if (frame.size != frameBytes || !frameBytesSupported(frameBytes)) return null
        if ((frame[0].toInt() and 0xFF) != VERSION || (frame[1].toInt() and 0xFF) != OPCODE_OPUS) return null
        val sequence = (frame[2].toInt() and 0xFF) or ((frame[3].toInt() and 0xFF) shl 8)
        return CompanionAudioFrame(sequence, frame.copyOfRange(HEADER_BYTES, frame.size))
    }

    /**
     * Frames lost between [previous] and [current], wrap-safe: zero for
     * consecutive frames, negative for a duplicate or an older frame.
     */
    fun gap(previous: Int, current: Int): Int = ((current - previous) and 0xFFFF).toShort().toInt() - 1
}

/** One audio notification: its 16-bit sequence and Opus packet. */
class CompanionAudioFrame(val sequence: Int, val opus: ByteArray)
