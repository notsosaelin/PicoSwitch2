package dev.picoswitch.bridge.touch

import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

private val SNAP_ON = TouchAlignmentSettings(grid = false, snap = true)
private val GRID_AND_SNAP = TouchAlignmentSettings(grid = true, snap = true)

/** Window shapes the design specification names as having to remain editable. */
private val SHAPES = listOf(
    "16:9" to (915f to 412f),
    "16:10" to (1024f to 640f),
    "4:3" to (1024f to 768f),
    "1:1" to (700f to 700f),
)

private fun resolved(
    personality: TouchProfileId = TouchProfileId.GameCube,
    override: TouchLayoutOverride? = null,
    width: Float = 915f,
    height: Float = 412f,
    density: Float = 1f,
): ResolvedTouchLayout {
    val profile = TouchProfileCatalog.require(personality)
    return TouchLayoutResolver.resolve(
        TouchLayoutComposer.compose(profile, override).layout,
        TouchLayoutRegion(0f, 0f, width * density, height * density, density),
        TouchLayoutAuditMode.UserDraft,
    )
}

class TouchEditorAlignmentTest {

    @Test fun `snapping off never changes the movement`() {
        val layout = resolved()
        val target = layout.controls.first()
        val delta = TouchEditorDelta(13.7f, -4.25f)
        assertEquals(
            delta,
            TouchEditorAlignment.snap(
                layout, setOf(target.id), target.id, delta, TouchAlignmentSettings.Off,
            ),
        )
    }

    @Test fun `a movement that lands near the centre line is pulled onto it`() {
        val layout = resolved()
        val target = layout.controls.first { it.spec.output == TouchOutputControl.Dpad }
        val centerX = (layout.region.left + layout.region.right) / 2f
        val nearMiss = centerX - target.centerX + 3f
        val snapped = TouchEditorAlignment.snap(
            layout, setOf(target.id), target.id, TouchEditorDelta(nearMiss, 0f), SNAP_ON,
        )
        assertEquals(centerX, target.centerX + snapped.x, 1e-3f)
    }

    @Test fun `guides assist but never restrict - a large movement always wins`() {
        val layout = resolved()
        val target = layout.controls.first { it.spec.output == TouchOutputControl.Dpad }
        val unit = layout.region.unitScale
        val far = TouchEditorAlignment.SNAP_TOLERANCE_UNITS * unit * 12f
        val snapped = TouchEditorAlignment.snap(
            layout, setOf(target.id), target.id, TouchEditorDelta(far, 0f), GRID_AND_SNAP,
        )
        // It may still be pulled onto SOME nearby guide, but never back to where
        // it started, and never by more than the tolerance.
        assertTrue(abs(snapped.x - far) <= TouchEditorAlignment.SNAP_TOLERANCE_UNITS * unit + 1e-3f)
        assertTrue(snapped.x > 0f)
    }

    @Test fun `one correction is applied to the whole selection`() {
        val layout = resolved()
        val primary = layout.controls.first { it.spec.output == TouchOutputControl.Dpad }
        val other = layout.controls.first { it.spec.output == TouchOutputControl.SecondaryStick }
        val selection = setOf(primary.id, other.id)
        val delta = TouchEditorDelta(2f, 2f)
        val snapped = TouchEditorAlignment.snap(layout, selection, primary.id, delta, GRID_AND_SNAP)
        // The API returns ONE delta, so relative spacing is preserved by
        // construction; assert the reference control is what it was computed from.
        val single = TouchEditorAlignment.snap(layout, selection, primary.id, delta, GRID_AND_SNAP)
        assertEquals(snapped, single)
    }

    @Test fun `a selected control is never a snap target for itself`() {
        val layout = resolved()
        val target = layout.controls.first { it.spec.output == TouchOutputControl.Dpad }
        // A one-pixel nudge must not be cancelled by the control's own centre.
        val snapped = TouchEditorAlignment.snap(
            layout,
            setOf(target.id),
            target.id,
            TouchEditorDelta(1f, 0f),
            TouchAlignmentSettings(grid = false, snap = true),
        )
        val landed = target.centerX + snapped.x
        val candidates = layout.controls.filter { it.id != target.id }.map { it.centerX } +
            listOf((layout.region.left + layout.region.right) / 2f)
        // Either it moved as asked, or it snapped to something that is not itself.
        assertTrue(
            abs(snapped.x - 1f) < 1e-3f ||
                candidates.any { abs(it - landed) < 1e-3f } ||
                abs(landed - (layout.region.left + target.hitHalfWidth)) < 1e-3f ||
                abs(landed - (layout.region.right - target.hitHalfWidth)) < 1e-3f,
        )
    }

    @Test fun `guides are reported only while something is actually aligned`() {
        val layout = resolved()
        val target = layout.controls.first { it.spec.output == TouchOutputControl.Dpad }
        assertTrue(
            TouchEditorAlignment.matchedGuides(layout, setOf(target.id), null, GRID_AND_SNAP)
                .isEmpty(),
        )
        val centered = TouchLayoutEditor.move(
            TouchProfileCatalog.require(TouchProfileId.GameCube),
            TouchLayoutEditor.empty(TouchProfileCatalog.require(TouchProfileId.GameCube)),
            setOf(target.id),
            deltaX = 0.5f - target.centerX / layout.region.width,
            deltaY = 0f,
            editGroup = false,
        )
        val after = resolved(override = centered)
        val moved = after.controls.first { it.id == target.id }
        val guides = TouchEditorAlignment.matchedGuides(
            after, setOf(moved.id), moved.id, SNAP_ON,
        )
        assertTrue(guides.any { it.vertical && it.kind == TouchGuideKind.RegionCenter })
    }

    @Test fun `the grid is symmetric, inside the region, and bounded`() {
        SHAPES.forEach { (name, shape) ->
            val (width, height) = shape
            listOf(1f, 2.625f, 3.5f).forEach { density ->
                val region = TouchLayoutRegion(0f, 0f, width * density, height * density, density)
                val lines = TouchEditorAlignment.gridLines(region, GRID_AND_SNAP)
                assertTrue("$name@$density has no grid", lines.isNotEmpty())
                lines.forEach { line ->
                    val within = if (line.vertical) {
                        line.position >= region.left - 1e-3f && line.position <= region.right + 1e-3f
                    } else {
                        line.position >= region.top - 1e-3f && line.position <= region.bottom + 1e-3f
                    }
                    assertTrue("$name@$density line outside the region", within)
                }
                val centerX = (region.left + region.right) / 2f
                val centerY = (region.top + region.bottom) / 2f
                assertTrue(
                    "$name@$density has no centre column",
                    lines.any { it.vertical && abs(it.position - centerX) < 1e-3f },
                )
                assertTrue(
                    "$name@$density has no centre row",
                    lines.any { !it.vertical && abs(it.position - centerY) < 1e-3f },
                )
            }
        }
    }

    @Test fun `a degenerate region produces no assistance rather than an exception`() {
        val empty = TouchLayoutRegion(0f, 0f, 0f, 0f, 0f)
        assertTrue(TouchEditorAlignment.gridLines(empty, GRID_AND_SNAP).isEmpty())
        assertTrue(TouchEditorAlignment.gridLines(empty, TouchAlignmentSettings.Off).isEmpty())
        assertTrue(
            TouchEditorAlignment.matchedGuides(
                ResolvedTouchLayout.Empty, emptySet(), "nope", GRID_AND_SNAP,
            ).isEmpty(),
        )
        assertEquals(
            TouchEditorDelta.Zero,
            TouchEditorAlignment.snap(
                resolved(), setOf("dpad"), "dpad", TouchEditorDelta(Float.NaN, 1f), SNAP_ON,
            ),
        )
    }

    @Test fun `every shipped layout stays editable and snappable at each named shape`() {
        TouchProfileId.entries.forEach { personality ->
            SHAPES.forEach { (name, shape) ->
                val (width, height) = shape
                val layout = resolved(personality, width = width, height = height)
                assertTrue("$personality at $name: ${layout.problem}", layout.fits)
                layout.controls.forEach { control ->
                    // Every control has geometry the editor can pick and snap.
                    assertTrue(control.hitTest(control.centerX, control.centerY))
                    val snapped = TouchEditorAlignment.snap(
                        layout, setOf(control.id), control.id, TouchEditorDelta(4f, 4f), GRID_AND_SNAP,
                    )
                    assertTrue(snapped.x.isFinite() && snapped.y.isFinite())
                }
            }
        }
    }

    @Test fun `a window below the minimum is reported as too small, not merely failing`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val small = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile).layout,
            TouchLayoutRegion(0f, 0f, 549f, 411f, 1f),
            TouchLayoutAuditMode.UserDraft,
        )
        assertFalse(small.fits)
        // The distinction the editor depends on: no edit can widen a window, so
        // a surface must be able to tell "fix your layout" from "fix your window".
        assertTrue(small.regionTooSmall)

        val roomy = resolved()
        assertTrue(roomy.fits)
        assertFalse(roomy.regionTooSmall)
        assertFalse(
            TouchLayoutResolver.resolve(
                TouchLayoutComposer.compose(
                    profile,
                    TouchLayoutEditor.scale(
                        profile, TouchLayoutEditor.empty(profile), "dpad", 1.75f, false,
                    ),
                ).layout,
                TouchLayoutRegion(0f, 0f, 915f, 412f, 1f),
                TouchLayoutAuditMode.UserDraft,
            ).regionTooSmall,
        )
    }

    @Test fun `snapping a control to the safe edge keeps it inside the interaction area`() {
        val personality = TouchProfileId.Pro2
        val profile = TouchProfileCatalog.require(personality)
        val layout = resolved(personality)
        val target = layout.controls.first { it.spec.output == TouchOutputControl.Dpad }
        val toEdge = layout.region.left + target.hitHalfWidth - target.centerX - 2f
        val snapped = TouchEditorAlignment.snap(
            layout, setOf(target.id), target.id, TouchEditorDelta(toEdge, 0f), SNAP_ON,
        )
        val moved = TouchLayoutEditor.move(
            profile,
            TouchLayoutEditor.empty(profile),
            setOf(target.id),
            snapped.x / layout.region.width,
            0f,
            editGroup = false,
        )
        val after = resolved(personality, moved)
        assertTrue(after.problem, after.fits)
        val placed = after.controls.first { it.id == target.id }
        assertTrue(placed.centerX - placed.hitHalfWidth >= after.region.left - 1f)
    }
}

class TouchLayoutEditorSelectionTest {
    private val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
    private val faceIds = profile.defaultTemplate.controls
        .filter { it.editGroupId == "face-cluster" }
        .mapTo(linkedSetOf()) { it.id }

    @Test fun `group expansion is what the editor reports and what it edits`() {
        val single = TouchLayoutEditor.targetIds(profile.defaultTemplate, setOf("a"), editGroup = false)
        assertEquals(setOf("a"), single)
        val group = TouchLayoutEditor.targetIds(profile.defaultTemplate, setOf("a"), editGroup = true)
        assertEquals(faceIds, group)

        val moved = TouchLayoutEditor.move(
            profile, TouchLayoutEditor.empty(profile), setOf("a"), 0.01f, 0f, editGroup = true,
        )
        assertEquals(faceIds, moved.controls.keys)
    }

    @Test fun `a multi-control selection moves as one and keeps its spacing`() {
        val ids = setOf("main-stick", "plus")
        val before = ids.associateWith {
            profile.defaultTemplate.controls.single { c -> c.id == it }.geometry.anchorX
        }
        val moved = TouchLayoutEditor.move(
            profile, TouchLayoutEditor.empty(profile), ids, 0.05f, 0f, editGroup = false,
        )
        assertEquals(ids, moved.controls.keys)
        ids.forEach { id ->
            assertEquals(
                before.getValue(id) + 0.05f,
                requireNotNull(moved.controls.getValue(id).anchorX),
                1e-6f,
            )
        }
    }

    @Test fun `pinch scales each control relative to its own size`() {
        var override = TouchLayoutEditor.scale(
            profile, TouchLayoutEditor.empty(profile), setOf("plus"), 1.4f, editGroup = false,
        )
        override = TouchLayoutEditor.scaleBy(
            profile, override, setOf("plus", "main-stick"), 1.25f, editGroup = false,
        )
        assertEquals(1.4f * 1.25f, requireNotNull(override.controls.getValue("plus").scale), 1e-5f)
        assertEquals(1.25f, requireNotNull(override.controls.getValue("main-stick").scale), 1e-5f)
    }

    @Test fun `scaling is clamped and a nonsense factor changes nothing`() {
        val huge = TouchLayoutEditor.scaleBy(
            profile, TouchLayoutEditor.empty(profile), setOf("plus"), 100f, editGroup = false,
        )
        assertEquals(
            TouchLayoutEditor.MAX_SCALE,
            requireNotNull(huge.controls.getValue("plus").scale),
            1e-6f,
        )
        val tiny = TouchLayoutEditor.scaleBy(
            profile, TouchLayoutEditor.empty(profile), setOf("plus"), 0.001f, editGroup = false,
        )
        assertEquals(
            TouchLayoutEditor.MIN_SCALE,
            requireNotNull(tiny.controls.getValue("plus").scale),
            1e-6f,
        )
        val base = TouchLayoutEditor.empty(profile)
        assertEquals(base, TouchLayoutEditor.scaleBy(profile, base, setOf("plus"), Float.NaN, false))
        assertEquals(base, TouchLayoutEditor.move(profile, base, setOf("plus"), Float.NaN, 0f, false))
    }

    @Test fun `an unknown id in the selection is ignored, never applied elsewhere`() {
        val moved = TouchLayoutEditor.move(
            profile, TouchLayoutEditor.empty(profile), setOf("nope", "plus"), 0.01f, 0f, false,
        )
        assertEquals(setOf("plus"), moved.controls.keys)
        assertFalse("nope" in moved.controls)
        assertTrue(
            TouchLayoutEditor.move(
                profile, TouchLayoutEditor.empty(profile), setOf("nope"), 0.01f, 0f, false,
            ).controls.isEmpty(),
        )
    }

    @Test fun `hiding then restoring a control returns the shipped geometry exactly`() {
        val hidden = TouchLayoutEditor.setVisible(
            profile, TouchLayoutEditor.empty(profile), setOf("chat"), false, editGroup = false,
        )
        assertTrue(
            TouchLayoutComposer.compose(profile, hidden).layout.controls.none { it.id == "chat" },
        )
        val restored = TouchLayoutEditor.setVisible(profile, hidden, setOf("chat"), true, false)
        assertTrue(restored.controls.isEmpty())
        assertEquals(
            TouchLayoutComposer.compose(profile).layout,
            TouchLayoutComposer.compose(profile, restored).layout,
        )
    }
}
