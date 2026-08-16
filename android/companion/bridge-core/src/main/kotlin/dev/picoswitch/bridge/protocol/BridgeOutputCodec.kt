package dev.picoswitch.bridge.protocol

import dev.picoswitch.bridge.core.BridgeOutput
import dev.picoswitch.bridge.core.RumbleRequest

/**
 * PicoSwitch Bridge output report -> normalized [BridgeOutput].
 *
 * The framing tolerance here is protocol, not a platform workaround: HID hosts
 * legitimately deliver an output report either on the interrupt channel or as a
 * control-channel SET_REPORT, and stacks differ on whether the report ID is
 * included in the payload. Both framings carry the same five bytes, so the
 * decoder accepts both and every backend gets the tolerance for free.
 *
 * Returns null when the payload cannot be a bridge output report, so a stray
 * report can never be applied as rumble.
 */
object BridgeOutputCodec {
    /** `[id][rumble L][rumble R][player][flags]` */
    const val REPORT_SIZE_WITH_ID = 5
    const val BODY_SIZE = 4

    const val FLAG_MOTION_WANTED = 0x01

    fun decode(data: ByteArray?, reportId: Int? = null): BridgeOutput? {
        if (data == null) return null
        if (reportId != null && reportId != ControllerReportEncoder.OUTPUT_REPORT_ID) return null
        var body = data
        // Tolerate an embedded report ID: some stacks include it in `data`.
        if (body.size == REPORT_SIZE_WITH_ID &&
            body[0].toInt() and 0xFF == ControllerReportEncoder.OUTPUT_REPORT_ID
        ) {
            body = body.copyOfRange(1, body.size)
        }
        if (body.size < BODY_SIZE) return null
        return BridgeOutput(
            rumble = RumbleRequest(
                left = body[0].toInt() and 0xFF,
                right = body[1].toInt() and 0xFF,
            ),
            playerIndicator = body[2].toInt() and 0xFF,
            motionRequested = (body[3].toInt() and FLAG_MOTION_WANTED) != 0,
        )
    }
}
