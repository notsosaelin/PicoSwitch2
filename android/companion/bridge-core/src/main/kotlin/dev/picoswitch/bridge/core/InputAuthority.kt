package dev.picoswitch.bridge.core

/**
 * Which host-local control set is currently driving gameplay input.
 *
 * This is HOST INPUT AUTHORITY, and it is deliberately not any of the other two
 * "source" ideas this product has:
 *
 * - the adapter's active input — which controller the adapter forwards to the
 *   console — is a management concept and is untouched by this;
 * - the selected host input device — which physical pad feeds this client — is a
 *   platform concept and keeps its own persistence.
 *
 * Authority answers only "are the host's physical controls or its on-screen
 * controls the gameplay controller right now". Both can exist at once on a
 * handheld, and merging them is not a feature: a physical stick pushed left and
 * a touch stick pushed right have no defensible combined meaning, and whichever
 * event happened last would win by accident. So exactly one origin contributes
 * gameplay state, chosen explicitly.
 *
 * Software/meta buttons ([ControllerInputState.setVirtualButton]) are outside
 * this rule on purpose — Home, Capture and C/GameChat have no physical key on
 * this hardware, so they are host actions rather than a second controller.
 */
enum class InputAuthority { Physical, Touch }

/**
 * One on-screen controller's complete gameplay contribution, in bridge units.
 *
 * Produced whole rather than field by field: a single contact event can move a
 * stick, cross a D-pad sector boundary and release a button at once, and
 * publishing those separately would let a sender observe a half-applied event.
 * This mirrors why [AnalogFrame] exists for physical analog input.
 *
 * Axes are already `0..255` with `128` neutral and triggers `0..255` at rest —
 * the conversion from the touch engine's `[-1,+1]` geometry happens exactly once,
 * in `dev.picoswitch.bridge.touch.TouchAxis`, so no renderer or platform adapter
 * can introduce a second, subtly different scaling.
 *
 * [positionalButtons] holds face-diamond presses in their POSITION, exactly as a
 * physical backend reports them, so the one face-layout resolver decides what
 * they mean. [logicalButtons] holds actions that are already logical (shoulders,
 * triggers, stick clicks, `-`/`+`, Home, Capture, C) and are never face-swapped.
 */
data class TouchContribution(
    val leftX: Int = 128,
    val leftY: Int = 128,
    val rightX: Int = 128,
    val rightY: Int = 128,
    val leftTrigger: Int = 0,
    val rightTrigger: Int = 0,
    val dpad: DpadState = DpadState.None,
    val positionalButtons: Set<ControllerButton> = emptySet(),
    val logicalButtons: Set<ControllerButton> = emptySet(),
) {
    companion object { val Neutral = TouchContribution() }
}
