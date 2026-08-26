package dev.picoswitch.bridge.touch

/**
 * What one touch contact is doing right now.
 *
 * [Cancel] is not a synonym for [Up]. A platform reports it when the gesture was
 * taken away — a system bar was swiped in, a dialog appeared, the window lost the
 * gesture — and the contact's last known position is meaningless afterwards. Both
 * end ownership, but only [Up] is a deliberate release by the user.
 */
enum class TouchPhase { Down, Move, Up, Cancel }

/**
 * One touch contact, in the portable vocabulary the control engine needs and
 * nothing more.
 *
 * [id] MUST be stable for the whole lifetime of the contact and MUST NOT be a
 * position in whatever array the platform delivered. Every touch platform this
 * project is likely to target reports a stable identifier alongside an index
 * that reorders between events, and keying ownership on the index is the classic
 * bug that works perfectly with two fingers and breaks the moment a third
 * arrives or the first lifts. See `TouchControlEngine`.
 *
 * [x] and [y] are in the same coordinate space the layout was resolved into —
 * see [TouchLayoutRegion]. The platform adapter is the only thing that knows
 * about display density, window origins or rotation; by the time a contact gets
 * here it is just a point in the same plane as the control geometry.
 *
 * [timeNanos] is a monotonic host stamp. Nothing in the CONTROL MATH reads it —
 * a stick's value is its position and nothing else — but the hold-to-latch
 * recognizer does, so a platform that passes zero gets diagnostics and no latch
 * gesture rather than a latch that fires on every second tap. That refusal is
 * deliberate: a recognizer running on a clock stuck at zero would toggle a
 * persistent hold at random, which is the worst failure this feature has.
 *
 * Pressure, tool type, contact ellipse and historical samples are deliberately
 * absent: none of them change what a gamepad control does, and every one of them
 * would be a platform-shaped field in a portable model.
 */
data class TouchContact(
    val id: Long,
    val phase: TouchPhase,
    val x: Float,
    val y: Float,
    val timeNanos: Long = 0L,
)

/**
 * Why every touch contribution was dropped.
 *
 * Recorded rather than merely logged: "the console kept walking" and "the app
 * cleared input but the link was already gone" are different defects with the
 * same symptom, and the reason of the last global release is what separates
 * them. Kept as project vocabulary so a platform adapter names a boundary rather
 * than inventing free text at each call site.
 */
enum class TouchReleaseReason {
    /** The contact lifted or the platform cancelled it; ordinary per-contact end. */
    ContactEnded,

    /** The user left the on-screen controller. */
    ModeExit,

    /** The host client stopped being visible or focused. */
    HostInactive,

    /** Control geometry changed, so every retained contact position is stale. */
    GeometryInvalidated,

    /** The confirmed console-facing controller profile changed. */
    PersonalityChanged,

    /** Gameplay routing stopped while the user edits a draft layout. */
    EditorEntered,

    /** Gameplay input moved to another host control set. */
    AuthorityChanged,

    /** The controller link dropped or was stopped. */
    LinkEnded,

    /** The control surface was torn down. */
    Disposed,

    /** A fault was caught at a boundary and the retained state is not trustworthy. */
    Fault,

    /**
     * Configuration changed under a hold that outlives contacts.
     *
     * Used only for double-tap latches: turning the setting off, or retiming the
     * gesture, must not leave a control held under rules that no longer apply.
     */
    SettingsChanged,
}
