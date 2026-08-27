package dev.picoswitch.companion.bridge

import android.view.HapticFeedbackConstants
import android.view.View
import dev.picoswitch.bridge.touch.TouchFeedbackBackend
import dev.picoswitch.bridge.touch.TouchFeedbackEvent

/**
 * Local touch feedback for the on-screen controller.
 *
 * NOT console rumble, and deliberately not routed through [AndroidOutputBackend].
 * The two are different data paths with different owners: console rumble arrives
 * from the adapter and drives whatever actuator the session bound, while this is
 * a UI affordance that says a control was hit. Running a button press through the
 * rumble backend would let the interface mutate bridge output state and fight a
 * game's own effects.
 *
 * `View.performHapticFeedback` rather than a vibrator handle, because that is the
 * API that respects the user's touch-feedback setting. The "ignore global
 * setting" flags exist and are not used: a person who turned haptics off meant it.
 */
class AndroidTouchFeedback(private val view: View) : TouchFeedbackBackend {

    override fun perform(event: TouchFeedbackEvent) {
        val constant = when (event) {
            TouchFeedbackEvent.Press -> HapticFeedbackConstants.VIRTUAL_KEY
            TouchFeedbackEvent.Release -> HapticFeedbackConstants.VIRTUAL_KEY_RELEASE
            // Crossing a D-pad sector is a smaller moment than a press; a full
            // virtual-key tap for every direction change while sliding a thumb
            // around the ring turns into a rattle.
            TouchFeedbackEvent.DirectionChange -> HapticFeedbackConstants.CLOCK_TICK
            // Engaging a hold is a state change the user has to be able to feel
            // without looking, so it is deliberately the heaviest thing here --
            // one short stronger tap, not a sustained vibration. Releasing it is
            // the lightest, so the two are told apart by weight rather than by
            // counting buzzes.
            // Arming is an offer, not a change: the lightest tick there is, and
            // the same one a D-pad sector crossing uses.
            TouchFeedbackEvent.LatchArmed -> HapticFeedbackConstants.CLOCK_TICK
            TouchFeedbackEvent.LatchEngaged -> HapticFeedbackConstants.LONG_PRESS
            TouchFeedbackEvent.LatchReleased -> HapticFeedbackConstants.CLOCK_TICK
            // The end of an analog trigger's travel, where a real GameCube
            // trigger has a switch under the thumb and a pane of glass has
            // nothing. A virtual-key tap rather than the heavier hold buzz: it is
            // a mechanical edge inside one continuous gesture, not a state the
            // user has to be told about, and it can legitimately happen several
            // times in a fight.
            TouchFeedbackEvent.TriggerDetent -> HapticFeedbackConstants.VIRTUAL_KEY
        }
        runCatching { view.performHapticFeedback(constant) }
    }
}
