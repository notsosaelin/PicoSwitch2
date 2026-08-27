package dev.picoswitch.bridge.touch

import kotlin.math.abs
import kotlin.math.hypot
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Arbitrary groups, transformed as one object.
 *
 * The requirement that shapes all of this is that a group behaves like something
 * physical: dragging it keeps its spacing, scaling it spreads it as well as
 * enlarging it, and turning it turns the whole composition rather than spinning
 * each member in place. Anything less and a "group" is just a selection with a
 * name.
 *
 * Group membership is deliberately NOT geometric — grouping and ungrouping only
 * write a [TouchControlInstance.groupId] — which is what makes ungroup exactly
 * lossless on any window shape.
 */
class TouchGroupTransformTest {

    private val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
    private val region = TouchLayoutRegion(0f, 0f, 900f, 460f, 1f)

    private fun resolve(document: TouchLayoutDocument) = TouchLayoutResolver.resolve(
        TouchLayoutComposer.compose(pro2, document).layout,
        region,
        TouchLayoutAuditMode.UserDraft,
    )

    /** A mixed cluster: a button, a stick and a D-pad, grouped by hand. */
    private fun mixedScene(): Triple<TouchLayoutDocument, List<String>, ResolvedTouchLayout> {
        val (base, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.30f to 0.35f),
            TouchLayoutV1.STICK_LEFT to (0.50f to 0.55f),
            TouchLayoutV1.DPAD to (0.72f to 0.35f),
        )
        val grouped = TouchLayoutEditor.group(base, ids.toSet()).document
        return Triple(grouped, ids, resolve(grouped))
    }

    private fun centres(layout: ResolvedTouchLayout, ids: List<String>) =
        ids.associateWith { requireNotNull(layout.control(it)) }

    // -------------------------------------------------------------------- grouping

    @Test fun `any controls at all can be grouped, whatever kind they are`() {
        val (grouped, ids, _) = mixedScene()
        val group = requireNotNull(grouped.instance(ids.first())).groupId
        assertTrue(ids.all { grouped.instance(it)?.groupId == group })
        assertEquals(ids.toSet(), grouped.groupMembers(ids.first()))
    }

    @Test fun `ungroup is visually lossless`() {
        val (grouped, ids, before) = mixedScene()
        val ungrouped = TouchLayoutEditor.ungroup(grouped, ids.toSet()).document
        val after = resolve(ungrouped)
        ids.forEach { id ->
            val a = requireNotNull(before.control(id))
            val b = requireNotNull(after.control(id))
            assertEquals(a.centerX, b.centerX, 1e-4f)
            assertEquals(a.centerY, b.centerY, 1e-4f)
            assertEquals(a.halfWidth, b.halfWidth, 1e-4f)
        }
        assertTrue(ids.all { ungrouped.instance(it)?.groupId == null })
    }

    // ------------------------------------------------------------------ group move

    @Test fun `moving a group translates every member by the same amount`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        val moved = TouchLayoutEditor.move(
            grouped, layout, setOf(ids.first()), 40f, -25f, editGroup = true,
        )
        val after = centres(resolve(moved), ids)
        ids.forEach { id ->
            assertEquals(before.getValue(id).centerX + 40f, after.getValue(id).centerX, 1e-3f)
            assertEquals(before.getValue(id).centerY - 25f, after.getValue(id).centerY, 1e-3f)
        }
    }

    // ----------------------------------------------------------------- group scale

    /**
     * Scaling a cluster moves its members apart as well as enlarging them.
     *
     * A group that only grew its members would overlap itself; one that only
     * spread them would leave gaps. Both, about the centroid, is what makes it
     * read as one object being resized.
     */
    @Test fun `scaling a group spreads it about its centroid`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        val centroidX = before.values.map { it.centerX }.average().toFloat()
        val centroidY = before.values.map { it.centerY }.average().toFloat()

        val factor = 1.2f
        val scaled = TouchLayoutEditor.scaleBy(
            grouped, layout, setOf(ids.first()), factor, editGroup = true,
        )
        val after = centres(resolve(scaled), ids)

        ids.forEach { id ->
            // Each member grew...
            assertEquals(
                before.getValue(id).halfWidth * factor,
                after.getValue(id).halfWidth,
                1e-2f,
            )
            // ... and moved away from the centre by the same factor.
            assertEquals(
                centroidX + (before.getValue(id).centerX - centroidX) * factor,
                after.getValue(id).centerX,
                0.5f,
            )
            assertEquals(
                centroidY + (before.getValue(id).centerY - centroidY) * factor,
                after.getValue(id).centerY,
                0.5f,
            )
        }
    }

    /**
     * A member that hits the size limit must not tear the cluster apart.
     *
     * The displacement follows the factor that was ACTUALLY applied, so a group
     * already at maximum size simply stops growing instead of continuing to
     * spread while its members stay put.
     */
    @Test fun `a clamped member stops spreading with the rest`() {
        val (base, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.35f to 0.45f),
            TouchLayoutV1.FACE_EAST to (0.65f to 0.45f),
        )
        var document = TouchLayoutEditor.group(base, ids.toSet()).document
        document = TouchLayoutEditor.setScale(
            document, setOf(ids.first()), TouchLayoutEditor.MAX_SCALE, editGroup = false,
        )
        val layout = resolve(document)
        val before = centres(layout, ids)
        val scaled = TouchLayoutEditor.scaleBy(
            document, layout, setOf(ids.first()), 1.5f, editGroup = true,
        )
        val after = centres(resolve(scaled), ids)
        // The pinned member neither grew nor moved; it was already at the limit.
        assertEquals(
            before.getValue(ids[0]).halfWidth,
            after.getValue(ids[0]).halfWidth,
            1e-3f,
        )
        assertEquals(before.getValue(ids[0]).centerX, after.getValue(ids[0]).centerX, 1e-3f)
        // The other one did both.
        assertTrue(after.getValue(ids[1]).halfWidth > before.getValue(ids[1]).halfWidth)
    }

    // -------------------------------------------------------------- group rotation

    /**
     * Turning a group turns the composition, not just its members.
     *
     * Each member's own orientation gains the angle AND its position rotates
     * about the shared centre, so the cluster arrives where a physical object
     * turned by the same amount would.
     */
    @Test fun `rotating a group turns positions and members together`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        val centroidX = before.values.map { it.centerX }.average().toFloat()
        val centroidY = before.values.map { it.centerY }.average().toFloat()

        val turned = TouchLayoutEditor.rotateBy(
            grouped, layout, setOf(ids.first()), 90f, editGroup = true,
        )
        val after = centres(resolve(turned), ids)

        ids.forEach { id ->
            val dx = before.getValue(id).centerX - centroidX
            val dy = before.getValue(id).centerY - centroidY
            // Clockwise by 90 degrees in screen coordinates: (x, y) -> (-y, x).
            assertEquals(centroidX - dy, after.getValue(id).centerX, 1f)
            assertEquals(centroidY + dx, after.getValue(id).centerY, 1f)
            assertEquals(
                90f,
                requireNotNull(turned.instance(id)).rotationDegrees,
                1e-3f,
            )
        }
    }

    @Test fun `a rotated group keeps its internal distances`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        val turned = TouchLayoutEditor.rotateBy(
            grouped, layout, setOf(ids.first()), 37f, editGroup = true,
        )
        val after = centres(resolve(turned), ids)
        ids.forEach { a ->
            ids.forEach { b ->
                if (a == b) return@forEach
                val d0 = hypot(
                    before.getValue(a).centerX - before.getValue(b).centerX,
                    before.getValue(a).centerY - before.getValue(b).centerY,
                )
                val d1 = hypot(
                    after.getValue(a).centerX - after.getValue(b).centerX,
                    after.getValue(a).centerY - after.getValue(b).centerY,
                )
                assertEquals("$a to $b", d0, d1, 1f)
            }
        }
    }

    /**
     * Rotation is presentation. A grouped stick still reports the direction the
     * thumb is pushing, and a grouped D-pad still calls up "up".
     */
    @Test fun `a rotated group changes nothing about what its members send`() {
        val (grouped, ids, layout) = mixedScene()
        val turned = TouchLayoutEditor.rotateBy(
            grouped, layout, setOf(ids.first()), 45f, editGroup = true,
        )
        val before = TouchLayoutComposer.compose(pro2, grouped).layout
        val after = TouchLayoutComposer.compose(pro2, turned).layout
        ids.forEach { id ->
            val a = before.controls.single { it.id == id }
            val b = after.controls.single { it.id == id }
            assertEquals(a.action, b.action)
            assertEquals(a.output, b.output)
            assertEquals(a.kind, b.kind)
            assertNotEquals(a.visualRotationDegrees, b.visualRotationDegrees)
        }
    }

    // ----------------------------------------------------------- repeated transforms

    /**
     * Group geometry must not drift as transforms accumulate.
     *
     * Every operation composes into the same stored offset in logical units
     * rather than re-transforming an already-rounded screen position, so a
     * scale-then-inverse-scale round trip returns where it started.
     */
    @Test fun `scaling out and back returns the cluster to where it was`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        var document = TouchLayoutEditor.scaleBy(
            grouped, layout, setOf(ids.first()), 1.25f, editGroup = true,
        )
        document = TouchLayoutEditor.scaleBy(
            document, resolve(document), setOf(ids.first()), 1f / 1.25f, editGroup = true,
        )
        val after = centres(resolve(document), ids)
        ids.forEach { id ->
            assertEquals(before.getValue(id).centerX, after.getValue(id).centerX, 0.5f)
            assertEquals(before.getValue(id).centerY, after.getValue(id).centerY, 0.5f)
            assertEquals(before.getValue(id).halfWidth, after.getValue(id).halfWidth, 0.05f)
        }
    }

    @Test fun `four quarter turns return the cluster to where it was`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        var document = grouped
        repeat(4) {
            document = TouchLayoutEditor.rotateBy(
                document, resolve(document), setOf(ids.first()), 90f, editGroup = true,
            )
        }
        val after = centres(resolve(document), ids)
        ids.forEach { id ->
            assertEquals(before.getValue(id).centerX, after.getValue(id).centerX, 1f)
            assertEquals(before.getValue(id).centerY, after.getValue(id).centerY, 1f)
            assertEquals(0f, requireNotNull(document.instance(id)).rotationDegrees, 1e-3f)
        }
    }

    // -------------------------------------------------------------------- selection

    @Test fun `editing one member of a group is still possible`() {
        val (grouped, ids, layout) = mixedScene()
        val before = centres(layout, ids)
        val moved = TouchLayoutEditor.move(
            grouped, layout, setOf(ids.first()), 30f, 0f, editGroup = false,
        )
        val after = centres(resolve(moved), ids)
        assertEquals(before.getValue(ids[0]).centerX + 30f, after.getValue(ids[0]).centerX, 1e-3f)
        assertEquals(before.getValue(ids[1]).centerX, after.getValue(ids[1]).centerX, 1e-3f)
    }

    /** No nested groups: joining a second group leaves the first. */
    @Test fun `a control cannot be in two groups at once`() {
        val (base, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.25f to 0.35f),
            TouchLayoutV1.FACE_EAST to (0.45f to 0.35f),
            TouchLayoutV1.FACE_WEST to (0.65f to 0.35f),
        )
        val first = TouchLayoutEditor.group(base, setOf(ids[0], ids[1])).document
        val second = TouchLayoutEditor.group(first, setOf(ids[1], ids[2])).document
        assertEquals(setOf(ids[1], ids[2]), second.groupMembers(ids[1]))
        assertEquals(setOf(ids[0]), second.groupMembers(ids[0]))
        assertTrue(second.groups.values.all { members -> members.size <= 2 })
        // Every group id maps to a flat member list; there is no group of groups
        // for a nested structure to hide in.
        assertTrue(second.groups.keys.none { abs(it.length) == 0 })
    }

    /**
     * What a surface must count when it reports a delete.
     *
     * [TouchLayoutEditor.expand] is the answer to "what will this operation
     * actually touch", and delete has to remove exactly that set. The editor
     * used to count the TAPPED ids instead, so deleting a grouped cluster of
     * four announced "A deleted" — a destructive action under-reporting itself
     * by three controls. Undo was unaffected, because a revision is the whole
     * scene, but the user had no way to know what had just gone.
     */
    @Test fun `deleting a grouped control removes exactly the expanded set`() {
        val (base, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.25f to 0.35f),
            TouchLayoutV1.FACE_EAST to (0.45f to 0.35f),
            TouchLayoutV1.FACE_WEST to (0.65f to 0.35f),
            TouchLayoutV1.DPAD to (0.85f to 0.35f),
        )
        val grouped = TouchLayoutEditor.group(base, setOf(ids[0], ids[1], ids[2])).document
        val tapped = setOf(ids[0])

        val expanded = TouchLayoutEditor.expand(grouped, tapped, editGroup = true)
        assertEquals(setOf(ids[0], ids[1], ids[2]), expanded)
        val after = TouchLayoutEditor.delete(grouped, tapped, editGroup = true).document
        assertEquals(
            "delete removes the expanded set and nothing else",
            expanded,
            grouped.controls.map { it.instanceId }.toSet() -
                after.controls.map { it.instanceId }.toSet(),
        )

        // And with whole-group editing off, one tap is still one control.
        val alone = TouchLayoutEditor.expand(grouped, tapped, editGroup = false)
        assertEquals(tapped, alone)
        assertEquals(3, TouchLayoutEditor.delete(grouped, tapped, false).document.controls.size)
    }
}
