package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Where the editor's toolbar can go, and the one property that matters most:
 * it can never end up somewhere the user cannot reach it.
 *
 * A toolbar off the edge of the window is a toolbar with no Done button, and the
 * only ways out of the editor then are the system back gesture or force-quitting
 * the app — both of which discard the layout being worked on.
 */
class TouchToolbarPlacementTest {

    private val region = TouchLayoutRegion(0f, 0f, 1000f, 500f, 1f)
    private val width = 300f
    private val height = 60f

    private fun placementAt(x: Float, y: Float) =
        TouchToolbarLayout.placementFor(x, y, width, height, region)

    @Test fun `the centre of the window is a free float`() {
        val placement = placementAt(400f, 220f)
        assertTrue("$placement", placement is TouchToolbarPlacement.Floating)
        placement as TouchToolbarPlacement.Floating
        assertEquals(0.4f, placement.x, 1e-4f)
        assertEquals(0.44f, placement.y, 1e-4f)
    }

    @Test fun `each safe edge is reachable and only from inside its own zone`() {
        // The zone is one toolbar thickness, so a bar 60 tall docks within 60 of
        // an edge and floats beyond it.
        val zone = minOf(width, height)
        assertEquals(
            TouchToolbarPlacement.Docked(TouchToolbarEdge.Top),
            placementAt(350f, zone - 1f),
        )
        assertEquals(
            TouchToolbarPlacement.Docked(TouchToolbarEdge.Bottom),
            placementAt(350f, region.bottom - height - zone + 1f),
        )
        assertEquals(
            TouchToolbarPlacement.Docked(TouchToolbarEdge.Left),
            placementAt(zone - 1f, 220f),
        )
        assertEquals(
            TouchToolbarPlacement.Docked(TouchToolbarEdge.Right),
            placementAt(region.right - width - zone + 1f, 220f),
        )
    }

    @Test fun `leaving the zone cancels the candidate`() {
        val zone = minOf(width, height)
        assertEquals(
            TouchToolbarEdge.Left,
            TouchToolbarLayout.dockCandidate(zone - 1f, 220f, width, height, region),
        )
        assertNull(
            TouchToolbarLayout.dockCandidate(zone + 40f, 220f, width, height, region),
        )
    }

    @Test fun `the nearest edge wins a corner`() {
        // Nearer the top than the left: the toolbar will visibly line up there.
        assertEquals(
            TouchToolbarEdge.Top,
            TouchToolbarLayout.dockCandidate(40f, 2f, width, height, region),
        )
        assertEquals(
            TouchToolbarEdge.Left,
            TouchToolbarLayout.dockCandidate(2f, 40f, width, height, region),
        )
    }

    @Test fun `a floating toolbar is always clamped back into the window`() {
        listOf(
            TouchToolbarPlacement.Floating(2f, 2f),
            TouchToolbarPlacement.Floating(-1f, -1f),
            TouchToolbarPlacement.Floating(0.99f, 0.99f),
        ).forEach { placement ->
            val (x, y) = TouchToolbarLayout.topLeft(placement, width, height, region)
            assertTrue("$placement left", x >= region.left - 1e-3f)
            assertTrue("$placement top", y >= region.top - 1e-3f)
            assertTrue("$placement right", x + width <= region.right + 1e-3f)
            assertTrue("$placement bottom", y + height <= region.bottom + 1e-3f)
        }
    }

    /**
     * The safe rectangle, not the window.
     *
     * A docked toolbar aligned to the physical edge would sit under the system
     * gesture strip or behind a cutout — exactly what the interaction-safe
     * region exists to keep controls out of.
     */
    @Test fun `every docked edge lands inside the safe region, insets and all`() {
        val inset = TouchLayoutRegion(48f, 60f, 952f, 440f, 1f)
        val expected = mapOf(
            TouchToolbarEdge.Top to (350f to 60f),
            TouchToolbarEdge.Bottom to (350f to (440f - height)),
            TouchToolbarEdge.Left to (48f to (60f + (380f - height) / 2f)),
            TouchToolbarEdge.Right to ((952f - width) to (60f + (380f - height) / 2f)),
        )
        TouchToolbarEdge.entries.forEach { edge ->
            val (x, y) = TouchToolbarLayout.topLeft(
                TouchToolbarPlacement.Docked(edge), width, height, inset,
            )
            val (ex, ey) = expected.getValue(edge)
            assertEquals("$edge x", ex, x, 1e-3f)
            assertEquals("$edge y", ey, y, 1e-3f)
            assertTrue("$edge left", x >= inset.left - 1e-3f)
            assertTrue("$edge top", y >= inset.top - 1e-3f)
            assertTrue("$edge right", x + width <= inset.right + 1e-3f)
            assertTrue("$edge bottom", y + height <= inset.bottom + 1e-3f)
        }
    }

    /**
     * Docking to an edge and reading that edge's position must agree, or the
     * toolbar would jump on release.
     */
    @Test fun `docking where the preview said lands where the dock is drawn`() {
        TouchToolbarEdge.entries.forEach { edge ->
            val docked = TouchToolbarPlacement.Docked(edge)
            val (x, y) = TouchToolbarLayout.topLeft(docked, width, height, region)
            assertEquals(
                "$edge is still inside its own snap zone",
                docked,
                TouchToolbarLayout.placementFor(x, y, width, height, region),
            )
        }
    }

    /**
     * A window that changed shape must not strand a remembered position.
     *
     * The same normalized placement is re-clamped against whatever rectangle is
     * on screen now, so rotating the device or resizing a freeform window brings
     * the toolbar back rather than leaving it half off the side.
     */
    @Test fun `a placement from a larger window survives a smaller one`() {
        val remembered = TouchToolbarPlacement.Floating(0.95f, 0.95f)
        val narrow = TouchLayoutRegion(0f, 0f, 320f, 700f, 1f)
        val (x, y) = TouchToolbarLayout.topLeft(remembered, width, height, narrow)
        assertTrue(x + width <= narrow.right + 1e-3f)
        assertTrue(y + height <= narrow.bottom + 1e-3f)
    }

    @Test fun `a docked toolbar needs no repair whatever the window does`() {
        TouchToolbarEdge.entries.forEach { edge ->
            val docked = TouchToolbarPlacement.Docked(edge)
            assertEquals(docked, TouchToolbarLayout.clamp(docked, width, height, region))
            assertEquals(
                docked,
                TouchToolbarLayout.clamp(
                    docked, width, height, TouchLayoutRegion(0f, 0f, 10f, 10f, 1f),
                ),
            )
        }
    }

    /**
     * The invariant, stated once and checked against everything that can change
     * the geometry underneath a remembered placement.
     *
     * A toolbar whose leading corner has left the safe rectangle has taken the
     * drag handle with it, and there is then no gesture that brings it back and
     * no Done button to leave the editor by.
     */
    @Test fun `the toolbar's leading corner is reachable in every geometry`() {
        val regions = mapOf(
            "landscape phone" to TouchLayoutRegion(0f, 0f, 1000f, 500f, 1f),
            "portrait phone" to TouchLayoutRegion(0f, 0f, 500f, 1000f, 1f),
            "heavy insets" to TouchLayoutRegion(96f, 72f, 904f, 428f, 1f),
            "square tablet" to TouchLayoutRegion(0f, 0f, 800f, 780f, 1f),
            "tiny" to TouchLayoutRegion(0f, 0f, 180f, 120f, 1f),
            "narrower than the toolbar" to TouchLayoutRegion(0f, 0f, 120f, 400f, 1f),
            "shorter than the toolbar" to TouchLayoutRegion(0f, 0f, 900f, 40f, 1f),
        )
        val placements = TouchToolbarEdge.entries.map { TouchToolbarPlacement.Docked(it) } +
            listOf(
                TouchToolbarPlacement.Floating(0f, 0f),
                TouchToolbarPlacement.Floating(1f, 1f),
                TouchToolbarPlacement.Floating(0.5f, 0.5f),
                // What a corrupt or pre-clamp stored value could carry.
                TouchToolbarPlacement.Floating(9f, -4f),
                TouchToolbarPlacement.Floating(Float.NaN, Float.NaN),
            )
        regions.forEach { (name, region) ->
            placements.forEach { placement ->
                val (x, y) = TouchToolbarLayout.topLeft(placement, width, height, region)
                assertTrue("$name / $placement: x=$x", x.isFinite() && y.isFinite())
                assertTrue("$name / $placement left", x >= region.left - 1e-3f)
                assertTrue("$name / $placement top", y >= region.top - 1e-3f)
                // The leading corner is inside; the far edge may overflow only
                // when the toolbar is genuinely larger than the window.
                assertTrue("$name / $placement right", x <= region.right + 1e-3f)
                assertTrue("$name / $placement bottom", y <= region.bottom + 1e-3f)
            }
        }
    }

    /**
     * A stored placement is re-read against whatever rectangle is on screen NOW.
     *
     * Rotating the device, resizing the window, a personality switch that
     * reloads the layout, or simply reopening the editor all go through the same
     * path, so one test covers them: the remembered value never carries stale
     * coordinates into a new geometry.
     */
    @Test fun `a placement remembered in one geometry is rehomed in the next`() {
        val landscape = TouchLayoutRegion(0f, 0f, 1000f, 500f, 1f)
        val portrait = TouchLayoutRegion(0f, 0f, 500f, 1000f, 1f)
        // Parked in the bottom-right of a wide window.
        val remembered = TouchToolbarLayout.placementFor(
            690f, 430f, width, height, landscape,
        )
        val (x, y) = TouchToolbarLayout.topLeft(remembered, width, height, portrait)
        assertTrue(x >= portrait.left - 1e-3f)
        assertTrue(y >= portrait.top - 1e-3f)
        assertTrue(x + width <= portrait.right + 1e-3f)
        assertTrue(y + height <= portrait.bottom + 1e-3f)
    }

    @Test fun `a toolbar larger than the window keeps its handle on screen`() {
        // Every edge, in a window narrower and shorter than the toolbar itself.
        val cramped = TouchLayoutRegion(20f, 30f, 140f, 70f, 1f)
        TouchToolbarEdge.entries.forEach { edge ->
            val (x, y) = TouchToolbarLayout.topLeft(
                TouchToolbarPlacement.Docked(edge), width, height, cramped,
            )
            assertEquals("$edge x", cramped.left, x, 1e-3f)
            assertEquals("$edge y", cramped.top, y, 1e-3f)
        }
    }

    @Test fun `a degenerate region never produces a nonsense placement`() {
        val empty = TouchLayoutRegion(0f, 0f, 0f, 0f, 1f)
        assertNull(TouchToolbarLayout.dockCandidate(0f, 0f, width, height, empty))
        val placement = TouchToolbarLayout.placementFor(0f, 0f, width, height, empty)
        assertTrue("$placement", placement is TouchToolbarPlacement.Floating)
    }

    /** The preview a drag shows and the placement a release commits are one value. */
    @Test fun `the docking preview and the docking result cannot disagree`() {
        listOf(10f to 10f, 500f to 250f, 990f to 480f, 5f to 250f).forEach { (x, y) ->
            val candidate = TouchToolbarLayout.dockCandidate(x, y, width, height, region)
            val placement = TouchToolbarLayout.placementFor(x, y, width, height, region)
            if (candidate == null) {
                assertTrue("$x,$y", placement is TouchToolbarPlacement.Floating)
            } else {
                assertEquals(TouchToolbarPlacement.Docked(candidate), placement)
            }
        }
    }
}
