package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which controls the editor should paint as broken, and why that answer comes
 * from the layout itself.
 *
 * The rule the surface depends on: [ResolvedTouchLayout.invalidControlIds] is
 * derived from the SAME findings that decide [ResolvedTouchLayout.fits]. An
 * editor that recomputed its own idea of validity could show a control in red
 * while the layout played, or play a layout with nothing marked — and the user
 * would have no way to tell which of the two was lying.
 */
class TouchInvalidControlTest {

    private val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
    private val region = TouchLayoutRegion(0f, 0f, 900f, 460f, 1f)

    private fun resolve(document: TouchLayoutDocument) = TouchLayoutResolver.resolve(
        TouchLayoutComposer.compose(pro2, document).layout,
        region,
        TouchLayoutAuditMode.UserDraft,
    )

    /** A layout with one control, so a case is about that control and nothing else. */
    private fun lone(catalogId: String, x: Float, y: Float): Pair<TouchLayoutDocument, String> {
        val (document, ids) = documentOf(pro2, catalogId to (x to y))
        return document to ids.single()
    }

    // -------------------------------------------------------------- the invariant

    @Test fun `a healthy layout marks nothing`() {
        val resolved = resolve(authored(pro2))
        assertTrue(resolved.problem ?: "", resolved.fits)
        assertEquals(emptySet<String>(), resolved.invalidControlIds)
    }

    @Test fun `what is marked and what is refused are the same answer`() {
        // Swept across the region, every position agrees with itself.
        var offScreen = 0
        var onScreen = 0
        listOf(0.0f, 0.02f, 0.06f, 0.2f, 0.5f, 0.8f, 0.94f, 0.98f, 1.0f).forEach { x ->
            val (document, id) = lone(TouchLayoutV1.STICK_LEFT, x, 0.5f)
            val resolved = resolve(document)
            if (resolved.fits) {
                onScreen++
                assertEquals("$x should mark nothing", emptySet<String>(), resolved.invalidControlIds)
                assertNull(resolved.problemFor(id))
            } else {
                offScreen++
                assertEquals("$x should mark itself", setOf(id), resolved.invalidControlIds)
                assertNotNull(resolved.problemFor(id))
            }
        }
        assertTrue("the sweep must cover both outcomes", offScreen > 0 && onScreen > 0)
    }

    // ------------------------------------------------------------ dynamic changes

    /**
     * Dragging a control out of the region and back must flip the mark both
     * ways, on the geometry of every intermediate frame.
     */
    @Test fun `moving a control out and back toggles its error state`() {
        val (document, id) = lone(TouchLayoutV1.DPAD, 0.5f, 0.5f)
        assertTrue(resolve(document).fits)

        val pushedOut = TouchLayoutEditor.place(document, setOf(id), 0.0f, 0.5f)
        val broken = resolve(pushedOut)
        assertFalse(broken.fits)
        assertEquals(setOf(id), broken.invalidControlIds)

        val broughtBack = TouchLayoutEditor.place(pushedOut, setOf(id), 0.5f, 0.5f)
        val healed = resolve(broughtBack)
        assertTrue(healed.problem ?: "", healed.fits)
        assertEquals(emptySet<String>(), healed.invalidControlIds)
    }

    @Test fun `scaling a control past the edge marks it, and shrinking clears it`() {
        val (document, id) = lone(TouchLayoutV1.STICK_LEFT, 0.10f, 0.5f)
        assertTrue(resolve(document).fits)

        val grown = TouchLayoutEditor.setScale(
            document, setOf(id), TouchLayoutEditor.MAX_SCALE, editGroup = false,
        )
        assertEquals(setOf(id), resolve(grown).invalidControlIds)

        val shrunk = TouchLayoutEditor.setScale(grown, setOf(id), 1f, editGroup = false)
        assertEquals(emptySet<String>(), resolve(shrunk).invalidControlIds)
    }

    /**
     * Rotation is geometry, so a turn that pushes a corner out is an error like
     * any other — and reversing it clears the error.
     */
    @Test fun `rotating a control past the edge marks it`() {
        val (document, id) = lone(TouchLayoutV1.SHOULDER_LEFT, 0.5f, 0.075f)
        assertTrue(resolve(document).problem ?: "", resolve(document).fits)

        val turned = TouchLayoutEditor.setRotation(document, setOf(id), 90f)
        val broken = resolve(turned)
        assertFalse(broken.fits)
        assertEquals(setOf(id), broken.invalidControlIds)
        assertTrue(requireNotNull(broken.problemFor(id)).contains("outside"))

        val restored = TouchLayoutEditor.resetRotation(turned, setOf(id), editGroup = false)
        assertEquals(emptySet<String>(), resolve(restored).invalidControlIds)
    }

    @Test fun `a group transform that pushes members out marks each of them`() {
        // A short region and a wide row: turning the cluster a quarter turn
        // stands it on end, and the two outer members leave through the top and
        // bottom while the one at the centre of rotation stays put.
        val short = TouchLayoutRegion(0f, 0f, 900f, 340f, 1f)
        val (base, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.20f to 0.5f),
            TouchLayoutV1.FACE_EAST to (0.50f to 0.5f),
            TouchLayoutV1.FACE_WEST to (0.80f to 0.5f),
        )
        val grouped = TouchLayoutEditor.group(base, ids.toSet()).document
        fun placed(document: TouchLayoutDocument) = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(pro2, document).layout,
            short,
            TouchLayoutAuditMode.UserDraft,
        )
        assertTrue(placed(grouped).problem ?: "", placed(grouped).fits)

        val turned = TouchLayoutEditor.rotateBy(
            grouped, placed(grouped), setOf(ids[1]), 90f, editGroup = true,
        )
        val broken = placed(turned)
        assertFalse(broken.fits)
        assertEquals(
            "both outer members, each on its own",
            setOf(ids[0], ids[2]),
            broken.invalidControlIds,
        )
        assertNull("the pivot stayed inside", broken.problemFor(ids[1]))
    }

    // ------------------------------------------------------------------ multiples

    @Test fun `several invalid controls each carry their own error`() {
        val (document, ids) = documentOf(
            pro2,
            TouchLayoutV1.STICK_LEFT to (0.0f to 0.5f),
            TouchLayoutV1.STICK_RIGHT to (1.0f to 0.5f),
            TouchLayoutV1.DPAD to (0.5f to 0.5f),
        )
        val resolved = resolve(document)
        assertFalse(resolved.fits)
        assertEquals(setOf(ids[0], ids[1]), resolved.invalidControlIds)
        assertNotNull(resolved.problemFor(ids[0]))
        assertNotNull(resolved.problemFor(ids[1]))
        assertNull("the one that fits is not marked", resolved.problemFor(ids[2]))
    }

    @Test fun `an overlap marks both controls, not just one`() {
        val (document, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.50f to 0.50f),
            TouchLayoutV1.MINUS to (0.505f to 0.50f),
        )
        val resolved = resolve(document)
        assertFalse(resolved.fits)
        assertEquals(ids.toSet(), resolved.invalidControlIds)
    }

    /** Margins meeting is information, not an error, and must not paint anything. */
    @Test fun `a non-blocking finding never marks a control`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val resolved = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(gameCube, authored(gameCube)).layout,
            TouchLayoutRegion(0f, 0f, 800f, 400f, 1f),
            TouchLayoutAuditMode.ShippedTemplate,
        )
        val margins = resolved.findings.filter { it.message.contains("margin") }
        assertTrue("the shipped GameCube layout has one", margins.isNotEmpty())
        margins.forEach { assertFalse(it.blocking) }
        assertTrue(
            "and it paints nothing",
            margins.flatMap { it.controlIds }.none { it in resolved.invalidControlIds },
        )
    }

    // ------------------------------------------------------- geometry changes

    /**
     * A layout that fits one window may not fit the next. The mark follows the
     * geometry rather than anything remembered from before the change.
     */
    @Test fun `a window change re-decides which controls are marked`() {
        // 8% across: clear of the edge in a wide window, where controls stop
        // growing at MAX_SCALE, and past it in a narrow one, where they do not.
        val (document, id) = lone(TouchLayoutV1.STICK_LEFT, 0.08f, 0.5f)
        val roomy = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(pro2, document).layout,
            TouchLayoutRegion(0f, 0f, 1400f, 700f, 1f),
            TouchLayoutAuditMode.UserDraft,
        )
        assertEquals(emptySet<String>(), roomy.invalidControlIds)

        // The same normalized position in a much narrower window puts the same
        // control's edge outside, because sizes stop shrinking with the window.
        val narrow = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(pro2, document).layout,
            TouchLayoutRegion(0f, 0f, 640f, 330f, 1f),
            TouchLayoutAuditMode.UserDraft,
        )
        assertEquals(setOf(id), narrow.invalidControlIds)
    }
}
