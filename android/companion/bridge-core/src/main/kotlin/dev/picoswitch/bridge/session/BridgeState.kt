package dev.picoswitch.bridge.session

import dev.picoswitch.bridge.core.DeviceCapabilities

/**
 * Everything an application frontend needs to render the bridge, in bridge
 * vocabulary only.
 *
 * A Compose screen, a Win32 dialog and a curses status line can all be written
 * against this without knowing which platform produced it. Anything that would
 * force a frontend to understand host APIs is either normalized (capabilities,
 * phases) or reduced to display text the platform composed itself
 * ([MotionDiagnostics.platformRaw], [OutputDiagnostics.route]).
 */
data class BridgeState(
    val phase: BridgeLinkPhase = BridgeLinkPhase.Idle,
    val hostName: String? = null,
    val message: String? = null,
    val registered: Boolean = false,
    val reportCount: Long = 0,
    val lastReportAtMillis: Long = 0,

    /** What the selected source and this host can do. */
    val capabilities: DeviceCapabilities = DeviceCapabilities.None,

    /** Console player number assigned to this host (0 = none yet). */
    val playerIndicator: Int = 0,

    /** True while the console is actually consuming motion (sensors are live). */
    val motionActive: Boolean = false,

    /** Newest rumble amplitude the console asked for, for a live indicator. */
    val rumbleAmplitude: Int = 0,

    val batteryPercent: Int = 0,

    /** Layered motion view; see [MotionDiagnostics]. */
    val motion: MotionDiagnostics = MotionDiagnostics(),

    /** Layered output view; see [OutputDiagnostics]. */
    val output: OutputDiagnostics = OutputDiagnostics(),
) {
    /** False when the host has no IMU to offer at all. */
    val motionAvailable: Boolean get() = capabilities.motion
}

/**
 * Pure gates for restoring a foreground-only host session after the application
 * comes back to the front.
 *
 * Not Android-specific despite where it was written: any platform whose input
 * capture is tied to window focus has the same "should I silently take the
 * console back" question, and the same answer — only when the user already chose
 * a source, only when a saved adapter exists, and never when a physical
 * controller currently owns the adapter.
 */
object SessionResumePolicy {
    fun canQueryAdapter(
        hasSelectedSource: Boolean,
        hasRelationship: Boolean,
        managementConnected: Boolean,
        busy: Boolean,
        phase: BridgeLinkPhase,
    ): Boolean = hasSelectedSource && hasRelationship && managementConnected && !busy &&
        phase in setOf(BridgeLinkPhase.Idle, BridgeLinkPhase.Ready, BridgeLinkPhase.Failed)

    fun shouldAcquire(activeSourceId: Long, bondedHostAvailable: Boolean): Boolean =
        activeSourceId == 0L && bondedHostAvailable

    /**
     * Choose nothing until the bridge is physically present in Pico's registry. A sole source can
     * safely become active only while the arbiter still reports no owner; multiple candidates are
     * deliberately ambiguous and an existing owner is never stolen.
     */
    fun soleSourceToActivate(activeSourceId: Long, sourceIds: List<Long>): Long? =
        sourceIds.singleOrNull()?.takeIf { activeSourceId == 0L }
}
