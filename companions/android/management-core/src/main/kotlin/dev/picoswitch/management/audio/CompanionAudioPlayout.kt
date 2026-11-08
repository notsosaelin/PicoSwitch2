package dev.picoswitch.management.audio

import kotlin.math.abs
import kotlin.math.roundToInt

/*
 * Companion audio playout policy (EXPERIMENTAL): the Kotlin twin of the Windows
 * CompanionAudioJitterPolicy / CompanionAudioDriftControl / CompanionAudioPlayout
 * (companions/windows/src/PicoSwitch.Companion.Platform/CompanionAudio). The
 * constants and the control law are the ones the Windows pipeline simulation
 * (CompanionAudioPipelineSimulationTests) validated against the measured link;
 * CompanionAudioPlayoutTest pins the same vectors here. Change both or neither.
 */

/** How much audio the sink holds before it plays, and how that adapts. */
class CompanionAudioJitterPolicy(initialPrebufferBlocks: Int = MIN_PREBUFFER_BLOCKS) {
    private val floorBlocks = initialPrebufferBlocks.coerceIn(MIN_PREBUFFER_BLOCKS, MAX_PREBUFFER_BLOCKS)
    private var stableBlocks = 0

    var prebufferBlocks: Int = floorBlocks
        private set

    val maxQueuedBlocks: Int get() = prebufferBlocks + HEADROOM_BLOCKS
    val prebufferMillis: Int get() = prebufferBlocks * BLOCK_MILLIS

    fun onUnderrun() {
        grow()
    }

    /** @return true when the prebuffer grew (it was below the ceiling). */
    fun onNearUnderrun(): Boolean = grow()

    /**
     * @param spareBlock false when this block landed with less than a block
     * (plus the low-water margin) still queued; that restarts the shrink window.
     */
    fun onBlockQueued(spareBlock: Boolean = true) {
        if (!spareBlock) {
            stableBlocks = 0
            return
        }
        if (++stableBlocks < STABLE_BLOCKS_TO_SHRINK) return
        stableBlocks = 0
        if (prebufferBlocks > floorBlocks) prebufferBlocks--
    }

    private fun grow(): Boolean {
        stableBlocks = 0
        if (prebufferBlocks >= MAX_PREBUFFER_BLOCKS) return false
        prebufferBlocks++
        return true
    }

    companion object {
        const val BLOCK_MILLIS = 20
        const val MIN_PREBUFFER_BLOCKS = 3
        const val MAX_PREBUFFER_BLOCKS = 8
        const val HEADROOM_BLOCKS = 3
        /** 10 minutes of blocks that never needed the deepest one. */
        const val STABLE_BLOCKS_TO_SHRINK = 30_000
        const val MAX_QUEUED_BLOCKS_CEILING = MAX_PREBUFFER_BLOCKS + HEADROOM_BLOCKS
    }
}

/**
 * Holds the sink's queue at a target depth while the console's and this
 * device's audio clocks disagree, by playing each 20 ms block as 960 +/- 1
 * frames (1,042 ppm of correction, twice the USB full-speed tolerance).
 */
class CompanionAudioDriftControl {
    private var blocks = 0
    private var smoothed = 0.0
    private var correcting = 0

    /** The depth being held, once smoothing has settled. */
    var targetMillis: Double? = null
        private set

    val smoothedMillis: Double get() = smoothed

    var framesAdjusted: Long = 0
        private set

    fun reset() {
        blocks = 0
        smoothed = 0.0
        targetMillis = null
        correcting = 0
    }

    fun framesFor(queuedMillis: Double, target: Double): Int {
        smoothed = if (blocks == 0) queuedMillis else smoothed + SMOOTHING * (queuedMillis - smoothed)
        blocks++
        if (blocks < SETTLE_BLOCKS) return BLOCK_FRAMES
        targetMillis = target
        val error = smoothed - target
        val toward = if (error > 0) -1 else 1
        if (abs(error) > DEADBAND_MILLIS) {
            correcting = toward
        } else if (correcting != toward || abs(error) < RELEASE_MILLIS) {
            correcting = 0
        }
        framesAdjusted += correcting
        return BLOCK_FRAMES + correcting
    }

    companion object {
        const val BLOCK_FRAMES = CompanionAudioJitterPolicy.BLOCK_MILLIS * 48
        const val SETTLE_BLOCKS = 50
        const val SMOOTHING = 1.0 / 50.0
        const val DEADBAND_MILLIS = 4.0
        const val RELEASE_MILLIS = 1.0

        /**
         * Plays [input] (interleaved, [channels] channels, [inputSamples]
         * samples) as exactly [outputFrames] frames into [output] by linear
         * interpolation, first and last frames kept exactly. Returns samples written.
         */
        fun stretch(
            input: ShortArray,
            inputSamples: Int,
            channels: Int,
            outputFrames: Int,
            output: ShortArray,
        ): Int {
            val inputFrames = inputSamples / channels
            if (outputFrames <= 0 || inputFrames == 0) return 0
            if (outputFrames == inputFrames) {
                input.copyInto(output, 0, 0, inputFrames * channels)
                return inputFrames * channels
            }
            val step = if (outputFrames == 1) 0.0 else (inputFrames - 1).toDouble() / (outputFrames - 1)
            for (i in 0 until outputFrames) {
                val position = i * step
                val left = position.toInt()
                val right = minOf(left + 1, inputFrames - 1)
                val fraction = position - left
                for (c in 0 until channels) {
                    val a = input[left * channels + c].toInt()
                    val b = input[right * channels + c].toInt()
                    output[i * channels + c] = (a + (b - a) * fraction).roundToInt().toShort()
                }
            }
            return outputFrames * channels
        }
    }
}

/** What a sink does with one arriving block. */
data class CompanionAudioPlayoutDecision(
    val underrun: Boolean,
    val drop: Boolean,
    val frames: Int,
    val extend: Boolean = false,
)

/**
 * Every buffering decision a companion audio sink makes, separated from the
 * device. Per block: [accept] with the device's queue as it is now; write the
 * block stretched to `frames` unless dropped; when `extend`, the caller then
 * writes one packet-loss-concealment block; call [queued] after each write and
 * start the device when it returns true.
 */
class CompanionAudioPlayout(initialPrebufferBlocks: Int = CompanionAudioJitterPolicy.MIN_PREBUFFER_BLOCKS) {
    private val policy = CompanionAudioJitterPolicy(initialPrebufferBlocks)
    val drift = CompanionAudioDriftControl()
    private var lastQueuedMillis = Double.MAX_VALUE

    var playing = false
        private set
    var underruns = 0L
        private set
    var drops = 0L
        private set
    var extensions = 0L
        private set

    val prebufferBlocks: Int get() = policy.prebufferBlocks
    val prebufferMillis: Int get() = policy.prebufferMillis
    val maxQueuedBlocks: Int get() = policy.maxQueuedBlocks
    val targetMillis: Double get() = ((policy.prebufferBlocks - 1) * CompanionAudioJitterPolicy.BLOCK_MILLIS).toDouble()

    fun accept(queuedBlocks: Int, queuedMillis: Double): CompanionAudioPlayoutDecision {
        lastQueuedMillis = if (playing) queuedMillis else Double.MAX_VALUE
        var underrun = false
        if (playing && queuedBlocks == 0) {
            underruns++
            policy.onUnderrun()
            drift.reset()
            playing = false
            underrun = true
        }
        if (queuedBlocks >= policy.maxQueuedBlocks) {
            drops++
            return CompanionAudioPlayoutDecision(underrun, drop = true, frames = 0)
        }
        if (!playing) {
            return CompanionAudioPlayoutDecision(underrun, drop = false, frames = CompanionAudioDriftControl.BLOCK_FRAMES)
        }
        val extend = queuedMillis < LOW_WATER_MILLIS && policy.onNearUnderrun()
        if (extend) extensions++
        val frames = drift.framesFor(queuedMillis, targetMillis)
        return CompanionAudioPlayoutDecision(underrun, drop = false, frames = frames, extend = extend)
    }

    /** @return true when the device should start (the prebuffer is full). */
    fun queued(queuedBlocksAfter: Int): Boolean {
        policy.onBlockQueued(lastQueuedMillis >= CompanionAudioJitterPolicy.BLOCK_MILLIS + LOW_WATER_MILLIS)
        if (!playing && queuedBlocksAfter >= policy.prebufferBlocks) {
            playing = true
            drift.reset()
            return true
        }
        return false
    }

    fun stopped() {
        playing = false
        drift.reset()
    }

    companion object {
        const val LOW_WATER_MILLIS = 10.0
        /** Starting depth, and floor, for a radio shared with Bluetooth Classic: 120 ms. */
        const val CLASSIC_SHARED_PREBUFFER_BLOCKS = 6
    }
}
