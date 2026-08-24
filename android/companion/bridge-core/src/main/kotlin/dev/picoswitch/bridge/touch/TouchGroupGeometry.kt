package dev.picoswitch.bridge.touch

/** Cardinal slots shared by face and independent-direction button diamonds. */
enum class TouchCardinalSlot { North, East, South, West }

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

    fun squareDiamond(radiusUnits: Float): Map<TouchCardinalSlot, TouchGroupPlacement> {
        require(radiusUnits > 0f && radiusUnits.isFinite())
        return mapOf(
            TouchCardinalSlot.North to at(0f, -radiusUnits),
            TouchCardinalSlot.East to at(radiusUnits, 0f),
            TouchCardinalSlot.South to at(0f, radiusUnits),
            TouchCardinalSlot.West to at(-radiusUnits, 0f),
        )
    }
}
