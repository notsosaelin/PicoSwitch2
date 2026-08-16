package dev.picoswitch.bridge.core

import java.util.concurrent.atomic.AtomicLong

/**
 * Boundary counters for locating the FIRST point where expected data disappears.
 *
 * Written after a refactor lost battery, motion and rumble together while
 * ordinary input kept working. 302 passing unit tests did not catch it, because
 * they proved `fake transport -> BridgeSession`, never
 * `real HID callback -> real transport -> live session`. Counters close that gap
 * on hardware: read them in order, and the first one that stays at zero while its
 * upstream neighbour advances is the broken boundary.
 *
 * Deliberately counters, not logs. Each is a single atomic increment on paths
 * that run at 125 Hz, and the whole set is rendered only when something asks.
 *
 * Read them in this order:
 *
 * ```text
 * REVERSE  (adapter -> host)
 *   transportOutputCallbacks   HID stack delivered an output report at all
 *   outputFramesDecoded        it parsed as a bridge output report
 *   outputFramesRejected       it did not (framing/report-id mismatch)
 *   sessionOutputApplied       the live session applied it
 *   motionWantedTransitions    the console's motion gate actually changed
 *   rumbleRequestsProduced     a non-silent rumble reached the output backend
 *
 * FORWARD  (host -> adapter)
 *   reportsSent                input reports the transport accepted
 *   reportsWithMotionBlock     ...carrying a valid motion block
 *   reportsWithBatteryBlock    ...carrying a valid battery level
 *   motionSamplesValid         backend returned a usable IMU sample
 *   batterySamplesValid        backend returned a usable battery reading
 * ```
 */
class BridgeCounters {
    val transportOutputCallbacks = AtomicLong()
    val outputFramesDecoded = AtomicLong()
    val outputFramesRejected = AtomicLong()
    val sessionOutputApplied = AtomicLong()
    val motionWantedTransitions = AtomicLong()
    val rumbleRequestsProduced = AtomicLong()

    val reportsSent = AtomicLong()
    val reportsWithMotionBlock = AtomicLong()
    val reportsWithBatteryBlock = AtomicLong()
    val motionSamplesValid = AtomicLong()
    val batterySamplesValid = AtomicLong()

    /** One line, ordered so the first zero after a non-zero is the divergence. */
    fun snapshot(): String = buildString {
        append("in:cb=").append(transportOutputCallbacks.get())
        append(" dec=").append(outputFramesDecoded.get())
        append(" rej=").append(outputFramesRejected.get())
        append(" applied=").append(sessionOutputApplied.get())
        append(" motionGate=").append(motionWantedTransitions.get())
        append(" rumble=").append(rumbleRequestsProduced.get())
        append(" | out:sent=").append(reportsSent.get())
        append(" motionBlk=").append(reportsWithMotionBlock.get())
        append(" battBlk=").append(reportsWithBatteryBlock.get())
        append(" imuSamples=").append(motionSamplesValid.get())
        append(" battSamples=").append(batterySamplesValid.get())
    }

    /**
     * The first boundary that produced nothing while its upstream neighbour did,
     * or null when every stage that should have advanced did advance.
     */
    fun firstDivergence(): String? {
        val stages = listOf(
            "HID output callbacks" to transportOutputCallbacks.get(),
            "output frames decoded" to outputFramesDecoded.get(),
            "session applied output" to sessionOutputApplied.get(),
        )
        stages.zipWithNext().forEach { (upstream, downstream) ->
            if (upstream.second > 0 && downstream.second == 0L) {
                return "${downstream.first} is 0 while ${upstream.first} is ${upstream.second}"
            }
        }
        if (transportOutputCallbacks.get() == 0L && reportsSent.get() > 0) {
            return "no HID output callbacks at all while ${reportsSent.get()} input reports were sent " +
                "-- the adapter is not sending feedback, or it did not recognize this bridge"
        }
        return null
    }

    fun reset() {
        listOf(
            transportOutputCallbacks, outputFramesDecoded, outputFramesRejected,
            sessionOutputApplied, motionWantedTransitions, rumbleRequestsProduced,
            reportsSent, reportsWithMotionBlock, reportsWithBatteryBlock,
            motionSamplesValid, batterySamplesValid,
        ).forEach { it.set(0) }
    }
}
