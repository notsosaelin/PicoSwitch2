package dev.picoswitch.companion.ui.touch

import androidx.compose.ui.geometry.Offset
import dev.picoswitch.bridge.touch.TouchCardinalSlot
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
