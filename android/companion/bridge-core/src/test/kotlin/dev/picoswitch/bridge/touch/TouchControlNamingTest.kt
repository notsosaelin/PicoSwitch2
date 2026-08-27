package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What a control is CALLED, wherever a person reads it.
 *
 * The rule under test is that one function answers this for the whole surface.
 * Pro Controller 2's face controls carry no authored legend — their letter comes
 * from the binding at draw time — so anything that names them from their ids
 * produces "face-north" and "face-east", which are internal cardinal slots and
 * not buttons anyone has pressed. The editor's labels and the audit's sentences
 * both resolve through [TouchControlNaming], so the control called "B" in the
 * picker is called "B" in the warning about it.
 */
class TouchControlNamingTest {

    private val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
    private val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
    private val region = TouchLayoutRegion(0f, 0f, 900f, 460f, 1f)

    private fun resolve(profile: TouchControllerProfile, document: TouchLayoutDocument) =
        TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile, document).layout,
            region,
            TouchLayoutAuditMode.UserDraft,
        )

    private fun nameOf(profile: TouchControllerProfile, document: TouchLayoutDocument, id: String) =
        requireNotNull(resolve(profile, document).control(id)).spec.displayName

    // ------------------------------------------------------------------ the letters

    @Test fun `Pro Controller 2 face controls are named by their letter`() {
        val document = authored(pro2)
        assertEquals("A", nameOf(pro2, document, TouchLayoutV1.FACE_EAST))
        assertEquals("B", nameOf(pro2, document, TouchLayoutV1.FACE_SOUTH))
        assertEquals("X", nameOf(pro2, document, TouchLayoutV1.FACE_NORTH))
        assertEquals("Y", nameOf(pro2, document, TouchLayoutV1.FACE_WEST))
    }

    /**
     * The stronger claim, and the one that keeps the fix from decaying: no name
     * anywhere in a shipped layout leaks a cardinal slot.
     */
    @Test fun `no shipped control is ever named by its cardinal slot`() {
        TouchProfileCatalog.profiles.forEach { (profileId, profile) ->
            resolve(profile, authored(profile)).controls.forEach { control ->
                val name = control.spec.displayName
                assertFalse(
                    "$profileId/${control.id} is called '$name'",
                    name.contains("face", ignoreCase = true) ||
                        name.contains("north", ignoreCase = true) ||
                        name.contains("south", ignoreCase = true) ||
                        name.contains("east", ignoreCase = true) ||
                        name.contains("west", ignoreCase = true),
                )
                assertTrue("$profileId/${control.id} has no name at all", name.isNotBlank())
            }
        }
    }

    @Test fun `an authored legend is used as it is drawn`() {
        val document = authored(gameCube)
        // GameCube's shoulder row, whose legends ARE authored -- and whose ids
        // ("trigger-l") are nothing a user would recognise.
        assertEquals("Z", nameOf(gameCube, document, "z"))
        assertEquals("ZL", nameOf(gameCube, document, "zl"))
        assertEquals("L", nameOf(gameCube, document, "trigger-l"))
        assertEquals("R", nameOf(gameCube, document, "trigger-r"))
    }

    // --------------------------------------------------------------------- copies

    /** Two copies are the same control to a user; a message still has to say which. */
    @Test fun `a duplicate is the same name with a copy number`() {
        val (document, id) = withDuplicate(pro2, TouchLayoutV1.FACE_SOUTH, 0.4f, 0.6f)
        assertEquals("face-south#2", id)
        assertEquals("B", nameOf(pro2, document, TouchLayoutV1.FACE_SOUTH))
        assertEquals("B (2)", nameOf(pro2, document, id))
    }

    // ---------------------------------------------------------------- the messages

    /**
     * The audit is a user-facing diagnostic: its sentence reaches the editor's
     * menu, the recovery notice and the unusable-window notice. It must not be
     * the one place the internal names come back.
     */
    @Test fun `an audit message names a face control by its letter`() {
        val (document, ids) = documentOf(pro2, TouchLayoutV1.FACE_NORTH to (0.0f to 0.5f))
        val resolved = resolve(pro2, document)
        val problem = requireNotNull(resolved.problemFor(ids.single()))
        assertTrue(problem, problem.startsWith("X "))
        assertFalse(problem, problem.contains("face"))
    }

    @Test fun `an overlap message names both controls the way they are drawn`() {
        val (document, _) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.50f to 0.50f),
            TouchLayoutV1.FACE_EAST to (0.505f to 0.50f),
        )
        val message = resolve(pro2, document).findings.single { it.blocking }.message
        assertTrue(message, message.contains("B") && message.contains("A"))
        assertFalse(message, message.contains("face"))
    }

    /**
     * A finding still points at instances by id. The sentence is presentation;
     * the ids are what an editor paints red, and two copies of one control have
     * the same name but never the same id.
     */
    @Test fun `findings identify instances by id, not by name`() {
        val (base, _) = documentOf(pro2, TouchLayoutV1.FACE_SOUTH to (0.5f to 0.5f))
        val added = TouchLayoutEditor.add(base, pro2, TouchLayoutV1.FACE_SOUTH, 0.5f, 0.5f)
        val document = pinned(added.document, added.created.single(), 0.5f, 0.5f)
        // Two instances of the same output may overlap freely, so force a real
        // collision with a different control instead.
        val (other, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.5f to 0.5f),
            TouchLayoutV1.FACE_SOUTH to (0.7f to 0.5f),
            TouchLayoutV1.MINUS to (0.705f to 0.5f),
        )
        assertEquals(2, document.controls.size)
        val finding = resolve(pro2, other).findings.single { it.blocking }
        assertEquals(setOf(ids[1], ids[2]), finding.controlIds)
        assertTrue(finding.message, finding.message.contains("B (2)"))
    }
}
