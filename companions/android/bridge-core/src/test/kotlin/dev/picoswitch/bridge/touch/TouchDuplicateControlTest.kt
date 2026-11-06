package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.DpadState
import dev.picoswitch.bridge.core.TouchContribution
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Two on-screen controls, one logical input.
 *
 * The failure this suite exists to prevent has one shape and many spellings:
 * something downstream keys state by BINDING rather than by INSTANCE, and the
 * moment a user makes a second A button, releasing either one releases both.
 * It is silent, it only appears in layouts the user built themselves, and it
 * looks exactly like a dropped input rather than like a bug in the editor.
 *
 * ```text
 * A1 down    A pressed
 * A2 down    A still pressed        <- two contributors
 * A1 up      A STILL pressed        <- the assertion that matters
 * A2 up      A released
 * ```
 *
 * Every layout here is built from scratch with only the controls under test, so
 * a future nudge to a shipped template cannot break a suite about routing.
 */
class TouchDuplicateControlTest {

    private val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
    private val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
    private val published = mutableListOf<TouchContribution>()

    private lateinit var engine: TouchControlEngine
    private lateinit var resolved: ResolvedTouchLayout

    private fun install(profile: TouchControllerProfile, document: TouchLayoutDocument) {
        published.clear()
        engine = TouchControlEngine(onContribution = { published += it })
        val composition = TouchLayoutComposer.compose(profile, document)
        assertEquals(null, composition.warning)
        resolved = TouchLayoutResolver.resolve(
            composition.layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
            // A partial layout is a legal DRAFT; the missing outputs are warnings.
            TouchLayoutAuditMode.UserDraft,
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        engine.setLayout(resolved)
        published.clear()
    }

    /** Build, install, and hand back the instance ids in placement order. */
    private fun scene(
        profile: TouchControllerProfile,
        vararg placements: Pair<String, Pair<Float, Float>>,
    ): List<String> {
        val (document, ids) = documentOf(profile, *placements)
        install(profile, document)
        return ids
    }

    private fun down(id: Long, control: String) {
        val target = requireNotNull(resolved.control(control)) { "$control is not in the layout" }
        engine.onContact(TouchContact(id, TouchPhase.Down, target.centerX, target.centerY, id))
    }

    private fun up(id: Long) = engine.onContact(TouchContact(id, TouchPhase.Up, 0f, 0f, id))

    private fun moveTo(id: Long, control: String, dx: Float, dy: Float) {
        val target = requireNotNull(resolved.control(control))
        engine.onContact(
            TouchContact(id, TouchPhase.Move, target.centerX + dx, target.centerY + dy, id),
        )
    }

    /** Drag a trigger along its own inward travel axis, which is how it is pulled. */
    private fun pull(id: Long, control: String, distance: Float) {
        val target = requireNotNull(resolved.control(control))
        val axis = TouchTriggerTravel.inwardAxis(
            target.centerX,
            target.centerY,
            resolved.region,
            engine.config.trigger.centerEpsilonUnits,
        )
        engine.onContact(
            TouchContact(
                id,
                TouchPhase.Move,
                target.centerX + axis.x * distance,
                target.centerY + axis.y * distance,
                id,
            ),
        )
    }

    private fun held() = engine.contribution

    // -------------------------------------------------------------- digital buttons

    @Test fun `a second instance keeps the binding pressed after the first lets go`() {
        val (first, second) = scene(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.30f to 0.50f),
            TouchLayoutV1.FACE_SOUTH to (0.70f to 0.50f),
        )
        assertNotEquals(first, second)

        down(11, first)
        assertEquals(setOf(ControllerButton.A), held().positionalButtons)

        down(22, second)
        assertEquals(
            "a second contributor is not a second press",
            setOf(ControllerButton.A),
            held().positionalButtons,
        )

        up(11)
        assertEquals(
            "the surviving instance still holds the binding",
            setOf(ControllerButton.A),
            held().positionalButtons,
        )

        up(22)
        assertEquals(emptySet<ControllerButton>(), held().positionalButtons)
    }

    /**
     * The same claim for an already-logical binding, because the two travel
     * through different accumulators and only one of them would be noticed.
     */
    @Test fun `duplicate logical buttons aggregate the same way`() {
        val (first, second) = scene(
            pro2,
            TouchLayoutV1.SHOULDER_LEFT to (0.30f to 0.50f),
            TouchLayoutV1.SHOULDER_LEFT to (0.70f to 0.50f),
        )

        down(1, first)
        down(2, second)
        assertTrue(ControllerButton.L1 in held().logicalButtons)
        up(1)
        assertTrue("still held by the duplicate", ControllerButton.L1 in held().logicalButtons)
        up(2)
        assertFalse(ControllerButton.L1 in held().logicalButtons)
    }

    /** Order must not matter: releasing the SECOND one first is the same story. */
    @Test fun `release order does not change the aggregate`() {
        val (first, second) = scene(
            pro2,
            TouchLayoutV1.FACE_EAST to (0.30f to 0.50f),
            TouchLayoutV1.FACE_EAST to (0.70f to 0.50f),
        )

        down(7, first)
        down(8, second)
        up(8)
        assertEquals(setOf(ControllerButton.B), held().positionalButtons)
        up(7)
        assertEquals(emptySet<ControllerButton>(), held().positionalButtons)
    }

    @Test fun `three instances of one binding all have to let go`() {
        val ids = scene(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.20f to 0.50f),
            TouchLayoutV1.FACE_SOUTH to (0.50f to 0.50f),
            TouchLayoutV1.FACE_SOUTH to (0.80f to 0.50f),
        )

        ids.forEachIndexed { index, id -> down(100L + index, id) }
        assertEquals(setOf(ControllerButton.A), held().positionalButtons)
        (0 until ids.lastIndex).forEach { index ->
            up(100L + index)
            assertEquals(
                "released ${index + 1} of ${ids.size}",
                setOf(ControllerButton.A),
                held().positionalButtons,
            )
        }
        up(100L + ids.lastIndex)
        assertEquals(emptySet<ControllerButton>(), held().positionalButtons)
    }

    // ----------------------------------------------------------------- lifecycle

    /**
     * Replacing the layout while a duplicate is held.
     *
     * The engine releases everything at a geometry boundary, so the question is
     * not whether the deleted instance lets go — it is whether anything is left
     * behind that could republish it. Nothing may be: the contribution has to be
     * neutral even though a finger is still physically down.
     */
    @Test fun `deleting one duplicate while both are held leaves nothing stale`() {
        val (document, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.30f to 0.50f),
            TouchLayoutV1.FACE_SOUTH to (0.70f to 0.50f),
        )
        install(pro2, document)
        val (first, second) = ids

        down(31, first)
        down(32, second)
        assertEquals(setOf(ControllerButton.A), held().positionalButtons)

        val reduced = TouchLayoutEditor.delete(document, setOf(second), editGroup = false).document
        engine.setLayout(
            TouchLayoutResolver.resolve(
                TouchLayoutComposer.compose(pro2, reduced).layout,
                resolved.region,
                TouchLayoutAuditMode.UserDraft,
            ),
        )
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(0, engine.diagnostics().ownedControls)

        // And a late event from either old contact claims nothing.
        up(31)
        up(32)
        assertEquals(TouchContribution.Neutral, engine.contribution)
    }

    @Test fun `a global release drops every instance of a duplicated binding`() {
        val (first, second) = scene(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.30f to 0.50f),
            TouchLayoutV1.FACE_SOUTH to (0.70f to 0.50f),
        )
        down(41, first)
        down(42, second)
        engine.releaseAll(TouchReleaseReason.EditorEntered)
        assertEquals(TouchContribution.Neutral, engine.contribution)
        assertEquals(0, engine.diagnostics().activeContacts)
    }

    // --------------------------------------------------------------------- sticks

    /**
     * Two sticks driving one axis cannot both speak.
     *
     * Averaging or last-event-wins would both be inventions; ownership is the
     * only rule that produces a direction the user is actually making. The
     * second instance stays silent while the first owns the axis, and takes over
     * only once that contact ends.
     */
    @Test fun `the first stick instance owns the axis until its contact ends`() {
        val (first, second) = scene(
            pro2,
            TouchLayoutV1.STICK_LEFT to (0.22f to 0.50f),
            TouchLayoutV1.STICK_LEFT to (0.78f to 0.50f),
        )

        down(51, first)
        moveTo(51, first, dx = 60f, dy = 0f)
        val owned = held().leftX
        assertTrue("the owning stick deflects", owned > TouchContribution.Neutral.leftX)

        // The second one is being held hard the other way and says nothing.
        down(52, second)
        moveTo(52, second, dx = -60f, dy = 0f)
        assertEquals("a second stick cannot contradict the first", owned, held().leftX)

        // Ownership transfers when the owner lifts, to the instance still held.
        up(51)
        moveTo(52, second, dx = -60f, dy = 0f)
        assertTrue("the survivor takes the axis", held().leftX < TouchContribution.Neutral.leftX)

        up(52)
        assertEquals(TouchContribution.Neutral.leftX, held().leftX)
    }

    @Test fun `duplicate d-pads follow the same ownership rule`() {
        val (first, second) = scene(
            pro2,
            TouchLayoutV1.DPAD to (0.22f to 0.50f),
            TouchLayoutV1.DPAD to (0.78f to 0.50f),
        )

        down(61, first)
        moveTo(61, first, dx = 0f, dy = -60f)
        assertTrue("the owner points up", held().dpad.up)

        down(62, second)
        moveTo(62, second, dx = 0f, dy = 60f)
        assertTrue("a second D-pad cannot contradict the first", held().dpad.up)
        assertFalse(held().dpad.down)

        up(61)
        moveTo(62, second, dx = 0f, dy = 60f)
        assertTrue("the survivor takes the hat", held().dpad.down)
        assertFalse(held().dpad.up)
        up(62)
        assertEquals(DpadState.None, held().dpad)
    }

    // ------------------------------------------------------------ analog triggers

    /**
     * Two analog triggers on one side aggregate by MAX.
     *
     * Deterministic and defensible: the console is told the deepest pull anyone
     * is making, so two widgets at different depths read as the deeper rather
     * than as whichever moved last. Summing them would let two light touches
     * assert a click nobody made.
     */
    @Test fun `duplicate analog triggers publish the deepest pull`() {
        val (first, second) = scene(
            gameCube,
            "trigger-l" to (0.16f to 0.12f),
            "trigger-l" to (0.84f to 0.12f),
        )

        down(71, first)
        down(72, second)
        assertEquals("neither has travelled yet", 0, held().leftTrigger)

        pull(71, first, 30f)
        val shallow = held().leftTrigger
        assertTrue("the first one is pulled: $shallow", shallow > 0)

        pull(72, second, 90f)
        val deep = held().leftTrigger
        assertTrue("the deeper pull wins: $shallow then $deep", deep > shallow)

        // Releasing the DEEPER one falls back to the shallower, not to zero.
        up(72)
        assertEquals("the shallow pull survives unchanged", shallow, held().leftTrigger)
        up(71)
        assertEquals(0, held().leftTrigger)
    }

    // ----------------------------------------------------------------- z ordering

    /**
     * Overlapping duplicates resolve by the order they are DRAWN in.
     *
     * The layout audit permits two instances of one output to overlap, because
     * whichever answers produces the same thing. The router still has to pick
     * deterministically, and the only answer that is not a surprise is the one
     * the user can see on top.
     */
    @Test fun `the front instance claims a contact where two overlap`() {
        val (document, ids) = documentOf(
            pro2,
            TouchLayoutV1.FACE_SOUTH to (0.50f to 0.50f),
            TouchLayoutV1.FACE_SOUTH to (0.50f to 0.50f),
        )
        val (first, second) = ids
        install(pro2, document)

        down(81, first)
        assertEquals("the later-drawn instance is in front", second, engine.ownerOf(81))
        assertEquals(setOf(ControllerButton.A), held().positionalButtons)

        // Send it to the back and the original wins instead.
        install(pro2, TouchLayoutEditor.sendToBack(document, setOf(second), editGroup = false))
        down(82, first)
        assertEquals(first, engine.ownerOf(82))
    }
}
