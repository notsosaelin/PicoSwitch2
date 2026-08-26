package dev.picoswitch.bridge.touch

/**
 * Cardinal slots shared by face and independent-direction button diamonds.
 *
 * These name a position on the PHYSICAL controller being represented — the top
 * button of its diamond is [North] whether or not that button ends up at the top
 * of the screen. Where the two differ, [TouchClusterRotation] is what maps one
 * to the other; nothing should read a slot as a screen position directly.
 */
enum class TouchCardinalSlot { North, East, South, West }

/**
 * How a physical control cluster is turned before it is drawn.
 *
 * A single Joy-Con used sideways is a whole controller rotated a quarter turn,
 * and the four buttons on its face go with it. Writing their screen positions
 * out by hand is how the layout ends up lying: the control named `direction-up`
 * gets placed at the top of the screen because the name says "up", when the
 * button it represents is physically pointing at the player's LEFT once the
 * shell is turned. The console reads the raw direction bits and applies its own
 * sideways interpretation, so the on-screen arrangement is the only thing that
 * can be wrong — and it was.
 *
 * Stating the rotation once, here, keeps the three things the layout has to get
 * right separable and separately checkable:
 *
 * ```text
 * physical identity   the button on the shell        TouchCardinalSlot / TouchOutputControl
 * logical action      what it sends                  TouchControllerProfile.bindings
 * screen position     where it is drawn              this
 * ```
 *
 * The direction matches the firmware's own statement of how each half is held —
 * `joycon2_pack_sideways_stick` rotates the left half's stick axes one way and
 * the right half's the other — so the touch layout and the report encoder cannot
 * disagree about which way a shell is turned.
 */
enum class TouchClusterRotation(
    /** Clockwise screen-space rotation, matching `TouchControlSpec.visualRotationDegrees`. */
    val degrees: Float,
) {
    /** Drawn as held: the shell's top is the screen's top. */
    Upright(0f),

    /** Joy-Con (R) sideways: the rail edge, and with it SL/SR, comes to the top. */
    QuarterClockwise(90f),

    /** Joy-Con (L) sideways: the rail edge, and with it SL/SR, comes to the top. */
    QuarterCounterClockwise(-90f);

    /**
     * Where a control that is physically at [physical] appears on screen.
     *
     * Turn a clock face a quarter turn anticlockwise and 12 lands where 9 was;
     * that is the whole of it.
     */
    fun screenSlot(physical: TouchCardinalSlot): TouchCardinalSlot = when (this) {
        Upright -> physical
        QuarterClockwise -> when (physical) {
            TouchCardinalSlot.North -> TouchCardinalSlot.East
            TouchCardinalSlot.East -> TouchCardinalSlot.South
            TouchCardinalSlot.South -> TouchCardinalSlot.West
            TouchCardinalSlot.West -> TouchCardinalSlot.North
        }
        QuarterCounterClockwise -> when (physical) {
            TouchCardinalSlot.North -> TouchCardinalSlot.West
            TouchCardinalSlot.West -> TouchCardinalSlot.South
            TouchCardinalSlot.South -> TouchCardinalSlot.East
            TouchCardinalSlot.East -> TouchCardinalSlot.North
        }
    }
}

/**
 * One control's placement relative to a group anchor.
 *
 * The anchor follows the available interaction rectangle while the offset stays
 * in logical units.  That distinction is what keeps a square diamond square on
 * screens whose aspect ratio differs from the 800 x 400 authoring reference.
 */
data class TouchGroupPlacement(
    val anchorX: Float,
    val anchorY: Float,
    val offsetXUnits: Float,
    val offsetYUnits: Float,
)

/** Defined geometry for a related control cluster, never four unrelated points. */
data class TouchGroupGeometry(
    val centerXUnits: Float,
    val centerYUnits: Float,
) {
    init {
        require(centerXUnits.isFinite() && centerYUnits.isFinite())
    }

    val anchorX: Float = centerXUnits / TouchLayoutResolver.REFERENCE_WIDTH_UNITS
    val anchorY: Float = centerYUnits / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS

    fun at(offsetXUnits: Float, offsetYUnits: Float) = TouchGroupPlacement(
        anchorX = anchorX,
        anchorY = anchorY,
        offsetXUnits = offsetXUnits,
        offsetYUnits = offsetYUnits,
    )

    /**
     * A four-button diamond, keyed by the slot each control occupies on the
     * PHYSICAL controller and placed where [rotation] puts that slot on screen.
     *
     * The key stays physical on purpose. A template that looked up "the button
     * I want at the top of the screen" would have to re-derive the rotation at
     * every call site and would silently drift out of step with the report
     * mapping; asking for the shell's own north button and letting the rotation
     * decide where it lands cannot.
     */
    fun squareDiamond(
        radiusUnits: Float,
        rotation: TouchClusterRotation = TouchClusterRotation.Upright,
    ): Map<TouchCardinalSlot, TouchGroupPlacement> {
        require(radiusUnits > 0f && radiusUnits.isFinite())
        val screen = mapOf(
            TouchCardinalSlot.North to at(0f, -radiusUnits),
            TouchCardinalSlot.East to at(radiusUnits, 0f),
            TouchCardinalSlot.South to at(0f, radiusUnits),
            TouchCardinalSlot.West to at(-radiusUnits, 0f),
        )
        return TouchCardinalSlot.entries.associateWith { physical ->
            screen.getValue(rotation.screenSlot(physical))
        }
    }
}
