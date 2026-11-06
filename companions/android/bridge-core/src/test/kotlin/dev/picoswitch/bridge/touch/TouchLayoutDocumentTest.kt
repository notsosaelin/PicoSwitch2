package dev.picoswitch.bridge.touch

import kotlin.math.abs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The instance document itself: identity, validation, and the invariants the
 * whole editor rests on.
 *
 * Two statements are worth more than the rest of this file put together, and
 * both are asserted here rather than left to a comment:
 *
 * > Control instance identity is not logical button identity.
 * > Default layout membership is not personality capability.
 */
class TouchLayoutDocumentTest {

    private val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)

    // ------------------------------------------------------------------- identity

    @Test fun `instance identity survives every transform`() {
        val base = authored(pro2)
        val layout = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(pro2, base).layout,
            TouchLayoutRegion(0f, 0f, 832f, 440f, 1f),
            TouchLayoutAuditMode.UserDraft,
        )
        val id = TouchLayoutV1.FACE_SOUTH
        var document = base
        document = TouchLayoutEditor.move(document, layout, setOf(id), 12f, -8f, editGroup = false)
        document = TouchLayoutEditor.scaleBy(document, layout, setOf(id), 1.2f, editGroup = false)
        document = TouchLayoutEditor.rotateBy(document, layout, setOf(id), 33f, editGroup = false)
        document = TouchLayoutEditor.group(document, setOf(id, TouchLayoutV1.FACE_EAST)).document
        document = TouchLayoutEditor.bringToFront(document, setOf(id), editGroup = false)

        val instance = requireNotNull(document.instance(id))
        assertEquals(id, instance.instanceId)
        assertEquals(id, instance.catalogId)
        assertNotEquals(base.instance(id), instance)
    }

    @Test fun `a duplicate is a separate object that shares only its binding`() {
        val added = TouchLayoutEditor.add(authored(pro2), pro2, TouchLayoutV1.FACE_SOUTH, 0.5f, 0.5f)
        val second = added.created.single()
        assertNotEquals(TouchLayoutV1.FACE_SOUTH, second)
        assertEquals(TouchLayoutV1.FACE_SOUTH, added.document.instance(second)?.catalogId)

        val layout = TouchLayoutComposer.compose(pro2, added.document).layout
        val instances = layout.controls.filter { it.output == TouchOutputControl.FaceSouth }
        assertEquals(2, instances.size)
        assertEquals("same binding", 1, instances.map { it.action }.distinct().size)
        assertEquals("different identity", 2, instances.map { it.id }.distinct().size)
    }

    @Test fun `allocated ids are deterministic and never collide`() {
        var document = authored(pro2)
        val created = mutableListOf<String>()
        repeat(4) {
            val added = TouchLayoutEditor.add(document, pro2, TouchLayoutV1.HOME, 0.5f, 0.5f)
            document = added.document
            created += added.created.single()
        }
        assertEquals(listOf("home#2", "home#3", "home#4", "home#5"), created)
        assertEquals(
            "no duplicate identities",
            document.controls.size,
            document.controls.mapTo(mutableSetOf()) { it.instanceId }.size,
        )
    }

    // ----------------------------------------------------------------- the catalog

    @Test fun `default layout membership is not personality capability`() {
        val document = authored(pro2)
        // GL and GR are absent from the layout...
        assertNull(document.instance(TouchLayoutV1.GRIP_LEFT))
        // ... and fully present in the catalog, with a real binding.
        val entry = requireNotNull(pro2.catalogEntry(TouchLayoutV1.GRIP_LEFT))
        assertFalse(entry.inDefaultLayout)
        assertTrue(TouchOutputControl.GL in pro2.outputs)
        assertEquals(
            TouchControlAction.Logical(dev.picoswitch.bridge.core.ControllerButton.GL),
            pro2.bindings.getValue(TouchOutputControl.GL),
        )
        // ... so Add Control can instantiate it like anything else.
        val added = TouchLayoutEditor.add(document, pro2, TouchLayoutV1.GRIP_LEFT, 0.5f, 0.5f)
        assertTrue(added.changed)
        assertEquals(
            entry.geometry.anchorX,
            requireNotNull(added.document.instance(TouchLayoutV1.GRIP_LEFT)).anchorX,
            0f,
        )
    }

    @Test fun `every catalog entry can be instantiated for its own personality`() {
        TouchProfileCatalog.profiles.values.forEach { profile ->
            profile.catalog.forEach { entry ->
                val result = TouchLayoutEditor.add(
                    TouchLayoutDocument(
                        profileId = profile.id,
                        templateId = profile.defaultTemplate.id,
                        basedOnRevision = profile.defaultTemplate.templateRevision,
                    ),
                    profile,
                    entry.id,
                    0.5f,
                    0.5f,
                )
                assertTrue("${profile.id}/${entry.id}: ${result.refusal}", result.changed)
                val composed = TouchLayoutComposer.compose(profile, result.document).layout
                assertEquals(1, composed.controls.size)
                assertEquals(entry.output, composed.controls.single().output)
            }
        }
    }

    @Test fun `a catalog id from another personality is refused, not silently placed`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val result = TouchLayoutEditor.add(
            authored(gameCube), gameCube, TouchLayoutV1.GRIP_LEFT, 0.5f, 0.5f,
        )
        assertFalse(result.changed)
        assertEquals(authored(gameCube), result.document)
    }

    // ------------------------------------------------------------------ validation

    @Test fun `impossible numbers cost the instance, not the layout`() {
        val base = authored(pro2)
        listOf(
            base.controls.first().copy(instanceId = "nan-x", anchorX = Float.NaN),
            base.controls.first().copy(instanceId = "inf-y", anchorY = Float.POSITIVE_INFINITY),
            base.controls.first().copy(instanceId = "nan-scale", scale = Float.NaN),
            base.controls.first().copy(instanceId = "nan-rot", rotationDegrees = Float.NaN),
            base.controls.first().copy(instanceId = "inf-offset", offsetXUnits = Float.NEGATIVE_INFINITY),
        ).forEach { bad ->
            val validated = TouchLayoutDocumentValidator.validate(
                base.copy(controls = base.controls + bad),
                pro2,
            )
            assertTrue("${bad.instanceId} was accepted", validated.degraded)
            assertNull(validated.document.instance(bad.instanceId))
            assertEquals(
                "${bad.instanceId} cost an unrelated control",
                base.controls.size,
                validated.document.controls.size,
            )
        }
    }

    @Test fun `out-of-range values are clamped rather than dropped`() {
        val base = authored(pro2)
        val id = base.controls.first().instanceId
        val stretched = base.copy(
            controls = base.controls.map {
                if (it.instanceId == id) it.copy(anchorX = 4f, anchorY = -2f, scale = 99f) else it
            },
        )
        val validated = TouchLayoutDocumentValidator.validate(stretched, pro2)
        val repaired = requireNotNull(validated.document.instance(id))
        assertEquals(1f, repaired.anchorX, 0f)
        assertEquals(0f, repaired.anchorY, 0f)
        assertEquals(TouchLayoutLimits.MAX_SCALE, repaired.scale, 0f)
        assertFalse("clamping is not a loss", validated.degraded)
    }

    @Test fun `a document for another controller is replaced, never reinterpreted`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val validated = TouchLayoutDocumentValidator.validate(authored(gameCube), pro2)
        assertTrue(validated.degraded)
        assertEquals(authored(pro2), validated.document)
    }

    @Test fun `a latch opinion cannot survive on a control that cannot hold`() {
        val base = authored(pro2)
        val forced = base.copy(
            controls = base.controls.map {
                if (it.instanceId == TouchLayoutV1.STICK_LEFT) it.copy(latch = true) else it
            },
        )
        val validated = TouchLayoutDocumentValidator.validate(forced, pro2)
        assertNull(requireNotNull(validated.document.instance(TouchLayoutV1.STICK_LEFT)).latch)
    }

    // ---------------------------------------------------------------------- groups

    @Test fun `an instance can belong to at most one group, structurally`() {
        val document = authored(pro2)
        val grouped = TouchLayoutEditor.group(
            document, setOf(TouchLayoutV1.FACE_SOUTH, TouchLayoutV1.MINUS),
        ).document
        val south = requireNotNull(grouped.instance(TouchLayoutV1.FACE_SOUTH))
        // It LEFT the authored face cluster; there is one groupId field, so
        // being in two groups is not a state this model can represent.
        assertNotEquals("face-cluster", south.groupId)
        assertEquals(south.groupId, grouped.instance(TouchLayoutV1.MINUS)?.groupId)
        assertEquals(
            setOf(TouchLayoutV1.FACE_SOUTH, TouchLayoutV1.MINUS),
            grouped.groupMembers(TouchLayoutV1.FACE_SOUTH),
        )
    }

    @Test fun `grouping and ungrouping never touch geometry`() {
        val document = authored(pro2)
        val ids = setOf(TouchLayoutV1.FACE_SOUTH, TouchLayoutV1.STICK_RIGHT, TouchLayoutV1.PLUS)
        val grouped = TouchLayoutEditor.group(document, ids).document
        val ungrouped = TouchLayoutEditor.ungroup(grouped, ids).document

        ids.forEach { id ->
            val before = requireNotNull(document.instance(id))
            val after = requireNotNull(ungrouped.instance(id))
            assertEquals(before.copy(groupId = after.groupId), after)
        }
        // Ungroup is visually lossless because it is not a geometric operation
        // at all -- the composed layout is identical apart from membership.
        assertEquals(
            TouchLayoutComposer.compose(pro2, document).layout.controls.map { it.copy(editGroupId = null) },
            TouchLayoutComposer.compose(pro2, ungrouped).layout.controls.map { it.copy(editGroupId = null) },
        )
    }

    @Test fun `grouping fewer than two controls is refused with a reason`() {
        val document = authored(pro2)
        val refused = TouchLayoutEditor.group(document, setOf(TouchLayoutV1.PLUS))
        assertFalse(refused.changed)
        assertTrue(refused.refusal!!.isNotBlank())
        assertEquals(document, refused.document)
    }

    // --------------------------------------------------------------------- z-order

    @Test fun `z-order operations produce a dense, total ordering`() {
        val document = authored(pro2)
        val front = TouchLayoutEditor.bringToFront(document, setOf(TouchLayoutV1.DPAD), editGroup = false)
        assertEquals(
            document.controls.size - 1,
            requireNotNull(front.instance(TouchLayoutV1.DPAD)).zIndex,
        )
        val back = TouchLayoutEditor.sendToBack(front, setOf(TouchLayoutV1.DPAD), editGroup = false)
        assertEquals(0, requireNotNull(back.instance(TouchLayoutV1.DPAD)).zIndex)
        assertEquals(
            "every index is used exactly once",
            back.controls.indices.toSet(),
            back.controls.mapTo(mutableSetOf()) { it.zIndex },
        )

        val forward = TouchLayoutEditor.bringForward(back, setOf(TouchLayoutV1.DPAD), editGroup = false)
        assertEquals(1, requireNotNull(forward.instance(TouchLayoutV1.DPAD)).zIndex)
        val backward = TouchLayoutEditor.sendBackward(forward, setOf(TouchLayoutV1.DPAD), editGroup = false)
        assertEquals(0, requireNotNull(backward.instance(TouchLayoutV1.DPAD)).zIndex)
    }

    @Test fun `stepping a contiguous run keeps it together`() {
        val document = authored(pro2)
        val pair = setOf(TouchLayoutV1.CAPTURE, TouchLayoutV1.HOME)
        val before = pair.associateWith { requireNotNull(document.instance(it)).zIndex }
        val forward = TouchLayoutEditor.bringForward(document, pair, editGroup = false)
        val after = pair.associateWith { requireNotNull(forward.instance(it)).zIndex }
        pair.forEach { assertEquals(before.getValue(it) + 1, after.getValue(it)) }
    }

    // -------------------------------------------------------------------- rotation

    @Test fun `rotation is stored normalized and reset restores the authored angle`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val document = authored(gameCube)
        val turned = TouchLayoutEditor.setRotation(document, setOf("y"), 400f)
        assertEquals(40f, requireNotNull(turned.instance("y")).rotationDegrees, 1e-4f)

        val wrapped = TouchLayoutEditor.setRotation(document, setOf("y"), -190f)
        assertEquals(170f, requireNotNull(wrapped.instance("y")).rotationDegrees, 1e-4f)

        // The authored bean is NOT at zero, so reset means the catalog's angle.
        val authoredAngle = requireNotNull(gameCube.catalogEntry("y")).visual.rotationDegrees
        assertNotEquals(0f, authoredAngle)
        val reset = TouchLayoutEditor.resetRotation(turned, setOf("y"), editGroup = false)
        assertEquals(0f, requireNotNull(reset.instance("y")).rotationDegrees, 0f)
        assertEquals(
            "reset means the authored orientation, not a blind zero",
            authoredAngle,
            TouchLayoutComposer.compose(gameCube, reset).layout.controls
                .single { it.id == "y" }.visualRotationDegrees,
            1e-4f,
        )
    }

    @Test fun `the composed angle is the authored one plus the user's`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val authoredAngle = requireNotNull(gameCube.catalogEntry("y")).visual.rotationDegrees
        val turned = TouchLayoutEditor.setRotation(authored(gameCube), setOf("y"), 25f)
        val control = TouchLayoutComposer.compose(gameCube, turned).layout.controls
            .single { it.id == "y" }
        assertEquals(authoredAngle + 25f, control.visualRotationDegrees, 1e-4f)
        assertEquals(authoredAngle, control.authoredRotationDegrees, 1e-4f)
    }

    @Test fun `rotation snaps near the authored orientation and its quarter turns`() {
        // Zero IS the authored orientation, because the stored angle is relative.
        assertEquals(0f, TouchLayoutEditor.snapRotation(3f), 0f)
        assertEquals(90f, TouchLayoutEditor.snapRotation(87f), 0f)
        // Half a turn has one stored spelling: the range is [-180, 180).
        assertEquals(-180f, TouchLayoutEditor.snapRotation(-177f), 0f)
        assertEquals(-180f, TouchLayoutEditor.snapRotation(178f), 0f)
        assertEquals(-90f, TouchLayoutEditor.snapRotation(-93f), 0f)
        // ... and never traps a deliberate angle in between.
        assertEquals(45f, TouchLayoutEditor.snapRotation(45f), 0f)
        assertEquals(30f, TouchLayoutEditor.snapRotation(30f), 0f)
        assertTrue(abs(TouchLayoutEditor.snapRotation(70f) - 70f) < 1e-4f)
    }

    /**
     * A live rotation snaps by adjusting the DELTA, so the whole selection turns
     * by the same amount and only the reference control is pinned to the target.
     */
    @Test fun `a live rotation is pulled onto a target without trapping the user`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val near = TouchLayoutEditor.setRotation(authored(gameCube), setOf("y"), 86f)
        // 4 degrees short of a quarter turn: the delta is stretched to land on it.
        assertEquals(4f, TouchLayoutEditor.snappedRotationDelta(near, "y", 2f, 88f), 1e-3f)
        // Far from any target: the delta is passed through untouched.
        assertEquals(-40f, TouchLayoutEditor.snappedRotationDelta(near, "y", -40f, 46f), 1e-3f)
        // No reference control: nothing to snap against, so nothing changes.
        assertEquals(9f, TouchLayoutEditor.snappedRotationDelta(near, null, 9f, 95f), 0f)
        assertEquals(9f, TouchLayoutEditor.snappedRotationDelta(near, "nope", 9f, 95f), 0f)
    }

    @Test fun `snapping across the half-turn seam takes the short way round`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val near = TouchLayoutEditor.setRotation(authored(gameCube), setOf("y"), 178f)
        val delta = TouchLayoutEditor.snappedRotationDelta(near, "y", 3f, 181f)
        // -180 and 178 are two degrees apart, not 358.
        assertTrue("a snap must not spin the control: $delta", abs(delta) <= 10f)
        val turned = TouchLayoutEditor.setRotation(near, setOf("y"), 178f + delta)
        assertEquals(-180f, requireNotNull(turned.instance("y")).rotationDegrees, 1e-3f)
    }

    /**
     * The snap is a DETENT, not a wall, and this is the test that says so.
     *
     * The defect it pins: with the target derived from the stored angle plus one
     * frame's delta, a control starting on a snap target could never leave it.
     * Every frame proposed stored-plus-a-fraction, the magnet pulled it back to
     * the same place, so the stored angle never moved and the next frame asked
     * the identical question. Rotation only escaped when a single frame carried
     * more than the snap radius, which on a 120 Hz panel means flinging a whole
     * hand. Feeding the gesture's own accumulated intent is what fixes it, and a
     * simulated slow turn is the only way to catch a regression.
     */
    @Test fun `a slow rotation escapes the detent it started in`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        var document = authored(gameCube)
        assertEquals(0f, requireNotNull(document.instance("y")).rotationDegrees, 0f)

        // Sixty frames of a deliberate but unhurried turn: a third of a degree
        // each, well inside the snap radius, 20 degrees in total.
        val perFrame = 1f / 3f
        var intent = 0f
        repeat(60) {
            intent += perFrame
            val applied = TouchLayoutEditor.snappedRotationDelta(document, "y", perFrame, intent)
            document = TouchLayoutEditor.setRotation(
                document,
                setOf("y"),
                requireNotNull(document.instance("y")).rotationDegrees + applied,
            )
        }
        assertEquals(
            "the control follows the fingers once they leave the detent",
            20f,
            requireNotNull(document.instance("y")).rotationDegrees,
            1e-2f,
        )
    }

    /** Inside the detent the control genuinely holds still. That part is wanted. */
    @Test fun `a rotation smaller than the snap radius is absorbed`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        var document = authored(gameCube)
        var intent = 0f
        repeat(12) {
            intent += 0.4f
            val applied = TouchLayoutEditor.snappedRotationDelta(document, "y", 0.4f, intent)
            document = TouchLayoutEditor.setRotation(
                document,
                setOf("y"),
                requireNotNull(document.instance("y")).rotationDegrees + applied,
            )
        }
        // 4.8 degrees of intent, still within ROTATION_SNAP_DEGREES of zero.
        assertEquals(0f, requireNotNull(document.instance("y")).rotationDegrees, 1e-3f)
    }

    /**
     * Scale and rotation come from the same two fingers and must not suppress
     * each other: a frame that pinches AND turns has to do both.
     */
    @Test fun `a pinch that also turns applies both`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val region = TouchLayoutRegion(0f, 0f, 1600f, 900f, 1f)
        val layout = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(gameCube, authored(gameCube)).layout,
            region,
            TouchLayoutAuditMode.UserDraft,
        )
        var document = authored(gameCube)
        var intent = 0f
        repeat(40) {
            document = TouchLayoutEditor.scaleBy(document, layout, setOf("y"), 1.01f, false)
            intent += 0.5f
            val applied = TouchLayoutEditor.snappedRotationDelta(document, "y", 0.5f, intent)
            document = TouchLayoutEditor.rotateBy(document, layout, setOf("y"), applied, false)
        }
        val instance = requireNotNull(document.instance("y"))
        assertTrue("scale did not move: ${instance.scale}", instance.scale > 1.2f)
        assertEquals("rotation did not move", 20f, instance.rotationDegrees, 1e-2f)
    }

    /**
     * The line the design draws and the one most likely to be erased later:
     * rotation is presentation and hit geometry, never semantics.
     */
    @Test fun `rotation changes no binding, no direction and no trigger`() {
        val gameCube = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val base = TouchLayoutComposer.compose(gameCube, authored(gameCube)).layout
        val turned = TouchLayoutComposer.compose(
            gameCube,
            gameCube.catalog.fold(authored(gameCube)) { document, entry ->
                TouchLayoutEditor.setRotation(document, setOf(entry.id), 47f)
            },
        ).layout
        assertEquals(base.controls.size, turned.controls.size)
        base.controls.zip(turned.controls).forEach { (before, after) ->
            assertEquals(before.id, after.id)
            assertEquals("binding", before.action, after.action)
            assertEquals("output", before.output, after.output)
            assertEquals("kind", before.kind, after.kind)
            assertEquals("size", before.widthUnits, after.widthUnits, 0f)
            assertEquals("anchor", before.anchorX, after.anchorX, 0f)
            assertNotEquals(before.visualRotationDegrees, after.visualRotationDegrees)
        }
    }

    // -------------------------------------------------------------- rotated hitting

    @Test fun `a rotated control answers where it is drawn, not where it was`() {
        val (document, ids) = documentOf(pro2, TouchLayoutV1.SHOULDER_LEFT to (0.5f to 0.5f))
        val id = ids.single()
        val region = TouchLayoutRegion(0f, 0f, 832f, 440f, 1f)

        fun place(rotation: Float) = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(
                pro2,
                TouchLayoutEditor.setRotation(document, setOf(id), rotation),
            ).layout,
            region,
            TouchLayoutAuditMode.UserDraft,
        ).control(id)!!

        val upright = place(0f)
        val turned = place(90f)
        val halfLong = upright.hitHalfWidth
        val halfShort = upright.hitHalfHeight
        assertTrue("the pad is oblong to begin with", halfLong > halfShort + 4f)

        // A point off the long axis: inside when upright, outside once turned.
        val alongX = upright.centerX + (halfLong + halfShort) / 2f
        assertTrue(upright.hitTest(alongX, upright.centerY))
        assertFalse("the turned pad no longer reaches sideways", turned.hitTest(alongX, turned.centerY))

        // ... and the mirror image on the short axis.
        val alongY = upright.centerY + (halfLong + halfShort) / 2f
        assertFalse(upright.hitTest(upright.centerX, alongY))
        assertTrue("the turned pad now reaches downward", turned.hitTest(turned.centerX, alongY))

        // The screen-space extents follow the drawing, which is what the audit
        // and the drag clamp both measure against.
        assertEquals(upright.hitHalfHeight, turned.hitExtentX, 1e-3f)
        assertEquals(upright.hitHalfWidth, turned.hitExtentY, 1e-3f)
    }

    @Test fun `a rotated control is still refused when it leaves the safe area`() {
        // High enough that the upright pad clears the safe edge, low enough that
        // the same pad on its side does not.
        val (document, ids) = documentOf(pro2, TouchLayoutV1.SHOULDER_LEFT to (0.5f to 0.09f))
        val id = ids.single()
        val region = TouchLayoutRegion(0f, 0f, 832f, 440f, 1f)

        fun fits(rotation: Float) = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(
                pro2,
                TouchLayoutEditor.setRotation(document, setOf(id), rotation),
            ).layout,
            region,
            TouchLayoutAuditMode.UserDraft,
        )

        assertTrue(fits(0f).problem ?: "", fits(0f).fits)
        // Turned on its side the same pad is much taller and runs off the top.
        val turned = fits(90f)
        assertFalse("a turned control must be measured as drawn", turned.fits)
        assertTrue(turned.problem!!.contains("outside the interaction area"))
    }
}
