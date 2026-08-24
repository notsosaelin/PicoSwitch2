package dev.picoswitch.companion.ui.touch

import androidx.compose.ui.geometry.Offset
import dev.picoswitch.bridge.touch.TouchOutputControl
import dev.picoswitch.bridge.touch.TouchProfileCatalog
import dev.picoswitch.bridge.touch.TouchProfileId
import dev.picoswitch.bridge.touch.TouchVisualRole
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

}
