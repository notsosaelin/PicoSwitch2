package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.InputAuthority

/**
 * The on-screen controller, wired to the shared state machine.
 *
 * Three things that every touchscreen host client would otherwise repeat
 * identically, done once:
 *
 * ```text
 * platform contacts -> TouchContactTracker -> TouchControlEngine
 *                                                   |
 *                                          TouchContribution
 *                                                   v
 *                                          ControllerInputState
 * ```
 *
 * plus taking and returning [InputAuthority], in the order that cannot leave a
 * held control behind: RELEASE the engine first, THEN move authority. Doing it
 * the other way round drops the engine's contribution on the floor while the
 * engine still believes the control is down, so the next contact event would
 * republish it.
 *
 * The session-level work — pushing a neutral report while the link is still up,
 * rebinding output actuators — belongs to the host client, because only it knows
 * what the host's actuators are.
 */
class TouchGamepad(
    private val input: ControllerInputState,
    config: TouchControlConfig = TouchControlConfig.Default,
) {
    private var menuRequest: () -> Unit = {}

    val engine = TouchControlEngine(
        config = config,
        onContribution = input::applyTouch,
        onMenu = { menuRequest() },
    )

    val contacts = TouchContactTracker(engine)

    /** True while the on-screen controller is the authoritative gameplay input. */
    var active: Boolean = false
        private set

    /** What the on-screen controller opens when its menu control is tapped. */
    fun onMenuRequested(action: () -> Unit) {
        menuRequest = action
    }

    fun setFeedbackBackend(backend: TouchFeedbackBackend) = engine.setFeedbackBackend(backend)

    fun setConfig(config: TouchControlConfig) = engine.setConfig(config)

    /** Take gameplay input. Idempotent. */
    fun activate() {
        if (active) return
        contacts.releaseAll(TouchReleaseReason.AuthorityChanged)
        input.setAuthority(InputAuthority.Touch)
        active = true
    }

    /** Give gameplay input back to the host's physical controls. Idempotent. */
    fun deactivate() {
        if (!active) return
        contacts.releaseAll(TouchReleaseReason.ModeExit)
        input.setAuthority(InputAuthority.Physical)
        active = false
    }

    /** Drop every held control without giving up authority. */
    fun release(reason: TouchReleaseReason) = contacts.releaseAll(reason)

    fun diagnostics(): TouchDiagnosticsSnapshot = engine.diagnostics()
}
