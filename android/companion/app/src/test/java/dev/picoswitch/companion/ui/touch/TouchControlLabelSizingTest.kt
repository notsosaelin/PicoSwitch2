package dev.picoswitch.companion.ui.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class TouchControlLabelSizingTest {
    @Test
    fun `short labels retain the shared global size`() {
        val scale = fitTouchLabelScale(
            measuredWidth = 44f,
            measuredHeight = 60f,
            availableWidth = 140f,
            availableHeight = 126f,
        )

        assertEquals(1f, scale, 0.0001f)
    }

    @Test
    fun `wide labels shrink to preserve horizontal padding`() {
        val availableWidth = 130f
        val measuredWidth = 180f
        val scale = fitTouchLabelScale(
            measuredWidth = measuredWidth,
            measuredHeight = 60f,
            availableWidth = availableWidth,
            availableHeight = 126f,
        )

        assertEquals(availableWidth * 0.78f / measuredWidth, scale, 0.0001f)
        assertTrue(measuredWidth * scale < availableWidth)
    }

    @Test
    fun `tall labels shrink to preserve vertical padding`() {
        val availableHeight = 100f
        val measuredHeight = 100f
        val scale = fitTouchLabelScale(
            measuredWidth = 40f,
            measuredHeight = measuredHeight,
            availableWidth = 140f,
            availableHeight = availableHeight,
        )

        assertEquals(availableHeight * 0.68f / measuredHeight, scale, 0.0001f)
        assertTrue(measuredHeight * scale < availableHeight)
    }

    @Test
    fun `fitter never enlarges beyond the requested global size`() {
        val scale = fitTouchLabelScale(
            measuredWidth = 1f,
            measuredHeight = 1f,
            availableWidth = 1_000f,
            availableHeight = 1_000f,
        )

        assertEquals(1f, scale, 0.0001f)
    }
}
