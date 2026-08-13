package dev.picoswitch.companion.controller

/** Pure gates for restoring foreground-only handheld input after Android resumes the app. */
object ControllerAutoResumePolicy {
    fun canQueryAdapter(
        hasSelectedSource: Boolean,
        hasRelationship: Boolean,
        managementConnected: Boolean,
        busy: Boolean,
        bridgePhase: BridgePhase,
    ): Boolean = hasSelectedSource && hasRelationship && managementConnected && !busy &&
        bridgePhase in setOf(BridgePhase.Idle, BridgePhase.Ready, BridgePhase.Failed)

    fun shouldAcquire(controllerAttached: Boolean, bondedHostAvailable: Boolean): Boolean =
        !controllerAttached && bondedHostAvailable
}
