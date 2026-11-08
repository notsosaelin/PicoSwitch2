package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The GameCube beans are drawn as a smooth curve through their twenty traced
 * anchors ([TouchGameCubeGeometry.smoothOutline]) on every companion. The first
 * desktop ports drew the anchors as a polygon (2026-11-08). The C#
 * TouchGameCubeOutlineTests pins the same numbers.
 */
class TouchGameCubeOutlineTest {
    @Test
    fun outlinePassesThroughEveryAnchorInOrderAndCloses() {
        for (role in listOf(TouchVisualRole.GameCubeBeanX, TouchVisualRole.GameCubeBeanY)) {
            val points = TouchGameCubeGeometry.orientedContour(role, 120f, 80f, 30f)
            val outline = TouchGameCubeGeometry.smoothOutline(points)
            assertEquals(points.size, outline.size)
            points.indices.forEach { index ->
                assertEquals(points[(index + 1) % points.size], outline[index].end)
            }
        }
    }

    @Test
    fun firstSegmentMatchesTheCSharpArithmetic() {
        val outline = TouchGameCubeGeometry.smoothOutline(
            TouchGameCubeGeometry.contour(TouchVisualRole.GameCubeBeanX),
        )
        assertEquals(0.234033f, outline[0].control1.x, 1e-4f)
        assertEquals(0.004633f, outline[0].control1.y, 1e-4f)
        assertEquals(0.160383f, outline[0].control2.x, 1e-4f)
        assertEquals(0.025067f, outline[0].control2.y, 1e-4f)
        assertEquals(TouchVector(0.1126f, 0.0523f), outline[0].end)
    }
}
