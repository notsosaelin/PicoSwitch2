package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** What every personality template declared before any of them was corrected. */
private const val PERSONALITY_BASELINE_REVISION = 2

/**
 * The sideways Joy-Con action clusters, pinned to the shell they represent.
 *
 * A single Joy-Con used sideways is a whole controller turned a quarter turn,
 * and the bug this suite exists to prevent is subtle precisely because nothing
 * crashes: the control called `direction-up` gets drawn at the top of the screen
 * because the name says "up", the console reads the raw direction bit and
 * applies its own sideways interpretation, and the player is left pressing the
 * button in the X position to get Y. Everything is individually defensible and
 * the layout as a whole is a lie.
 *
 * So three things are asserted SEPARATELY, because conflating any two of them is
 * how the defect got in:
 *
 * ```text
 * physical identity   which button on the shell   the control's output
 * logical action      what it sends               the profile's binding
 * screen position     where it is drawn           the resolved geometry
 * ```
 *
 * The rotation directions are not a guess. The firmware's own
 * `joycon2_pack_sideways_stick` states them for the stick axes — "L is held 90
 * degrees counter-clockwise ... R is held clockwise" — and the layout has to
 * agree with the encoder about which way a shell is turned.
 */
class TouchSidewaysJoyConTest {

    // -------------------------------------------------------------- the rotation

    @Test fun `a quarter turn moves every slot exactly one place around`() {
        TouchCardinalSlot.entries.forEach { slot ->
            assertEquals(slot, TouchClusterRotation.Upright.screenSlot(slot))
            // Opposite turns undo each other, and neither is the identity.
            assertNotEquals(slot, TouchClusterRotation.QuarterClockwise.screenSlot(slot))
            assertEquals(
                slot,
                TouchClusterRotation.QuarterCounterClockwise.screenSlot(
                    TouchClusterRotation.QuarterClockwise.screenSlot(slot),
                ),
            )
        }
        // Turn a clock face anticlockwise and 12 lands where 9 was.
        assertEquals(
            TouchCardinalSlot.West,
            TouchClusterRotation.QuarterCounterClockwise.screenSlot(TouchCardinalSlot.North),
        )
        assertEquals(
            TouchCardinalSlot.East,
            TouchClusterRotation.QuarterClockwise.screenSlot(TouchCardinalSlot.North),
        )
    }

    @Test fun `a rotated diamond is the same diamond, not a different one`() {
        val group = TouchGroupGeometry(400f, 200f)
        val upright = group.squareDiamond(60f)
        TouchClusterRotation.entries.forEach { rotation ->
            val turned = group.squareDiamond(60f, rotation)
            assertEquals(
                "$rotation must reuse the same four placements",
                upright.values.toSet(),
                turned.values.toSet(),
            )
            TouchCardinalSlot.entries.forEach { physical ->
                assertEquals(
                    "$rotation $physical",
                    upright.getValue(rotation.screenSlot(physical)),
                    turned.getValue(physical),
                )
            }
        }
    }

    // ------------------------------------------------------------ screen position

    /**
     * Joy-Con (L) is held rotated anticlockwise, so its directional cluster
     * arrives at the player as an ordinary face diamond: the shell's `up` button
     * is under the thumb on the LEFT, its `right` button on TOP.
     */
    @Test fun `Joy-Con L directions are drawn in the shell's rotated orientation`() {
        assertScreenSlots(
            TouchProfileId.JoyConLeft,
            mapOf(
                TouchCardinalSlot.West to TouchOutputControl.DirectionUp,
                TouchCardinalSlot.North to TouchOutputControl.DirectionRight,
                TouchCardinalSlot.East to TouchOutputControl.DirectionDown,
                TouchCardinalSlot.South to TouchOutputControl.DirectionLeft,
            ),
        )
    }

    /**
     * Joy-Con (R) is held rotated clockwise — the opposite way, because its rail
     * is on the other edge. Its printed X ends up at the player's RIGHT.
     */
    @Test fun `Joy-Con R face buttons are drawn in the shell's rotated orientation`() {
        assertScreenSlots(
            TouchProfileId.JoyConRight,
            mapOf(
                TouchCardinalSlot.East to TouchOutputControl.X,
                TouchCardinalSlot.South to TouchOutputControl.A,
                TouchCardinalSlot.West to TouchOutputControl.B,
                TouchCardinalSlot.North to TouchOutputControl.Y,
            ),
        )
    }

    /** The two halves turn OPPOSITE ways; neither was fixed by copying the other. */
    @Test fun `the two halves are not rotated the same way`() {
        val left = screenSlots(TouchProfileId.JoyConLeft)
        val right = screenSlots(TouchProfileId.JoyConRight)
        // Physically-north on each shell: left's goes west, right's goes east.
        assertEquals(
            TouchCardinalSlot.West,
            left.entries.single { it.value == TouchOutputControl.DirectionUp }.key,
        )
        assertEquals(
            TouchCardinalSlot.East,
            right.entries.single { it.value == TouchOutputControl.X }.key,
        )
    }

    /**
     * What a player actually reads off the screen. Each screen position lands on
     * the button the console will treat as that position's action once the half
     * is registered sideways — which is the point of the whole correction.
     */
    @Test fun `each screen position sends what that position means on a Switch`() {
        // North/East/South/West of a Nintendo face diamond are X/A/B/Y.
        assertEquals(
            mapOf(
                TouchCardinalSlot.North to "X",
                TouchCardinalSlot.East to "A",
                TouchCardinalSlot.South to "B",
                TouchCardinalSlot.West to "Y",
            ),
            screenSlots(TouchProfileId.JoyConLeft).mapValues { (_, output) ->
                // The console derives the action from the raw direction bit; the
                // sideways mapping it applies is the standard one.
                when (output) {
                    TouchOutputControl.DirectionRight -> "X"
                    TouchOutputControl.DirectionDown -> "A"
                    TouchOutputControl.DirectionLeft -> "B"
                    TouchOutputControl.DirectionUp -> "Y"
                    else -> error("$output is not a Joy-Con L direction")
                }
            },
        )
    }

    // --------------------------------------------------------------- logical action

    /**
     * The correction is VISUAL. Every binding must still be exactly what the
     * firmware's sideways encoder expects, or a layout fix has quietly become a
     * remap.
     *
     * These are the pairings in `switch_joycon2_encode.c`'s "paired controller ->
     * sideways Joy-Con 2" branches, transcribed as the bridge usages the
     * companion sends.
     */
    @Test fun `logical bindings still match the firmware's sideways encoder`() {
        val left = TouchProfileCatalog.require(TouchProfileId.JoyConLeft)
        assertEquals(
            // Square/X -> Up, Cross/A -> Left, Triangle/Y -> Right, Circle/B -> Down.
            mapOf(
                TouchOutputControl.DirectionUp to ControllerButton.X,
                TouchOutputControl.DirectionLeft to ControllerButton.A,
                TouchOutputControl.DirectionRight to ControllerButton.Y,
                TouchOutputControl.DirectionDown to ControllerButton.B,
            ),
            left.bindings.filterKeys { it.name.startsWith("Direction") }
                .mapValues { (_, action) -> (action as TouchControlAction.Logical).button },
        )

        val right = TouchProfileCatalog.require(TouchProfileId.JoyConRight)
        assertEquals(
            // Square/X -> B, Triangle/Y -> Y, Cross/A -> A, Circle/B -> X.
            mapOf(
                TouchOutputControl.A to ControllerButton.A,
                TouchOutputControl.B to ControllerButton.X,
                TouchOutputControl.X to ControllerButton.B,
                TouchOutputControl.Y to ControllerButton.Y,
            ),
            right.bindings.filterKeys {
                it in setOf(
                    TouchOutputControl.A, TouchOutputControl.B,
                    TouchOutputControl.X, TouchOutputControl.Y,
                )
            }.mapValues { (_, action) -> (action as TouchControlAction.Logical).button },
        )
    }

    // ------------------------------------------------------------ glyph treatment

    /**
     * A direction marking's meaning IS its orientation, so it turns with the
     * shell; a letter's meaning is the letter, so it stays readable and only its
     * POSITION turns. Blanket-rotating everything would leave the Joy-Con (R)
     * cluster with four sideways letters.
     */
    @Test fun `direction markings turn with the shell and letters do not`() {
        val left = TouchProfileCatalog.require(TouchProfileId.JoyConLeft).defaultTemplate
        left.controls.filter { it.visual.role == TouchVisualRole.JoyConDirectionButton }
            .also { assertEquals(4, it.size) }
            .forEach {
                assertEquals(
                    "${it.id} marking must follow the shell",
                    TouchClusterRotation.QuarterCounterClockwise.degrees,
                    it.visual.rotationDegrees,
                    0f,
                )
                assertEquals("${it.id} draws a marking, not a legend", "", it.visual.label)
            }

        val right = TouchProfileCatalog.require(TouchProfileId.JoyConRight).defaultTemplate
        right.controls.filter { it.editGroupId == "face-cluster" }
            .also { assertEquals(4, it.size) }
            .forEach {
                assertEquals("${it.id} legend must stay upright", 0f, it.visual.rotationDegrees, 0f)
                assertTrue("${it.id} draws a legend", it.visual.label.isNotBlank())
            }
    }

    // ---------------------------------------------------------- latch eligibility

    /**
     * These four ARE the sideways personality's action buttons, so they must
     * stay latch-capable. Excluding everything called `Direction*` would have
     * been an easy way to sidestep the orientation question and would have taken
     * the feature away from the one personality that has nothing else.
     */
    @Test fun `the sideways action clusters remain latch-capable`() {
        listOf(TouchProfileId.JoyConLeft, TouchProfileId.JoyConRight).forEach { id ->
            val template = TouchProfileCatalog.require(id).defaultTemplate
            val cluster = template.controls.filter {
                it.editGroupId == "direction-cluster" || it.editGroupId == "face-cluster"
            }
            assertEquals("$id action cluster", 4, cluster.size)
            cluster.forEach {
                assertTrue("$id ${it.id} must be latchable", it.interaction.supportsLatch)
            }
        }
    }

    // ------------------------------------------------------------------- stability

    /**
     * Moving the shipped defaults is exactly what a template revision records.
     * No control id changed, so a stored profile written against the previous
     * revision still composes and keeps every position its owner chose — the
     * bump is metadata, not a schema gate.
     */
    /**
     * Revision 2 was the shared personality baseline before any correction. Both
     * sideways halves moved their action cluster past it, and the constant is
     * written out here rather than read from another personality: GameCube has
     * since moved its own top row for an unrelated reason, so comparing against
     * a live sibling would pass or fail for reasons that have nothing to do with
     * the Joy-Con correction this test exists for.
     */
    @Test fun `the corrected halves declare a newer template revision`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube).defaultTemplate
        listOf(TouchProfileId.JoyConLeft, TouchProfileId.JoyConRight).forEach { id ->
            val template = TouchProfileCatalog.require(id).defaultTemplate
            assertTrue(
                "$id geometry moved, so its revision must be past the shared baseline",
                template.templateRevision > PERSONALITY_BASELINE_REVISION,
            )
            assertEquals("$id schema is unchanged", gameCube.schemaVersion, template.schemaVersion)
        }
    }

    @Test fun `a profile saved before the correction still composes and keeps its own positions`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.JoyConLeft)
        val stored = TouchLayoutOverride(
            profileId = profile.id,
            templateId = profile.defaultTemplate.id,
            // Written against the pre-correction shipped geometry.
            basedOnRevision = profile.defaultTemplate.templateRevision - 1,
            controls = mapOf("direction-up" to TouchControlOverride(anchorX = 0.5f, anchorY = 0.5f)),
        )
        // Migrated exactly as an upgrading install would migrate it.
        val composed = TouchLayoutComposer.compose(
            profile,
            TouchLayoutMigration.fromOverride(profile, stored),
        )
        assertTrue("stored layouts must not be refused", composed.customized)
        assertEquals(null, composed.warning)
        val moved = composed.layout.controls.single { it.id == "direction-up" }
        assertEquals(0.5f, moved.anchorX, 0f)
        assertEquals(0.5f, moved.anchorY, 0f)
        // Everything the user did NOT customize picks up the corrected default.
        assertEquals(4, composed.layout.controls.count { it.editGroupId == "direction-cluster" })
    }

    // --------------------------------------------------------------------- helpers

    private fun assertScreenSlots(
        id: TouchProfileId,
        expected: Map<TouchCardinalSlot, TouchOutputControl>,
    ) = assertEquals(id.name, expected, screenSlots(id))

    /**
     * Which output occupies each screen slot, read off REAL resolved geometry
     * rather than off the authored numbers, so a template that placed a control
     * correctly and a resolver that moved it would still be caught.
     */
    private fun screenSlots(id: TouchProfileId): Map<TouchCardinalSlot, TouchOutputControl> {
        val profile = TouchProfileCatalog.require(id)
        val resolved = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile).layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
            TouchLayoutAuditMode.ShippedTemplate,
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        val cluster = resolved.controls.filter {
            it.spec.editGroupId == "direction-cluster" || it.spec.editGroupId == "face-cluster"
        }
        assertEquals("$id action cluster", 4, cluster.size)
        return mapOf(
            TouchCardinalSlot.North to cluster.minBy { it.centerY }.spec.output,
            TouchCardinalSlot.South to cluster.maxBy { it.centerY }.spec.output,
            TouchCardinalSlot.West to cluster.minBy { it.centerX }.spec.output,
            TouchCardinalSlot.East to cluster.maxBy { it.centerX }.spec.output,
        )
    }
}
