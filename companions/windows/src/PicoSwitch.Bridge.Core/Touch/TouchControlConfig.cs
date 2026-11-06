namespace PicoSwitch.Bridge.Touch;

/// <summary>
/// Every tunable the touch control engine has, in one value.
///
/// Kept together rather than scattered as constants because these are the numbers
/// a renderer would otherwise re-declare: a stick drawn with one deadzone and
/// evaluated with another is a control that visibly disagrees with itself.
///
/// Defaults are an engineering baseline, not a measured optimum. The starting
/// deadzone follows the value mature touch-controller systems use; a touchscreen
/// has no spring, so the gate only has to cover the few pixels a resting thumb
/// wanders, and a large one would cost real range.
/// </summary>
public sealed record TouchControlConfig
{
    public static readonly TouchControlConfig Default = new();

    /// <summary>Inner fraction of a stick's travel radius that publishes exact centre.</summary>
    public float StickDeadzone { get; init; } = 0.05f;

    /// <summary>Fraction of the D-pad radius a contact must reach to engage a direction.</summary>
    public float DpadEnterFraction { get; init; } = 0.30f;

    /// <summary>
    /// Fraction below which an engaged D-pad returns to neutral. Lower than
    /// <see cref="DpadEnterFraction"/> on purpose: equal thresholds chatter at the
    /// boundary.
    /// </summary>
    public float DpadExitFraction { get; init; } = 0.20f;

    /// <summary>
    /// Extra angle a held D-pad direction keeps beyond its own sector.
    ///
    /// Large enough that a thumb resting on a boundary stays put, small enough
    /// that a deliberate turn still lands within one sector of travel.
    /// </summary>
    public float DpadHysteresisDegrees { get; init; } = 7f;

    /// <summary>Double-tap-to-hold timing and the default for controls that state none.</summary>
    public TouchLatchConfig Latch { get; init; } = new();

    /// <summary>Travel, slop and detent for the triggers that have real travel.</summary>
    public TouchTriggerConfig Trigger { get; init; } = new();
}

/// <summary>
/// Geometry and thresholds for an analog trigger's invisible travel axis.
///
/// ## The detent numbers are a wire contract, not a feel preference
///
/// On the NSO GameCube personality the touch path has no separate digital trigger
/// bit at all. The firmware's GameCube seam derives the terminal click from the
/// ANALOG BYTE for a generic bridge source (<c>ns2_seam.c</c>:
/// <c>analog[ANALOG_L2] &gt; 224</c>) and discards the <c>L2</c>/<c>R2</c> button
/// bits entirely, because a real pad's own click bit would otherwise stack a
/// second path on top of the same physical action.
///
/// That has one consequence worth stating plainly, because it is easy to
/// "simplify" away: a hysteresis band on a local Boolean would be decorative.
/// Whatever value is published IS the detent. So the band is enforced on the
/// PUBLISHED VALUE — below the detent the value is capped at
/// <see cref="SubDetentCeiling"/>, the largest byte the firmware still reads as
/// open — and the click can only ever be asserted by the detent itself.
/// </summary>
public sealed record TouchTriggerConfig
{
    /// <summary>
    /// The largest trigger byte the firmware's GameCube seam still reads as
    /// "not clicked". Mirrors <c>ns2_seam.c</c>.
    /// </summary>
    public const float SubDetentByte = 224f;

    /// <summary>
    /// Full travel for a purely HORIZONTAL pull, as a fraction of the region's
    /// SHORTER side.
    ///
    /// Scaled by the shorter side rather than by the width so the number cannot
    /// drift with the panel's aspect ratio: half the WIDTH of a 20:9 panel is a
    /// swipe longer than the screen is tall.
    /// </summary>
    public float TravelFraction { get; init; } = 0.50f;

    /// <summary>
    /// Full travel for a purely VERTICAL pull, relative to the horizontal one.
    ///
    /// Hardware feel testing, not a derivation: with one shared distance for every
    /// direction, horizontal and diagonal pulls felt right and near-vertical ones
    /// felt like they had to be dragged the whole way down the glass. The same
    /// absolute distance is a very different fraction of a landscape screen in
    /// each direction, and the thumb has correspondingly less range vertically.
    /// </summary>
    public float VerticalTravelRatio { get; init; } = 0.50f;

    /// <summary>
    /// How close to the middle of the region a control has to be before its
    /// inward vector stops being meaningful, in LOGICAL UNITS.
    /// </summary>
    public float CenterEpsilonUnits { get; init; } = 16f;

    /// <summary>
    /// How far a contact must move to become a pull rather than a tap, in LOGICAL
    /// UNITS.
    ///
    /// A PLATFORM CONVENTION, not an invented constant: a host adapter is expected
    /// to overwrite it with its own toolkit's drag slop, so starting a trigger
    /// pull takes the same movement as starting any other drag on the device.
    /// </summary>
    public float DragSlopUnits { get; init; } = 8f;

    /// <summary>Travel at which the terminal click engages.</summary>
    public float DetentEngageFraction { get; init; } = 0.92f;

    /// <summary>Travel at which it lets go again; see the type doc for why it is lower.</summary>
    public float DetentReleaseFraction { get; init; } = 0.84f;

    /// <summary>
    /// The most travel that may be published while the detent is open.
    ///
    /// <c>224/255</c> exactly, because the firmware seam's threshold is
    /// <c>&gt; 224</c>. Above this the console would see the click regardless of
    /// what this side believes, which would make the hysteresis band a local
    /// fiction.
    /// </summary>
    public float SubDetentCeiling { get; init; } = SubDetentByte / 255f;

    /// <summary>
    /// Refuse a configuration that cannot behave, rather than chattering later.
    ///
    /// A record's <c>init</c> setters cannot be validated in a constructor the way
    /// Kotlin's <c>init</c> block validates a data class, so the check is explicit
    /// and every consumer calls it once.
    /// </summary>
    public TouchTriggerConfig Validated()
    {
        if (DetentReleaseFraction >= DetentEngageFraction)
        {
            throw new ArgumentException(
                "The detent must let go below the travel that engages it, or it chatters");
        }

        if (SubDetentCeiling >= DetentEngageFraction)
        {
            throw new ArgumentException(
                "Sub-detent travel must stay below the value that asserts the click on the wire");
        }

        if (VerticalTravelRatio is <= 0f or > 1f)
        {
            throw new ArgumentException(
                "A vertical pull must be reachable and no longer than a horizontal one");
        }

        return this;
    }
}

/// <summary>
/// Timing, distances and defaults for the hold gestures, and for the retrigger
/// they make possible.
///
/// Engaging a hold is a double tap whose second press is held AND THEN SLID;
/// releasing one is a single press held for half as long.
///
/// **Timing alone cannot create a hold, because timing alone collides with real
/// play.** A plain double tap collides with mashing, which IS a stream of double
/// taps. A double tap whose second press is merely held collides with the very
/// ordinary "double tap, then keep holding" a game may ask for directly — and no
/// dwell separates those, because they are the same input. So the dwell only ARMS
/// the gesture; a deliberate slide away from where the press began is what commits
/// it. Nothing a game asks a player to do involves pressing a button and dragging
/// off it.
///
/// **Removing a hold stays deliberately easier than creating one.** A hold the
/// user did not mean is a stuck button they have to diagnose; a hold they lose by
/// accident is one gesture away from coming back. Releasing needs no leading tap,
/// no slide, and half the dwell.
/// </summary>
public sealed record TouchLatchConfig
{
    /// <summary>Engaging is intentionally this much more deliberate than releasing.</summary>
    public const long EngageMultiple = 2L;

    /// <summary>What a control with no explicit per-control choice does.</summary>
    public bool EnabledByDefault { get; init; } = true;

    /// <summary>Longest gap between one tap's release and the next tap's press.</summary>
    public long DoubleTapWindowNanos { get; init; } = 300_000_000L;

    /// <summary>
    /// Shortest gap that counts. Guards against a contact that bounced rather than
    /// a finger that tapped twice.
    /// </summary>
    public long MinTapGapNanos { get; init; } = 40_000_000L;

    /// <summary>
    /// Longest press that still counts as a tap.
    ///
    /// Applies only to a press being REMEMBERED as the first tap. The second press
    /// is deliberately held and its duration is unbounded.
    /// </summary>
    public long MaxTapDurationNanos { get; init; } = 500_000_000L;

    /// <summary>
    /// The base deliberate-hold duration both latch dwells are derived from.
    ///
    /// Chosen to sit in the gap between two things a thumb does. A mashed press is
    /// contact for roughly 30-80 ms; a deliberate "and hold" is something a player
    /// does without waiting. Well short of the platform's 500 ms long-press timeout
    /// on purpose — this has to stay usable mid-fight, not feel like a context menu.
    ///
    /// ONE number, with both thresholds derived from it, so the asymmetry between
    /// engaging and releasing is a stated rule rather than two constants that can
    /// drift apart.
    /// </summary>
    public long HoldThresholdNanos { get; init; } = 180_000_000L;

    /// <summary>
    /// How long a retrigger pulse masks the hold so the release edge is real.
    ///
    /// A latched control is already published as pressed, so tapping it can only
    /// produce a new press edge if a release is genuinely OBSERVABLE first. The
    /// session coalesces state onto a 125 Hz report cadence through a conflated
    /// mailbox, so a release and a press emitted in the same instant collapse into
    /// no change at all. Sized to survive both that and the consumer: five report
    /// intervals, and longer than one frame at 30 Hz.
    /// </summary>
    public long RetriggerReleaseNanos { get; init; } = 48_000_000L;

    /// <summary>
    /// How far a dwelling contact may drift before the gesture is abandoned.
    ///
    /// In LOGICAL UNITS, so the tolerance is a physical distance on the glass
    /// rather than a fraction of a control: a small button must not be harder to
    /// hold than a large one, and touching near an edge must not make the gesture
    /// fail.
    /// </summary>
    public float GestureSlopUnits { get; init; } = 24f;

    /// <summary>
    /// How far an ARMED contact must slide from where it began to commit a latch.
    ///
    /// A distinct threshold rather than a multiple of <see cref="GestureSlopUnits"/>,
    /// because the two answer different questions: slop is "did the user stay
    /// still", this is "did the user perform a deliberate motion". Sized well clear
    /// of the first so no amount of jitter can reach it, and comfortably reachable
    /// by a thumb already on the glass.
    /// </summary>
    public float LatchCommitDistanceUnits { get; init; } = 64f;

    /// <summary>
    /// How close to where it began an already-committed contact must return to take
    /// the hold back off, in LOGICAL UNITS.
    ///
    /// The SAME distance that decides whether a dwelling contact stayed still,
    /// because it is the same question asked twice. Being well inside
    /// <see cref="LatchCommitDistanceUnits"/> is what stops the gesture flapping.
    /// </summary>
    public float LatchCancelDistanceUnits => GestureSlopUnits;

    /// <summary>
    /// How long the second press must be held before a slide can commit a hold.
    ///
    /// Twice the base, because a hold the user did not mean is a stuck button they
    /// have to work out how to clear.
    /// </summary>
    public long LatchEngageThresholdNanos => HoldThresholdNanos * EngageMultiple;

    /// <summary>
    /// How long a single press must be held to REMOVE a hold. The base itself, and
    /// no leading tap: undoing something the user can see is wrong should be the
    /// easier half of the pair.
    /// </summary>
    public long LatchReleaseThresholdNanos => HoldThresholdNanos;

    /// <summary>See <see cref="TouchTriggerConfig.Validated"/> for why this is explicit.</summary>
    public TouchLatchConfig Validated()
    {
        if (LatchCancelDistanceUnits >= LatchCommitDistanceUnits)
        {
            throw new ArgumentException(
                "The cancel radius must sit inside the commit distance, or the gesture flaps");
        }

        return this;
    }
}
