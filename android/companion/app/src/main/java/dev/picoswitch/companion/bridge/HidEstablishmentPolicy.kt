package dev.picoswitch.companion.bridge

/**
 * Whether an Android HID Device profile rebind may start a Controller Link
 * establishment on its own.
 *
 * ## The defect this encodes (measured 2026-08-22)
 *
 * `BluetoothProfile.ServiceListener.onServiceConnected()` fires whenever Android
 * (re)binds the HID Device profile service — including after the service dies
 * and comes back, which is not something this app asked for. The transport used
 * to register and, because `requestedHost` was still set, immediately connect
 * from that callback, guarded only by a `stopped` flag that just `stop()` writes.
 *
 * After a FAILED attempt `stopped` is still false and `requestedHost` is still
 * set, so a spurious rebind resurrected an establishment nobody requested. A
 * captured trace shows exactly that:
 *
 * ```text
 * Registering -> connecting -> Idle -> Unsupported
 * HID profile: service disconnected
 * Registering -> connecting -> Failed      <- second attempt, never requested
 * ```
 *
 * Two overlapping establishment generations is the condition [HidConnectionState]
 * and the transport's watchdog are least able to reason about, and it doubles the
 * connect churn against the adapter.
 *
 * ## The invariant
 *
 * > A Controller Link establishment may only begin from an explicit request —
 * > `start()` or `connect()`. Reaching a terminal outcome, or losing the profile
 * > service, ends that authority; only a new explicit request restores it.
 *
 * "Not stopped" is NOT the same as "wants a link", and conflating them is what
 * produced the extra generation.
 */
enum class HidEstablishmentIntent {
    /** No establishment requested; a profile rebind must stay passive. */
    Idle,

    /** `start()`/`connect()` was called and no terminal outcome has occurred. */
    Wanted,
}

object HidEstablishmentPolicy {

    /**
     * May [onServiceConnected] register and connect?
     *
     * Only when an establishment is actually outstanding. A rebind arriving in
     * any other state is Android's business, not a reason to page the adapter.
     */
    fun mayAutoRegister(intent: HidEstablishmentIntent, stopped: Boolean): Boolean =
        !stopped && intent == HidEstablishmentIntent.Wanted

    /**
     * What an outcome does to the intent.
     *
     * Terminal outcomes clear it, so nothing can silently retry on the app's
     * behalf. A non-terminal transition leaves it alone — see
     * [HidConnectionState.isTerminal] for why `disconnecting` is not a
     * conclusion.
     */
    fun afterOutcome(
        intent: HidEstablishmentIntent,
        terminal: Boolean,
    ): HidEstablishmentIntent =
        if (terminal) HidEstablishmentIntent.Idle else intent
}
