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
    val engine = TouchControlEngine(
        config = config,
        onContribution = input::applyTouch,
    )

    val contacts = TouchContactTracker(engine)

    /** True while the on-screen controller is the authoritative gameplay input. */
    var active: Boolean = false
        private set

    fun setFeedbackBackend(backend: TouchFeedbackBackend) = engine.setFeedbackBackend(backend)

    /** Where double-tap-hold transitions are reported; the host picks its own log. */
    fun setLatchObserver(observer: TouchLatchObserver) = engine.setLatchObserver(observer)

    /**
     * What the engine is tuned to right now.
     *
     * Exposed so a caller can change ONE tunable without asserting the rest.
     * Two different owners set config here — the settings screen owns the
     * deadzone and the latch default, the platform adapter owns the gesture
     * timings it reads from the toolkit — and rebuilding from
     * [TouchControlConfig.Default] would let whichever ran last silently discard
     * the other's values.
     */
    val config: TouchControlConfig get() = engine.config

    fun setConfig(config: TouchControlConfig) = engine.setConfig(config)

    /** Replace geometry without allowing already-held contacts to claim it. */
    fun setLayout(
        layout: ResolvedTouchLayout,
        reason: TouchReleaseReason = TouchReleaseReason.GeometryInvalidated,
    ) {
        contacts.releaseAll(reason)
        engine.installLayout(layout)
    }

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

    /**
     * When the engine next has timed gesture work, in the host's contact clock.
     *
     * The host is the clock: it stamps contacts, so it is the only thing that
     * can say what time it is in the same units. Consult this after every
     * contact batch and after every [tick]; null means purely event-driven.
     */
    fun nextDeadlineNanos(): Long? = engine.nextDeadlineNanos()

    /**
     * Advance timed gesture work. Safe to call late, twice, or after a teardown
     * — see [TouchControlEngine.onTick].
     */
    fun tick(nowNanos: Long) = engine.onTick(nowNanos)

    fun diagnostics(): TouchDiagnosticsSnapshot = engine.diagnostics()
}
