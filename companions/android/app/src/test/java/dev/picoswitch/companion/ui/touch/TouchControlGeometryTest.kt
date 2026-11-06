package dev.picoswitch.companion.ui.touch

import androidx.compose.ui.geometry.Offset
import dev.picoswitch.bridge.touch.TouchCardinalSlot
import dev.picoswitch.bridge.touch.TouchClusterRotation
import dev.picoswitch.bridge.touch.TouchOutputControl
import dev.picoswitch.bridge.touch.TouchProfileCatalog
import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.bridge.touch.TouchVisualRole
import kotlin.math.hypot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchControlGeometryTest {
    @Test fun `Joy-Con direction triangles are centered and point toward their output`() {
        val center = Offset(50f, 70f)
        val radius = 10f
        val outputs = listOf(
            TouchOutputControl.DirectionUp,
            TouchOutputControl.DirectionRight,
            TouchOutputControl.DirectionDown,
            TouchOutputControl.DirectionLeft,
        )
        outputs.forEach { output ->
            val points = joyConDirectionTriangle(output, center, radius)
            assertEquals(3, points.size)
            assertEquals(center.x, points.sumOf { it.x.toDouble() }.toFloat() / 3f, 0.5f)
            assertEquals(center.y, points.sumOf { it.y.toDouble() }.toFloat() / 3f, 0.5f)
            when (output) {
                TouchOutputControl.DirectionUp -> assertEquals(center.y - radius, points[0].y, 0f)
                TouchOutputControl.DirectionRight -> assertEquals(center.x + radius, points[0].x, 0f)
                TouchOutputControl.DirectionDown -> assertEquals(center.y + radius, points[0].y, 0f)
                TouchOutputControl.DirectionLeft -> assertEquals(center.x - radius, points[0].x, 0f)
                else -> error("unreachable")
            }
        }
    }

    /**
     * The marking is authored in the JOY-CON'S own frame and turned with the
     * shell, so a rotated `DirectionUp` points where the shell's up button now
     * points. Deriving the arrow from the screen slot instead would look
     * identical today and silently break the moment a template placed one of
     * these anywhere else.
     */
    @Test fun `a rotated direction marking turns with the shell`() {
        val center = Offset(50f, 70f)
        val radius = 10f
        // Joy-Con (L) is held a quarter turn anticlockwise; its up button then
        // points at the player's left.
        val turned = joyConDirectionTriangle(
            TouchOutputControl.DirectionUp, center, radius,
            TouchClusterRotation.QuarterCounterClockwise.degrees,
        )
        assertEquals(center.x - radius, turned[0].x, 1e-3f)
        assertEquals(center.y, turned[0].y, 1e-3f)
        assertEquals(center.x, turned.sumOf { it.x.toDouble() }.toFloat() / 3f, 0.5f)
        assertEquals(center.y, turned.sumOf { it.y.toDouble() }.toFloat() / 3f, 0.5f)
    }

    /**
     * What the player actually sees: on the shipped sideways Joy-Con (L), every
     * arrow points away from the cluster centre, along the axis its own button
     * sits on. That is the property that makes the cluster read as a face
     * diamond rather than as four arrows pointing the wrong way.
     */
    @Test fun `every shipped Joy-Con L arrow points along its own screen position`() {
        val template = TouchProfileCatalog.require(TouchProfileId.JoyConLeft).defaultTemplate
        val directions = template.controls.filter {
            it.visual.role == TouchVisualRole.JoyConDirectionButton
        }
        assertEquals(4, directions.size)
        val centerX = directions.map { it.geometry.groupOffsetXUnits }.average().toFloat()
        val centerY = directions.map { it.geometry.groupOffsetYUnits }.average().toFloat()
        assertEquals("the cluster is centred on its anchor", 0f, centerX, 1e-3f)
        assertEquals("the cluster is centred on its anchor", 0f, centerY, 1e-3f)

        directions.forEach { control ->
            val drawn = Offset(50f, 70f)
            val apex = joyConDirectionTriangle(
                control.output, drawn, radius = 10f,
                rotationDegrees = control.visual.rotationDegrees,
            )[0] - drawn
            // The button's own offset from the cluster centre, as a direction.
            val fromCentre = Offset(
                control.geometry.groupOffsetXUnits,
                control.geometry.groupOffsetYUnits,
            )
            val length = hypot(fromCentre.x, fromCentre.y)
            assertTrue("${control.id} sits off the cluster centre", length > 0f)
            val dot = (apex.x * fromCentre.x + apex.y * fromCentre.y) / (length * 10f)
            assertEquals("${control.id} arrow must point outward", 1f, dot, 1e-3f)
        }
    }

    @Test fun `GameCube X and Y retain the separate Dolphin alpha silhouettes`() {
        val x = gameCubeFaceContour(TouchVisualRole.GameCubeBeanX)
        val y = gameCubeFaceContour(TouchVisualRole.GameCubeBeanY)

        assertEquals(20, x.size)
        assertEquals(20, y.size)
        assertEquals(Offset(0.2994f, 0f), x[0])
        assertEquals(Offset(0.2038f, 0.5181f), x[5])
        assertEquals(Offset(1f, 0.5631f), x[14])
        assertEquals(Offset(0.0039f, 0.6576f), y[6])
        assertEquals(Offset(0.2614f, 0.9939f), y[9])
        assertEquals(Offset(0.9991f, 0.3705f), y[16])
        assertTrue(x != y)
    }

    @Test fun `joystick knob proportion remains the original PicoSwitch2 value`() {
        assertEquals(0.46f, TOUCH_STICK_KNOB_FRACTION, 0f)
    }

    /**
     * The proportions the Windows surface copies.
     *
     * Both companions draw the same controller, and until 2026-09-03 they did not:
     * Windows derived its proportions and colours from Fluent theme resources while
     * these were resolved from the Material theme, and the result was square-ish
     * pads, no visible outlines, a white D-pad and a white stick knob. Windows now
     * carries these numbers literally, pinned by `TouchVisualParityTests`.
     *
     * Pinning them on BOTH sides is the point. One side alone catches a Windows
     * edit and misses an Android one, which is the direction the drift actually
     * came from.
     */
    @Test fun `shared visual proportions match the values Windows mirrors`() {
        assertEquals(2f, OUTLINE_WIDTH, 0f)
        assertEquals(0.90f, ARM_FRACTION, 0f)
        assertEquals(0.26f, ARM_HALF_WIDTH, 0f)
        assertEquals(0.55f, WELL_ALPHA, 0f)
        assertEquals(0.78f, LABEL_WIDTH_FRACTION, 0f)
        assertEquals(0.68f, LABEL_HEIGHT_FRACTION, 0f)
        assertEquals(0.28f, CAPTURE_DISC_RADIUS_FRACTION, 0f)
        assertEquals(0.18f, CAPTURE_DISC_FILL_ALPHA, 0f)
        assertEquals(0.82f, CAPTURE_DISC_RIM_ALPHA, 0f)
        assertEquals(0.045f, CAPTURE_DISC_STROKE_FRACTION, 0f)
        assertEquals(0.30f, HOME_CIRCLE_RADIUS_FRACTION, 0f)
        assertEquals(0.05f, HOME_CIRCLE_STROKE_FRACTION, 0f)
        assertEquals(0.18f, HOME_HOUSE_UNIT_FRACTION, 0f)
    }

    @Test fun `oriented GameCube contours stay inside their existing touch margins`() {
        val x = orientedGameCubeFaceContour(
            TouchVisualRole.GameCubeBeanX, width = 52f, height = 84f, rotationDegrees = 10.7f,
        )
        val y = orientedGameCubeFaceContour(
            TouchVisualRole.GameCubeBeanY, width = 84f, height = 54f, rotationDegrees = -11f,
        )
        assertTrue(x.maxOf { kotlin.math.abs(it.x) } <= 52f / 2f + 7f)
        assertTrue(x.maxOf { kotlin.math.abs(it.y) } <= 84f / 2f + 7f)
        assertTrue(y.maxOf { kotlin.math.abs(it.x) } <= 84f / 2f + 7f)
        assertTrue(y.maxOf { kotlin.math.abs(it.y) } <= 54f / 2f + 7f)
    }

    @Test fun `GameCube X and Y visual edge gaps are balanced around A`() {
        val template = TouchProfileCatalog.require(TouchProfileId.GameCube).defaultTemplate
        fun center(output: TouchOutputControl): Offset {
            val geometry = template.controls.single { it.output == output }.geometry
            return Offset(
                geometry.anchorX * 800f + geometry.groupOffsetXUnits,
                geometry.anchorY * 400f + geometry.groupOffsetYUnits,
            )
        }
        fun edgeGap(center: Offset, contour: List<Offset>, a: Offset): Float = contour.minOf { point ->
            kotlin.math.hypot(center.x + point.x - a.x, center.y + point.y - a.y) - 44f
        }

        val a = center(TouchOutputControl.A)
        val xGap = edgeGap(
            center(TouchOutputControl.X),
            orientedGameCubeFaceContour(
                TouchVisualRole.GameCubeBeanX, width = 52f, height = 84f, rotationDegrees = 10.7f,
            ),
            a,
        )
        val yGap = edgeGap(
            center(TouchOutputControl.Y),
            orientedGameCubeFaceContour(
                TouchVisualRole.GameCubeBeanY, width = 84f, height = 54f, rotationDegrees = -11f,
            ),
            a,
        )
        assertEquals(xGap, yGap, 0.2f)
    }

    // ------------------------------------------------------------- D-pad fill
    //
    // The pressed D-pad fill was once four rounded rectangles laid over the
    // cross, which squared off the body's rounded arm ends and left notches
    // where two of them met. It is now four wedges that meet at the exact
    // centre, intersected with the body. These pin the properties that made the
    // old version wrong, so it cannot come back unnoticed.

    private val dpadCenter = Offset(120f, 90f)
    private val dpadArm = 40f
    private val dpadHalf = 12f

    private fun wedge(slot: TouchCardinalSlot, overshoot: Float = 3f) =
        dpadDirectionWedge(slot, dpadCenter, dpadArm, dpadHalf, overshoot)

    @Test fun `every direction wedge terminates at the exact shared centre`() {
        TouchCardinalSlot.entries.forEach { slot ->
            val points = wedge(slot)
            assertEquals(5, points.size)
            assertEquals("$slot", 1, points.count { it == dpadCenter })
        }
    }

    @Test fun `adjacent wedges share a whole edge so a diagonal unions without a seam`() {
        // North and East meet along centre -> (+half, -half); North and West
        // along centre -> (-half, -half); and so on around the hub. A shared
        // EDGE, not merely a shared point, is what makes the union continuous.
        fun edges(slot: TouchCardinalSlot): Set<Set<Offset>> {
            val points = wedge(slot)
            return points.indices.mapTo(mutableSetOf()) { i ->
                setOf(points[i], points[(i + 1) % points.size])
            }
        }
        listOf(
            TouchCardinalSlot.North to TouchCardinalSlot.East,
            TouchCardinalSlot.East to TouchCardinalSlot.South,
            TouchCardinalSlot.South to TouchCardinalSlot.West,
            TouchCardinalSlot.West to TouchCardinalSlot.North,
        ).forEach { (a, b) ->
            val shared = edges(a).intersect(edges(b))
            assertEquals("$a + $b", 1, shared.size)
            assertTrue("$a + $b", shared.single().contains(dpadCenter))
        }
        // Opposite directions touch only at the hub itself.
        assertTrue(
            edges(TouchCardinalSlot.North).intersect(edges(TouchCardinalSlot.South)).isEmpty(),
        )
    }

    @Test fun `a wedge overshoots only its own arm end and never the perpendicular axis`() {
        val overshoot = 3f
        val north = wedge(TouchCardinalSlot.North, overshoot)
        // Past the flat arm end, so the intersection with the body decides the
        // rounded tip rather than float noise leaving an unlit hairline.
        assertEquals(dpadCenter.y - dpadArm - overshoot, north.minOf { it.y }, 0f)
        // Exact across the arm: a wider wedge would spill into the arm beside it
        // once it reached the central square.
        assertEquals(dpadCenter.x - dpadHalf, north.minOf { it.x }, 0f)
        assertEquals(dpadCenter.x + dpadHalf, north.maxOf { it.x }, 0f)

        val west = wedge(TouchCardinalSlot.West, overshoot)
        assertEquals(dpadCenter.x - dpadArm - overshoot, west.minOf { it.x }, 0f)
        assertEquals(dpadCenter.y - dpadHalf, west.minOf { it.y }, 0f)
        assertEquals(dpadCenter.y + dpadHalf, west.maxOf { it.y }, 0f)
    }

    @Test fun `the four wedges are congruent rotations, so the fill scales uniformly`() {
        fun signature(slot: TouchCardinalSlot) = wedge(slot)
            .map { hypot(it.x - dpadCenter.x, it.y - dpadCenter.y) }
            .sorted()
            .map { kotlin.math.round(it * 1000f) / 1000f }
        val north = signature(TouchCardinalSlot.North)
        TouchCardinalSlot.entries.forEach { assertEquals("$it", north, signature(it)) }
    }

    @Test fun `wedge geometry is proportional at any D-pad size`() {
        val small = dpadDirectionWedge(TouchCardinalSlot.North, Offset.Zero, 10f, 3f, 0.75f)
        val large = dpadDirectionWedge(TouchCardinalSlot.North, Offset.Zero, 100f, 30f, 7.5f)
        small.indices.forEach { i ->
            assertEquals(small[i].x * 10f, large[i].x, 1e-3f)
            assertEquals(small[i].y * 10f, large[i].y, 1e-3f)
        }
    }
}
