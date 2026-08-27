package dev.picoswitch.bridge.touch

import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The shipped GameCube arrangement, pinned against the two ways Editor 2.0 came
 * close to changing it by accident.
 *
 * ## What went wrong, so it cannot happen again
 *
 * The X and Y beans carry an AUTHORED art rotation — 10.7 and -11 degrees — that
 * turns each silhouette's concavity toward A. Editor 2.0 added a USER rotation
 * on top and then made two mistakes with the sum:
 *
 * 1. the renderer counter-rotated the beans' LEGENDS by the total, tilting
 *    letters that had never been turned, because the beans draw their own
 *    already-rotated path and the generic wrap must not apply to them;
 * 2. the editor turned every selection outline by the total, drawing a tilted
 *    box around an upright one.
 *
 * Separately, teaching the audit to measure rotated geometry surfaced a real
 * `z`/`Y` collision — but one that lies entirely between the two controls' hit
 * MARGINS. The top row briefly moved to compensate; it should not have, and this
 * suite pins it back at 42.
 *
 * The distinction underneath all three: an authored orientation belongs to the
 * catalog and is part of the artwork, while a user rotation is an instance
 * override that starts at zero.
 */
class TouchGameCubeDefaultTest {

    private val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
    private val document = authored(profile)

    /** The tablet region the approved GameCube render was signed off against. */
    private fun approvedRegion() = TouchLayoutRegion(
        left = 50f,
        top = 60f,
        right = 1474f,
        bottom = 947f,
        unitScale = 267f / 160f,
    )

    private fun resolve(region: TouchLayoutRegion, document: TouchLayoutDocument = this.document) =
        TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile, document).layout,
            region,
            TouchLayoutAuditMode.ShippedTemplate,
        )

    // ------------------------------------------------------ authored transforms

    @Test fun `the shipped top row sits at its established height`() {
        mapOf(
            "trigger-l" to 60f,
            "zl" to 160f,
            "z" to 640f,
            "trigger-r" to 740f,
        ).forEach { (id, x) ->
            val entry = requireNotNull(profile.catalogEntry(id))
            assertEquals(
                "$id must stay in its authored slot",
                x / TouchLayoutResolver.REFERENCE_WIDTH_UNITS,
                entry.geometry.anchorX,
                1e-6f,
            )
            assertEquals(
                "$id must stay on the established row",
                42f / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS,
                entry.geometry.anchorY,
                1e-6f,
            )
        }
        assertEquals("the template revision did not need to move", 2, profile.defaultTemplate.templateRevision)
    }

    @Test fun `X and Y keep their authored art rotation and no user rotation`() {
        mapOf("x" to 10.7f, "y" to -11.0f).forEach { (id, authoredAngle) ->
            val entry = requireNotNull(profile.catalogEntry(id))
            assertEquals("$id authored art", authoredAngle, entry.visual.rotationDegrees, 1e-4f)

            // The default layout adds NOTHING. A user rotation is an override
            // and starts at zero; a default control must never acquire one.
            val instance = requireNotNull(document.instance(id))
            assertEquals("$id user rotation", 0f, instance.rotationDegrees, 0f)

            val control = TouchLayoutComposer.compose(profile, document).layout
                .controls.single { it.id == id }
            assertEquals("$id total", authoredAngle, control.visualRotationDegrees, 1e-4f)
            assertEquals("$id authored", authoredAngle, control.authoredRotationDegrees, 1e-4f)
        }
    }

    @Test fun `every other shipped control is upright`() {
        TouchLayoutComposer.compose(profile, document).layout.controls
            .filterNot { it.id == "x" || it.id == "y" }
            .forEach { assertEquals(it.id, 0f, it.visualRotationDegrees, 0f) }
    }

    // ----------------------------------------------------------- reset orientation

    @Test fun `reset orientation restores the authored art, not zero`() {
        val turned = TouchLayoutEditor.setRotation(document, setOf("y"), 40f)
        assertEquals(
            -11.0f + 40f,
            TouchLayoutComposer.compose(profile, turned).layout.controls.single { it.id == "y" }
                .visualRotationDegrees,
            1e-4f,
        )
        val reset = TouchLayoutEditor.resetRotation(turned, setOf("y"), editGroup = false)
        assertEquals(0f, requireNotNull(reset.instance("y")).rotationDegrees, 0f)
        assertEquals(
            "back to the established appearance",
            -11.0f,
            TouchLayoutComposer.compose(profile, reset).layout.controls.single { it.id == "y" }
                .visualRotationDegrees,
            1e-4f,
        )
        assertEquals(document, reset)
    }

    // -------------------------------------------------------------- outline bounds

    /**
     * The editor outline must describe where the control actually answers.
     *
     * A bean's contour is rotated INSIDE an upright box, so its box is upright;
     * a round button is rotation-invariant, so turning its box would be a tilted
     * square around a circle and mean nothing at all.
     */
    @Test fun `selection outlines are turned only when the box itself is`() {
        val layout = resolve(approvedRegion())
        listOf("x", "y").forEach { id ->
            val control = requireNotNull(layout.control(id))
            assertNotEquals("the bean is drawn turned", 0f, control.spec.visualRotationDegrees)
            assertEquals("but its box is not", 0f, control.outlineRotationDegrees, 0f)
        }
        listOf("a", "b", "plus").forEach { id ->
            assertEquals(id, 0f, requireNotNull(layout.control(id)).outlineRotationDegrees, 0f)
        }
        // A control the USER turns does move its box, because for a rectangle
        // the box IS the region.
        val turned = TouchLayoutEditor.setRotation(document, setOf("z"), 30f)
        assertEquals(
            30f,
            requireNotNull(resolve(approvedRegion(), turned).control("z")).outlineRotationDegrees,
            1e-4f,
        )
    }

    // ------------------------------------------------------------- the real audit

    private fun blockingAt(width: Float, height: Float, density: Float): List<TouchLayoutFinding> {
        val region = TouchLayoutRegion(0f, 0f, width * density, height * density, density)
        val resolved = resolve(region)
        return TouchLayoutAudit.audit(
            resolved.layout, resolved.controls, region, profile,
            TouchLayoutAuditMode.ShippedTemplate,
        ).filter { it.blocking }
    }

    @Test fun `the restored default passes the real audit at every validated probe`() {
        val probes = listOf(
            "16:9 phone" to (915f to 412f),
            "squat" to (800f to 360f),
            "small" to (640f to 360f),
            "tall" to (832f to 440f),
            "tablet 4:3" to (1024f to 768f),
            "tablet 16:10" to (1280f to 800f),
        )
        probes.forEach { (name, size) ->
            listOf(1f, 2f, 2.75f, 3.5f).forEach { density ->
                val blocking = blockingAt(size.first, size.second, density)
                assertEquals("$name @ $density: $blocking", emptyList<TouchLayoutFinding>(), blocking)
            }
        }
    }

    /**
     * A PRE-EXISTING defect, characterised here rather than silently fixed.
     *
     * At aspect ratios near 2:1 the GameCube `c-stick` and the small `B` button
     * genuinely overlap — their DRAWN circles, not merely their margins, by
     * about four units at the worst point. It has nothing to do with Editor 2.0
     * or with the bean rotation: the pre-2.0 audit reported it too (and slightly
     * sooner, because it compared margin-expanded radii), but no shipped probe
     * shape lands inside the band, so nothing ever ran the check there.
     *
     * The band is roughly `1.945 < width/height < 2.057`, which includes 18:9
     * displays. Inside it the shipped GameCube controller refuses to draw.
     *
     * Fixing it means moving or shrinking approved GameCube artwork, which is
     * not something this pass may decide on its own. This test exists so the
     * knowledge is not lost and so that whoever does fix it is told by a failing
     * assertion to come back and delete this.
     */
    @Test fun `KNOWN pre-existing c-stick and B collision near two-to-one`() {
        val inside = blockingAt(800f, 400f, 1f)
        assertEquals(
            "if this is now empty, the defect is fixed -- delete this test",
            1,
            inside.size,
        )
        // Identified by instance id rather than by the sentence: the message is
        // presentation and names controls the way a user reads them, and the
        // ids are what the finding actually points at.
        assertEquals(setOf("c-stick", "b"), inside.single().controlIds)

        // Just outside the band on either side, the shipped layout is clean.
        assertTrue(blockingAt(800f, 420f, 1f).isEmpty())
        assertTrue(blockingAt(800f, 380f, 1f).isEmpty())
    }

    /**
     * The `z`/`Y` proximity, stated exactly, so a future change that turns it
     * into a real collision is caught rather than argued about.
     *
     * Their DRAWN shapes clear each other; only the courtesy margins meet. That
     * is why the arrangement is playable and why the top row did not have to
     * move.
     */
    @Test fun `z and the Y bean touch only in their margins`() {
        val layout = resolve(TouchLayoutRegion(0f, 0f, 800f, 400f, 1f))
        val z = requireNotNull(layout.control("z"))
        val y = requireNotNull(layout.control("y"))

        var sharedArtwork = 0
        var sharedMargin = 0
        var probeY = z.centerY
        while (probeY < y.centerY) {
            var probeX = z.centerX - z.hitExtentX
            while (probeX < z.centerX + z.hitExtentX) {
                if (z.hitTest(probeX, probeY) && y.hitTest(probeX, probeY)) sharedMargin++
                if (z.containsVisual(probeX, probeY) && y.containsVisual(probeX, probeY)) {
                    sharedArtwork++
                }
                probeX += 0.25f
            }
            probeY += 0.25f
        }
        assertTrue("their margins do meet: $sharedMargin", sharedMargin > 0)
        assertEquals("but the drawn shapes do not", 0, sharedArtwork)
    }

    /** Artwork that genuinely collides is still refused. */
    @Test fun `a control dragged onto the bean is still blocked`() {
        val bean = requireNotNull(resolve(TouchLayoutRegion(0f, 0f, 800f, 400f, 1f)).control("y"))
        val onTop = TouchLayoutEditor.place(
            document,
            setOf("z"),
            bean.centerX / 800f,
            bean.centerY / 400f,
        )
        val resolved = resolve(TouchLayoutRegion(0f, 0f, 800f, 400f, 1f), onTop)
        assertFalse(resolved.fits)
        assertTrue(requireNotNull(resolved.problem).contains("overlap"))
    }

    // ----------------------------------------------------------------- migration

    @Test fun `a stored GameCube layout still migrates onto the restored default`() {
        val template = profile.defaultTemplate
        val legacy = TouchLayoutOverride(
            profileId = TouchProfileId.GameCube,
            templateId = template.id,
            basedOnRevision = template.templateRevision,
            controls = mapOf("dpad" to TouchControlOverride(scale = 1.1f)),
        )
        val migrated = TouchLayoutMigration.fromOverride(profile, legacy)
        // The migration carries the user's change and nothing else: every other
        // instance is exactly the authored one, rotation included.
        document.controls.forEach { authoredInstance ->
            val after = requireNotNull(migrated.instance(authoredInstance.instanceId))
            if (authoredInstance.instanceId == "dpad") {
                assertEquals(1.1f, after.scale, 1e-6f)
            } else {
                assertEquals(authoredInstance, after)
            }
            assertEquals("migration never invents a rotation", 0f, after.rotationDegrees, 0f)
        }
    }

    /** The approved face-cluster coordinates, unchanged by any of this. */
    @Test fun `the approved face cluster is where it was signed off`() {
        val controls = resolve(approvedRegion()).controls.associateBy { it.spec.output }
        listOf(
            Approved(TouchOutputControl.A, 1260.38f, 433.08f, 79.07f, 79.07f),
            Approved(TouchOutputControl.B, 1137.28f, 536.42f, 50.32f, 50.32f),
            Approved(TouchOutputControl.X, 1398.76f, 424.10f, 46.73f, 75.48f),
            Approved(TouchOutputControl.Y, 1192.99f, 312.23f, 75.48f, 48.52f),
        ).forEach { approved ->
            val actual = controls.getValue(approved.output)
            assertEquals("${approved.output} x", approved.centerX, actual.centerX, 0.02f)
            assertEquals("${approved.output} y", approved.centerY, actual.centerY, 0.05f)
            assertEquals("${approved.output} w", approved.halfWidth, actual.halfWidth, 0.02f)
            assertEquals("${approved.output} h", approved.halfHeight, actual.halfHeight, 0.02f)
        }
        // And the top row is above them by the established distance.
        val z = controls.getValue(TouchOutputControl.Z)
        val y = controls.getValue(TouchOutputControl.Y)
        assertTrue("z stays above Y", z.centerY < y.centerY)
        assertTrue(abs(z.centerY - y.centerY) > 100f)
    }

    private data class Approved(
        val output: TouchOutputControl,
        val centerX: Float,
        val centerY: Float,
        val halfWidth: Float,
        val halfHeight: Float,
    )
}
