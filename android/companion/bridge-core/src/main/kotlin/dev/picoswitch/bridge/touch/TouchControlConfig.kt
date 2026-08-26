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

    /** Double-tap-to-hold timing and the default for controls that state none. */
    val latch: TouchLatchConfig = TouchLatchConfig(),
) {
    companion object { val Default = TouchControlConfig() }
}

/**
 * Timing, distances and defaults for the hold gestures, and for the retrigger they make
 * possible.
 *
 * Engaging a hold is a double tap whose second press is held AND THEN SLID;
 * releasing one is a single press held for half as long:
 *
 * ```text
 * engage:  press   release            press                       armed        slide
 *            |<-- tap 1 -->|<-- gap -->|<-------- dwell -------------->|--------->| -> latch
 *            |             |           |                               |          |
 *            |<= maxTapDuration ------>|<-- latchEngageThresholdNanos ->|          |
 *                          |<-------->|                   latchCommitDistanceUnits |
 *                  minTapGapNanos .. doubleTapWindowNanos
 *
 * release: press                        still down
 *            |<--------- dwell --------------->| -> unlatch
 *            |<-- latchReleaseThresholdNanos ->|
 * ```
 *
 * **Timing alone cannot create a hold, because timing alone collides with real
 * play.** A plain double tap collides with mashing, which IS a stream of double
 * taps. A double tap whose second press is merely held collides with the very
 * ordinary "double tap, then keep holding" a game may ask for directly — and no
 * dwell separates those, because they are the same input. So the dwell only ARMS
 * the gesture; a deliberate slide away from where the press began is what
 * commits it. Nothing a game asks a player to do involves pressing a button and
 * dragging off it.
 *
 * **Removing a hold stays deliberately easier than creating one.** A hold the
 * user did not mean is a stuck button they have to diagnose; a hold they lose by
 * accident is one gesture away from coming back. Releasing needs no leading tap,
 * no slide, and half the dwell.
 *
 * The first three timings are PLATFORM CONVENTIONS, not invented constants. A
 * host adapter is expected to overwrite them with the values its own toolkit
 * reports — on Android that is `ViewConfiguration.doubleTapTimeoutMillis`,
 * `doubleTapMinTimeMillis` and `longPressTimeoutMillis` — so the gesture matches
 * every other double tap on the device, including whatever the user's
 * accessibility settings have done to it. The defaults here are the stock
 * Android numbers so a host that never sets them still behaves conventionally.
 *
 * [holdThresholdNanos], the two thresholds derived from it, [gestureSlopUnits]
 * and [retriggerReleaseNanos] have no platform equivalent and are the project's
 * own, kept here rather than buried in pointer logic so they remain tunable from
 * one place after gameplay feel testing.
 */
data class TouchLatchConfig(
    /** What a control with no explicit per-control choice does. */
    val enabledByDefault: Boolean = true,

    /** Longest gap between one tap's release and the next tap's press. */
    val doubleTapWindowNanos: Long = 300_000_000L,

    /**
     * Shortest gap that counts. Guards against a contact that bounced rather
     * than a finger that tapped twice; the platform's own detector has the same
     * floor for the same reason.
     */
    val minTapGapNanos: Long = 40_000_000L,

    /**
     * Longest press that still counts as a tap.
     *
     * A long press is not a tap, so a control held past this cannot become half
     * of a double tap. Matching the platform's long-press timeout is what makes
     * "I held it" and "I tapped it" mean the same thing here as everywhere else.
     *
     * Applies only to a press being REMEMBERED as the first tap. The second
     * press is deliberately held and its duration is unbounded.
     */
    val maxTapDurationNanos: Long = 500_000_000L,

    /**
     * The base deliberate-hold duration both latch dwells are derived from.
     *
     * Chosen to sit in the gap between two things a thumb does. A mashed press
     * is contact for roughly 30-80 ms and a player sustaining eight taps a
     * second has no press anywhere near this long; a deliberate "and hold" is
     * something a player does without waiting. Well short of the platform's
     * 500 ms long-press timeout on purpose — this has to stay usable mid-fight,
     * not feel like a context menu.
     *
     * ONE number, with [latchEngageThresholdNanos] and
     * [latchReleaseThresholdNanos] derived from it, so the asymmetry between
     * engaging and releasing is a stated rule rather than two constants that can
     * drift apart. Not a user-facing setting: if gameplay shows it wants tuning,
     * tune it here and both dwells move together.
     */
    val holdThresholdNanos: Long = 180_000_000L,

    /**
     * How long a retrigger pulse masks the hold so the release edge is real.
     *
     * A latched control is already published as pressed, so tapping it can only
     * produce a new press edge if a release is genuinely OBSERVABLE first. The
     * session coalesces state onto a 125 Hz report cadence through a conflated
     * mailbox, so a release and a press emitted in the same instant collapse
     * into no change at all. This is sized to survive both that and the
     * consumer: five report intervals, and longer than one frame at 30 Hz, so a
     * game polling at any ordinary rate cannot miss it. Short enough that a
     * held run button visibly does not stutter.
     */
    val retriggerReleaseNanos: Long = 48_000_000L,

    /**
     * How far a dwelling contact may drift before the gesture is abandoned.
     *
     * In LOGICAL UNITS, so the tolerance is a physical distance on the glass
     * rather than a fraction of a control: a small button must not be harder to
     * hold than a large one, and touching near an edge must not make the gesture
     * fail. A dwell is a much longer gesture than a tap, so this is deliberately
     * more forgiving than the platform's drag slop — a thumb settling for a third
     * of a second wanders further than one deciding whether to scroll.
     */
    val gestureSlopUnits: Float = 24f,

    /**
     * How far an ARMED contact must slide from where it began to commit a latch.
     *
     * A distinct threshold rather than a multiple of [gestureSlopUnits], because
     * the two answer different questions: slop is "did the user stay still",
     * this is "did the user perform a deliberate motion". Sized well clear of
     * the first so no amount of jitter can reach it, and comfortably reachable
     * by a thumb that is already on the glass — roughly a centimetre on a
     * typical handset, which is a slide the user cannot make by accident and
     * never has to think about making on purpose.
     *
     * In LOGICAL UNITS, and deliberately unrelated to the control's own size:
     * a small button must not be harder to lock than a large one, and touching
     * near an edge must not change the gesture at all.
     */
    val latchCommitDistanceUnits: Float = 64f,
) {
    /**
     * How long the second press must be held before a slide can commit a hold.
     *
     * Twice the base, because a hold the user did not mean is a stuck button
     * they have to work out how to clear. Reaching it ARMS the gesture; see
     * [latchCommitDistanceUnits] for what completes it.
     */
    val latchEngageThresholdNanos: Long get() = holdThresholdNanos * ENGAGE_MULTIPLE

    /**
     * How long a single press must be held to REMOVE a hold.
     *
     * The base itself, and no leading tap: undoing something the user can see is
     * wrong should be the easier half of the pair.
     */
    val latchReleaseThresholdNanos: Long get() = holdThresholdNanos

    companion object {
        /** Engaging is intentionally this much more deliberate than releasing. */
        const val ENGAGE_MULTIPLE = 2L
    }
}
