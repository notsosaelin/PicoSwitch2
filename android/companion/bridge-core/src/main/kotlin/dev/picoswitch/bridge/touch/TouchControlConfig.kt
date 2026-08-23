package dev.picoswitch.bridge.touch

/**
 * Every tunable the touch control engine has, in one value.
 *
 * Kept together rather than scattered as constants because these are the numbers
 * a renderer would otherwise re-declare: a stick drawn with one deadzone and
 * evaluated with another is a control that visibly disagrees with itself.
 *
 * Defaults are an engineering baseline, not a measured optimum. The starting
 * deadzone follows the value mature touch-controller systems use; a touchscreen
 * has no spring, so the gate only has to cover the few pixels a resting thumb
 * wanders, and a large one would cost real range.
 */
data class TouchControlConfig(
    /** Inner fraction of a stick's travel radius that publishes exact centre. */
    val stickDeadzone: Float = 0.05f,

    /** Fraction of the D-pad radius a contact must reach to engage a direction. */
    val dpadEnterFraction: Float = 0.30f,

    /**
     * Fraction below which an engaged D-pad returns to neutral. Lower than
     * [dpadEnterFraction] on purpose: equal thresholds chatter at the boundary.
     */
    val dpadExitFraction: Float = 0.20f,

    /**
     * Extra angle a held D-pad direction keeps beyond its own sector.
     *
     * Large enough that a thumb resting on a boundary stays put, small enough
     * that a deliberate turn still lands within one sector of travel.
     */
    val dpadHysteresisDegrees: Float = 7f,
) {
    companion object { val Default = TouchControlConfig() }
}
