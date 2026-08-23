package dev.picoswitch.bridge.touch

/**
 * A local touch feedback moment, in semantic terms.
 *
 * This is UI affordance, NOT console rumble. The two travel completely different
 * paths — console rumble arrives from the adapter and is delivered by the
 * session's output backend — and confusing them would let a button press mutate
 * bridge output state or fight a game's own effects.
 */
enum class TouchFeedbackEvent {
    /** A control was taken down. */
    Press,

    /** A control was released deliberately. */
    Release,

    /** The D-pad moved to a different direction while still held. */
    DirectionChange,
}

/**
 * Host-local feedback, implemented by the platform.
 *
 * Deliberately tiny: the portable engine knows when something is worth feeling,
 * and the platform knows what that feels like and whether the user has asked for
 * it at all.
 */
fun interface TouchFeedbackBackend {
    fun perform(event: TouchFeedbackEvent)

    companion object {
        val None = TouchFeedbackBackend { }
    }
}
