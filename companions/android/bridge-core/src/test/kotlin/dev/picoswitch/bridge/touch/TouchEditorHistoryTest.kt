package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Undo and redo over whole layout revisions.
 *
 * The claim under test is that EVERY editor operation is undoable, which is a
 * property of the representation rather than of a list of supported commands: a
 * revision is the whole scene, so undoing one restores every field of every
 * control without anything having had to remember what it changed.
 */
class TouchEditorHistoryTest {

    private val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
    private val region = TouchLayoutRegion(0f, 0f, 900f, 460f, 1f)
    private val base = authored(pro2)

    private fun resolve(document: TouchLayoutDocument) = TouchLayoutResolver.resolve(
        TouchLayoutComposer.compose(pro2, document).layout,
        region,
        TouchLayoutAuditMode.UserDraft,
    )

    /** One of every meaningful mutation, paired with a name. */
    private fun operations(): List<Pair<String, (TouchLayoutDocument) -> TouchLayoutDocument>> {
        val layout = resolve(base)
        val id = TouchLayoutV1.FACE_SOUTH
        return listOf(
            "AddControl" to { d: TouchLayoutDocument ->
                TouchLayoutEditor.add(d, pro2, TouchLayoutV1.GRIP_LEFT, 0.5f, 0.5f).document
            },
            "DeleteControl" to { d -> TouchLayoutEditor.delete(d, setOf(id), false).document },
            "Duplicate" to { d -> TouchLayoutEditor.duplicate(d, setOf(id), false).document },
            "MoveControl" to { d ->
                TouchLayoutEditor.move(d, layout, setOf(id), 20f, 10f, editGroup = false)
            },
            "TransformControl" to { d ->
                TouchLayoutEditor.scaleBy(d, layout, setOf(id), 1.3f, editGroup = false)
            },
            "Rotate" to { d ->
                TouchLayoutEditor.rotateBy(d, layout, setOf(id), 30f, editGroup = false)
            },
            "Group" to { d ->
                TouchLayoutEditor.group(d, setOf(id, TouchLayoutV1.MINUS)).document
            },
            "Ungroup" to { d -> TouchLayoutEditor.ungroup(d, setOf(id)).document },
            "ChangeProperty" to { d ->
                TouchLayoutEditor.setLatch(d, pro2, setOf(id), latch = false, editGroup = false)
            },
            "ChangeZOrder" to { d -> TouchLayoutEditor.bringToFront(d, setOf(id), false) },
            "ResetLayout" to { _ -> TouchLayoutEditor.resetAll(pro2) },
        )
    }

    @Test fun `every command applies, undoes and redoes`() {
        operations().forEach { (name, operation) ->
            // ResetLayout on an untouched document changes nothing, so start
            // each case from a document that has something to reset.
            val start = TouchLayoutEditor.setScale(base, setOf(TouchLayoutV1.DPAD), 1.4f, false)
            val history = TouchEditorHistory(start)
            val next = operation(start)
            history.push(next, name)
            assertEquals(name, next, history.current)
            assertTrue(name, history.canUndo)

            assertEquals(name, start, history.undo())
            assertEquals(name, start, history.current)
            assertFalse(name, history.canUndo)
            assertTrue(name, history.canRedo)

            assertEquals(name, next, history.redo())
            assertEquals(name, next, history.current)
        }
    }

    @Test fun `a no-op change is not a history entry`() {
        val history = TouchEditorHistory(base)
        history.push(base, "Move")
        assertFalse("a gesture that ended where it began", history.canUndo)
        assertNull(history.undo())
    }

    /**
     * Gesture coalescing lives at the CALL SITE, and this pins what that has to
     * produce: one revision for a whole drag, whatever it passed through.
     */
    @Test fun `a coalesced drag is one entry`() {
        val layout = resolve(base)
        val history = TouchEditorHistory(base)
        var working = base
        // Sixty frames of a drag, mutating the working document freely.
        repeat(60) {
            working = TouchLayoutEditor.move(
                working, layout, setOf(TouchLayoutV1.DPAD), 1f, 0f, editGroup = false,
            )
        }
        history.push(working, "Move")
        assertEquals(working, history.current)
        assertEquals(base, history.undo())
        assertFalse("one entry, not sixty", history.canUndo)
    }

    /**
     * The contract a live gesture relies on, and the one an editor gets wrong.
     *
     * A drag mutates the working document directly and leaves the history alone,
     * so when it ends the endpoint is simply pushed. The bug this pins: the
     * surface first called [TouchEditorHistory.reset] to "rebase" onto the
     * pre-drag document — which clears the undo stack — so every earlier step
     * vanished the moment anything was dragged.
     */
    @Test fun `pushing a gesture endpoint keeps everything before it undoable`() {
        val layout = resolve(base)
        val history = TouchEditorHistory(base)

        // Two ordinary edits...
        history.push(TouchLayoutEditor.delete(base, setOf(TouchLayoutV1.CHAT), false).document, "Delete")
        val afterTwo = TouchLayoutEditor.setScale(
            history.current, setOf(TouchLayoutV1.DPAD), 1.2f, editGroup = false,
        )
        history.push(afterTwo, "Resize")

        // ... then a drag, whose frames never touch the history.
        var working = history.current
        repeat(30) {
            working = TouchLayoutEditor.move(
                working, layout, setOf(TouchLayoutV1.PLUS), 1f, 0f, editGroup = false,
            )
        }
        history.push(working, "Move")

        // All three steps are still there, newest first.
        assertEquals(afterTwo, history.undo())
        assertNotNull(history.undo())
        assertEquals(base, history.undo())
        assertNull(history.undo())
    }

    @Test fun `a new edit invalidates the redo stack`() {
        val history = TouchEditorHistory(base)
        val first = TouchLayoutEditor.setScale(base, setOf(TouchLayoutV1.DPAD), 1.2f, false)
        history.push(first, "Resize")
        history.undo()
        assertTrue(history.canRedo)

        val other = TouchLayoutEditor.setScale(base, setOf(TouchLayoutV1.PLUS), 1.2f, false)
        history.push(other, "Resize")
        assertFalse("the abandoned branch is gone", history.canRedo)
        assertEquals(other, history.current)
    }

    @Test fun `undo after a delete restores the instance exactly`() {
        val history = TouchEditorHistory(base)
        val id = TouchLayoutV1.FACE_SOUTH
        val before = requireNotNull(base.instance(id))
        history.push(TouchLayoutEditor.delete(base, setOf(id), false).document, "Delete")
        assertNull(history.current.instance(id))

        history.undo()
        val after = requireNotNull(history.current.instance(id))
        // Transform, rotation, group membership, z-order and behaviour, all of
        // them, because the revision was the whole scene.
        assertEquals(before, after)
    }

    @Test fun `the history is bounded and forgets the oldest revisions first`() {
        val history = TouchEditorHistory(base, limit = 3)
        val steps = (1..5).map {
            TouchLayoutEditor.setScale(base, setOf(TouchLayoutV1.DPAD), 1f + it * 0.05f, false)
        }
        steps.forEach { history.push(it, "Resize") }
        var undone = 0
        while (history.undo() != null) undone++
        assertEquals("only the last three are recoverable", 3, undone)
    }

    @Test fun `adopting an external document clears both stacks`() {
        val history = TouchEditorHistory(base)
        history.push(TouchLayoutEditor.bringToFront(base, setOf(TouchLayoutV1.DPAD), false), "Z")
        history.undo()
        assertTrue(history.canRedo)

        val fresh = TouchLayoutEditor.resetAll(pro2)
        history.reset(fresh)
        assertEquals(fresh, history.current)
        assertFalse(history.canUndo)
        assertFalse(history.canRedo)
    }

    @Test fun `the last operation can name itself`() {
        val history = TouchEditorHistory(base)
        assertNull(history.undoLabel)
        history.push(TouchLayoutEditor.delete(base, setOf(TouchLayoutV1.CHAT), false).document, "Delete")
        assertEquals("Delete", history.undoLabel)
        assertNotNull(history.undo())
    }
}
