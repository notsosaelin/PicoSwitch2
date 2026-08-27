package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.core.ControllerLayoutResolver
import java.nio.file.Files
import java.nio.file.Path
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

private val PROFILE_PROBES = listOf(
    640f to 360f,
    800f to 360f,
    915f to 412f,
    1024f to 600f,
    1024f to 768f,
    1280f to 800f,
    832f to 440f,
)

private fun profileRegion(width: Float = 832f, height: Float = 440f, density: Float = 1f) =
    TouchLayoutRegion(0f, 0f, width * density, height * density, density)

private fun TouchLayoutTemplate.control(output: TouchOutputControl): TouchTemplateControl =
    controls.single { it.output == output }

private val TouchControlGeometry.referenceX: Float
    get() = anchorX * TouchLayoutResolver.REFERENCE_WIDTH_UNITS + groupOffsetXUnits

private val TouchControlGeometry.referenceY: Float
    get() = anchorY * TouchLayoutResolver.REFERENCE_HEIGHT_UNITS + groupOffsetYUnits

private data class TestVisualBounds(
    val left: Float,
    val top: Float,
    val right: Float,
    val bottom: Float,
) {
    val centerX: Float get() = (left + right) / 2f
    val centerY: Float get() = (top + bottom) / 2f
    val width: Float get() = right - left
    val height: Float get() = bottom - top

    fun union(other: TestVisualBounds) = TestVisualBounds(
        left = minOf(left, other.left),
        top = minOf(top, other.top),
        right = maxOf(right, other.right),
        bottom = maxOf(bottom, other.bottom),
    )
}

private fun TouchTemplateControl.testVisualBounds(): TestVisualBounds {
    val centerX = geometry.referenceX
    val centerY = geometry.referenceY
    if (visual.role == TouchVisualRole.GameCubeBeanX ||
        visual.role == TouchVisualRole.GameCubeBeanY
    ) {
        val points = TouchGameCubeGeometry.orientedContour(
            visual.role,
            geometry.widthUnits,
            geometry.heightUnits,
            visual.rotationDegrees,
        )
        return TestVisualBounds(
            left = centerX + points.minOf { it.x },
            top = centerY + points.minOf { it.y },
            right = centerX + points.maxOf { it.x },
            bottom = centerY + points.maxOf { it.y },
        )
    }
    return TestVisualBounds(
        left = centerX - geometry.widthUnits / 2f,
        top = centerY - geometry.heightUnits / 2f,
        right = centerX + geometry.widthUnits / 2f,
        bottom = centerY + geometry.heightUnits / 2f,
    )
}

private fun Iterable<TouchTemplateControl>.compoundVisualBounds(): TestVisualBounds =
    map(TouchTemplateControl::testVisualBounds).reduce(TestVisualBounds::union)

private fun TouchTemplateControl.testVisualGapFrom(circle: TouchTemplateControl): Float {
    val circleRadius = circle.geometry.widthUnits / 2f
    val centerDx = geometry.referenceX - circle.geometry.referenceX
    val centerDy = geometry.referenceY - circle.geometry.referenceY
    if (visual.role == TouchVisualRole.GameCubeBeanX ||
        visual.role == TouchVisualRole.GameCubeBeanY
    ) {
        return TouchGameCubeGeometry.orientedContour(
            visual.role,
            geometry.widthUnits,
            geometry.heightUnits,
            visual.rotationDegrees,
        ).minOf { point ->
            kotlin.math.sqrt(
                (centerDx + point.x) * (centerDx + point.x) +
                    (centerDy + point.y) * (centerDy + point.y),
            )
        } - circleRadius
    }
    return kotlin.math.sqrt(centerDx * centerDx + centerDy * centerDy) -
        circleRadius - geometry.widthUnits / 2f
}

class TouchProfileCatalogTest {
    @Test fun `catalog exhaustively registers the four gameplay profiles`() {
        assertEquals(TouchProfileId.entries.toSet(), TouchProfileCatalog.profiles.keys)
    }

    @Test fun `every shipped profile has one complete fixed binding contract`() {
        TouchProfileCatalog.profiles.values.forEach { profile ->
            assertEquals(profile.id, profile.defaultTemplate.profileId)
            assertEquals(profile.outputs, profile.bindings.keys)
            assertFalse(profile.outputs.contains(TouchOutputControl.Unspecified))
            assertEquals(
                "${profile.id} duplicate output",
                profile.defaultTemplate.controls.size,
                profile.defaultTemplate.controls.map { it.output }.toSet().size,
            )
            assertEquals(
                "${profile.id} template contract",
                profile.outputs,
                profile.defaultTemplate.controls.mapTo(mutableSetOf()) { it.output },
            )
        }
    }

    @Test fun `Pro2 profile composition is byte-for-value geometry parity with V1`() {
        val composed = TouchLayoutComposer.compose(TouchProfileCatalog.require(TouchProfileId.Pro2))
        assertNull(composed.warning)
        assertFalse(composed.customized)
        // Modulo z-order only: V1 is the hand-authored reference and predates
        // explicit stacking, so it states every control at zero while the
        // authored document numbers them in list order. Every other field --
        // identity, binding, geometry, art -- must match exactly.
        assertEquals(
            TouchLayoutV1.layout.controls,
            composed.layout.controls.map { it.copy(zIndex = 0) },
        )
        assertEquals(
            TouchLayoutV1.layout.controls.indices.toList(),
            composed.layout.controls.map { it.zIndex },
        )
    }

    @Test fun `every shipped template audits at every probe and density`() {
        TouchProfileCatalog.profiles.values.forEach { profile ->
            val layout = TouchLayoutComposer.compose(profile).layout
            PROFILE_PROBES.forEach { (width, height) ->
                listOf(1f, 2f, 2.75f, 3.5f).forEach { density ->
                    val resolved = TouchLayoutResolver.resolve(
                        layout,
                        profileRegion(width, height, density),
                        TouchLayoutAuditMode.ShippedTemplate,
                    )
                    assertTrue(
                        "${profile.id} ${width}x$height density=$density: ${resolved.problem}",
                        resolved.fits,
                    )
                }
            }
        }
    }

    @Test fun `Joy-Con templates use one primary stick and independent button clusters`() {
        listOf(TouchProfileId.JoyConLeft, TouchProfileId.JoyConRight).forEach { id ->
            val profile = TouchProfileCatalog.require(id)
            assertEquals(
                TouchControlAction.Stick(ControlSide.Left),
                profile.bindings.getValue(TouchOutputControl.PrimaryStick),
            )
            assertEquals(
                TouchControlAction.Logical(ControllerButton.LeftStick),
                profile.bindings.getValue(TouchOutputControl.PrimaryStickClick),
            )
            assertFalse(profile.bindings.values.any { it == TouchControlAction.Directions })
            assertFalse(profile.outputs.contains(TouchOutputControl.SecondaryStick))
        }
        val left = TouchProfileCatalog.require(TouchProfileId.JoyConLeft)
        assertEquals(
            setOf(
                TouchOutputControl.DirectionUp,
                TouchOutputControl.DirectionLeft,
                TouchOutputControl.DirectionRight,
                TouchOutputControl.DirectionDown,
            ),
            left.outputs.filterTo(mutableSetOf()) { it.name.startsWith("Direction") },
        )
    }

    @Test fun `GameCube has complete native touch outputs and never stick clicks`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        assertTrue(profile.outputs.containsAll(setOf(
            TouchOutputControl.Z,
            TouchOutputControl.ZL,
            TouchOutputControl.L,
            TouchOutputControl.R,
            TouchOutputControl.PrimaryStick,
            TouchOutputControl.SecondaryStick,
        )))
        // The only two controls in the whole catalog with real travel. The
        // GameCube encoder passes their byte through continuously and the
        // firmware seam derives the terminal click from it; every other trigger
        // in the catalog is digital on the far side, so a travel gesture there
        // would let a stray drag send nothing at all.
        assertEquals(
            TouchControlAction.Trigger(ControlSide.Left, analog = true),
            profile.bindings.getValue(TouchOutputControl.L),
        )
        assertEquals(
            TouchControlAction.Trigger(ControlSide.Right, analog = true),
            profile.bindings.getValue(TouchOutputControl.R),
        )
        TouchProfileCatalog.profiles.forEach { (id, other) ->
            if (id == TouchProfileId.GameCube) return@forEach
            other.bindings.values.filterIsInstance<TouchControlAction.Trigger>().forEach {
                assertFalse("$id declared an analog trigger", it.analog)
            }
        }
        assertTrue(profile.outputs.contains(TouchOutputControl.Plus))
        assertFalse(profile.outputs.contains(TouchOutputControl.Minus))
        assertFalse(profile.outputs.contains(TouchOutputControl.PrimaryStickClick))
        assertFalse(profile.outputs.contains(TouchOutputControl.SecondaryStickClick))
    }

    /**
     * The physical legend and the host-facing semantic are separate facts about
     * this one control, and both are pinned here because conflating them is what
     * broke it.
     *
     * A GameCube shell prints `Z`. The protocol slot it occupies is the Switch
     * ZR control: a real Switch 2's Test Input screen names it "ZR", and a
     * genuine NSO GameCube Controller's Z is what Windows/Steam recognizes as
     * ZR. So the touch control keeps the legend the shell has, and its BINDING
     * is the shoulder the firmware's GameCube policy turns into that slot --
     * `R1`, which `ns2_seam.c` folds into `GC_MASK_Z` and both reports then
     * carry (report `0x0A`'s GC slot for the console, report `0x05`'s ZR bit for
     * a PC). Renaming the legend to "ZR" would be the wrong repair: nothing on
     * the device the user is holding says ZR.
     */
    @Test fun `GameCube Z keeps its shell legend and binds to the shoulder that becomes ZR`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)

        assertEquals(
            "Z binds to the shoulder the GameCube policy folds into GC_MASK_Z",
            TouchControlAction.Logical(ControllerButton.R1),
            profile.bindings.getValue(TouchOutputControl.Z),
        )
        // Its mirror, so the pair cannot drift apart the way the two reports did.
        assertEquals(
            TouchControlAction.Logical(ControllerButton.L1),
            profile.bindings.getValue(TouchOutputControl.ZL),
        )

        val z = profile.defaultTemplate.control(TouchOutputControl.Z)
        assertEquals("the legend is the shell's, not the protocol's", "Z", z.visual.label)
        assertEquals(TouchControlKind.Button, z.interaction)

        // The protocol semantic must never leak into the presentation.
        profile.defaultTemplate.controls.forEach { control ->
            assertNotEquals(
                "${control.id} shows a protocol name the GameCube shell does not print",
                "ZR",
                control.visual.label,
            )
        }
    }

    // ------------------------------------------------- optional GL/GR controls

    /**
     * Supporting a control and placing one are separate claims, and this pins
     * both halves for the Pro Controller 2 grips.
     *
     * The personality HAS them -- they are real inputs on that controller, and
     * bridge contract 4 is what finally gave the companion a way to send them --
     * so the profile binds them and the editor can offer them. The shipped
     * layout does not USE them, so nothing about the default changes.
     */
    @Test fun `Pro2 supports GL and GR without placing them in the default layout`() {
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)

        assertTrue("advertised as addable", pro2.outputs.containsAll(setOf(
            TouchOutputControl.GL, TouchOutputControl.GR,
        )))
        assertEquals(
            TouchControlAction.Logical(ControllerButton.GL),
            pro2.bindings.getValue(TouchOutputControl.GL),
        )
        assertEquals(
            TouchControlAction.Logical(ControllerButton.GR),
            pro2.bindings.getValue(TouchOutputControl.GR),
        )

        // Present in the CATALOG, and marked as not part of the shipped layout.
        val gl = pro2.defaultTemplate.control(TouchOutputControl.GL)
        val gr = pro2.defaultTemplate.control(TouchOutputControl.GR)
        assertFalse("GL is optional", gl.inDefaultLayout)
        assertFalse("GR is optional", gr.inDefaultLayout)
        assertEquals("GL", gl.visual.label)
        assertEquals("GR", gr.visual.label)
        assertEquals(TouchControlKind.Button, gl.interaction)
        assertEquals(TouchControlKind.Button, gr.interaction)

        // And absent from what a fresh install actually draws.
        val shipped = TouchLayoutComposer.compose(pro2).layout
        assertTrue(
            "the default layout must be unchanged",
            shipped.controls.none {
                it.output == TouchOutputControl.GL || it.output == TouchOutputControl.GR
            },
        )
        assertEquals(
            "byte-for-byte the shipped V1 layout",
            TouchLayoutV1.layout.controls,
            shipped.controls.map { it.copy(zIndex = 0) },
        )
    }

    /**
     * Added independently, exactly like any other override, and each one alone.
     * A layout that gained a control genuinely IS customized, so the override is
     * stored rather than dropped -- which is also what makes Reset able to take
     * it away again.
     */
    @Test fun `GL and GR can each be added on their own`() {
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)

        listOf(
            TouchLayoutV1.GRIP_LEFT to TouchOutputControl.GL,
            TouchLayoutV1.GRIP_RIGHT to TouchOutputControl.GR,
        ).forEach { (id, output) ->
            val added = TouchLayoutEditor.add(authored(pro2), pro2, id, 0.5f, 0.5f).document
            assertNotNull("adding is a real customization", added.instance(id))
            val layout = TouchLayoutComposer.compose(pro2, added).layout
            val control = layout.controls.single { it.output == output }
            assertEquals(id, control.id)
            assertEquals(if (output == TouchOutputControl.GL) "GL" else "GR", control.label)
            assertEquals(
                "the added control publishes the real grip input",
                pro2.bindings.getValue(output),
                control.action,
            )
            // Only that one appeared; the other stays optional.
            assertEquals(
                TouchLayoutV1.layout.controls.size + 1,
                layout.controls.size,
            )
        }
    }

    /**
     * What the editor's Add Control picker is built from.
     *
     * The CATALOG, unconditionally — not "what is missing from the layout".
     * Default layout membership is not personality capability, so every control
     * this personality has stays addable however many of it are already on
     * screen. That is what makes duplicates possible at all.
     */
    @Test fun `the add-control picker offers the whole catalog, always`() {
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
        val fresh = authored(pro2)

        // Everything the shipped layout places, plus the two it does not.
        assertEquals(
            pro2.catalog.mapTo(mutableSetOf()) { it.id },
            (
                fresh.controls.mapTo(mutableSetOf()) { it.catalogId } +
                    setOf(TouchLayoutV1.GRIP_LEFT, TouchLayoutV1.GRIP_RIGHT)
                ),
        )
        assertEquals(
            setOf(TouchLayoutV1.GRIP_LEFT, TouchLayoutV1.GRIP_RIGHT),
            pro2.catalog.filterNot { it.inDefaultLayout }.mapTo(mutableSetOf()) { it.id },
        )

        // Adding one that IS already on screen is legal and produces a second
        // instance with its own identity and the same binding.
        val twice = TouchLayoutEditor.add(fresh, pro2, TouchLayoutV1.HOME, 0.5f, 0.5f)
        val second = twice.created.single()
        assertNotEquals(TouchLayoutV1.HOME, second)
        val layout = TouchLayoutComposer.compose(pro2, twice.document).layout
        assertEquals(2, layout.controls.count { it.output == TouchOutputControl.Home })
        assertEquals(
            layout.controls.first { it.id == TouchLayoutV1.HOME }.action,
            layout.controls.first { it.id == second }.action,
        )
    }

    /** Deleting an added grip returns the layout to the shipped default exactly. */
    @Test fun `deleting an added GL restores the default layout`() {
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
        val added = TouchLayoutEditor.add(
            authored(pro2), pro2, TouchLayoutV1.GRIP_LEFT, 0.5f, 0.5f,
        ).document
        assertEquals(
            1,
            TouchLayoutComposer.compose(pro2, added).layout.controls.size -
                TouchLayoutV1.layout.controls.size,
        )

        val removed = TouchLayoutEditor.delete(
            added, setOf(TouchLayoutV1.GRIP_LEFT), editGroup = false,
        ).document
        // Structurally back to the shipped document, so the profile reports
        // itself as untouched and Reset genuinely has nothing left to do.
        assertEquals(authored(pro2), removed)
        assertEquals(
            TouchLayoutV1.layout.controls,
            TouchLayoutComposer.compose(pro2, removed).layout.controls.map { it.copy(zIndex = 0) },
        )
    }

    /**
     * A layout that never mentions the grips keeps them off screen.
     *
     * Absent from the document means absent from the layout — there is no
     * template ghost that could reappear, which is exactly the property the
     * instance model was adopted for.
     */
    @Test fun `a saved layout that predates the grips loads unchanged`() {
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2)
        val saved = TouchLayoutEditor.setScale(
            authored(pro2), setOf(TouchLayoutV1.HOME), 1.2f, editGroup = false,
        )
        val result = TouchLayoutComposer.compose(pro2, saved)
        assertTrue(result.customized)
        assertNull(result.warning)
        assertEquals(TouchLayoutV1.layout.controls.size, result.layout.controls.size)
        assertTrue(result.layout.controls.none { it.output == TouchOutputControl.GL })
    }

    /** No other personality has grips, so none of them may offer these. */
    @Test fun `only Pro2 exposes the grip buttons`() {
        TouchProfileCatalog.profiles.forEach { (id, profile) ->
            if (id == TouchProfileId.Pro2) return@forEach
            assertFalse("$id offers GL", profile.outputs.contains(TouchOutputControl.GL))
            assertFalse("$id offers GR", profile.outputs.contains(TouchOutputControl.GR))
            assertTrue(
                "$id placed a grip control",
                profile.defaultTemplate.controls.none {
                    it.output == TouchOutputControl.GL || it.output == TouchOutputControl.GR
                },
            )
        }
    }

    /**
     * The other personalities own `ZR` outright and are untouched by any of the
     * GameCube Z work: their `ZR` is a real, separately labelled control bound to
     * the real `R2`, never to the shoulder GameCube mode reroutes.
     */
    @Test fun `no other personality routes ZR through the GameCube Z path`() {
        TouchProfileCatalog.profiles.forEach { (id, profile) ->
            if (id == TouchProfileId.GameCube) return@forEach
            val zr = profile.bindings[TouchOutputControl.ZR] ?: return@forEach
            assertEquals(
                "$id's ZR must stay a real ZR",
                TouchControlAction.Trigger(ControlSide.Right, analog = false),
                zr,
            )
        }
    }

    @Test fun `Pro2 and Joy-Con Right diamonds remain square at every aspect ratio`() {
        val cases = listOf(
            TouchProfileId.Pro2 to listOf(
                TouchOutputControl.FaceNorth, TouchOutputControl.FaceEast,
                TouchOutputControl.FaceSouth, TouchOutputControl.FaceWest,
            ),
            TouchProfileId.JoyConRight to listOf(
                TouchOutputControl.X, TouchOutputControl.A,
                TouchOutputControl.B, TouchOutputControl.Y,
            ),
        )
        cases.forEach { (profileId, outputs) ->
            val profile = TouchProfileCatalog.require(profileId)
            val layout = TouchLayoutComposer.compose(profile).layout
            listOf(800f to 400f, 915f to 412f, 1024f to 768f).forEach { (width, height) ->
                val resolved = TouchLayoutResolver.resolve(
                    layout, profileRegion(width, height), TouchLayoutAuditMode.ShippedTemplate,
                )
                // Selected by where they LAND, not by what they are called. A
                // sideways Joy-Con's diamond is the same square turned a quarter
                // turn, so naming the members by output would make a squareness
                // test fail on a correct rotation; orientation is pinned
                // separately, by `TouchSidewaysJoyConTest`.
                val members = outputs.map { output ->
                    resolved.controls.single { it.spec.output == output }
                }
                val north = members.minBy { it.centerY }
                val south = members.maxBy { it.centerY }
                val west = members.minBy { it.centerX }
                val east = members.maxBy { it.centerX }
                val centerX = (east.centerX + west.centerX) / 2f
                val centerY = (north.centerY + south.centerY) / 2f
                val horizontalRadius = east.centerX - centerX
                val verticalRadius = south.centerY - centerY
                assertEquals("$profileId centre x", centerX, north.centerX, 1e-4f)
                assertEquals("$profileId centre y", centerY, east.centerY, 1e-4f)
                assertEquals("$profileId square radius at ${width}x$height", horizontalRadius, verticalRadius, 1e-4f)
                assertEquals(north.halfWidth, east.halfWidth, 1e-4f)
                assertEquals(north.halfWidth, south.halfWidth, 1e-4f)
                assertEquals(north.halfWidth, west.halfWidth, 1e-4f)
            }
        }
    }

    @Test fun `GameCube preserves the Pro2 composition with its mirrored lower pair`() {
        val template = TouchProfileCatalog.require(TouchProfileId.GameCube).defaultTemplate
        val pro2 = TouchProfileCatalog.require(TouchProfileId.Pro2).defaultTemplate
        val a = template.control(TouchOutputControl.A)
        val b = template.control(TouchOutputControl.B)
        val x = template.control(TouchOutputControl.X)
        val y = template.control(TouchOutputControl.Y)
        assertTrue(a.geometry.widthUnits > b.geometry.widthUnits)
        assertTrue(a.geometry.heightUnits > x.geometry.widthUnits)
        assertTrue(b.geometry.referenceX < a.geometry.referenceX && b.geometry.referenceY > a.geometry.referenceY)
        assertTrue(x.geometry.referenceX > a.geometry.referenceX)
        assertTrue(y.geometry.referenceX < a.geometry.referenceX && y.geometry.referenceY < a.geometry.referenceY)
        assertTrue(x.geometry.heightUnits > x.geometry.widthUnits)
        assertTrue(y.geometry.widthUnits > y.geometry.heightUnits)
        assertEquals(52f / 84f, x.geometry.widthUnits / x.geometry.heightUnits, 1e-6f)
        assertEquals(84f / 54f, y.geometry.widthUnits / y.geometry.heightUnits, 1e-6f)
        assertEquals(TouchVisualRole.GameCubeLargeA, a.visual.role)
        assertEquals(TouchVisualRole.GameCubeSmallB, b.visual.role)
        assertEquals(TouchVisualRole.GameCubeBeanX, x.visual.role)
        assertEquals(TouchVisualRole.GameCubeBeanY, y.visual.role)
        // The source contours' concavity vectors are 165.58 degrees (X) and
        // 71.88 degrees (Y). Template rotation points each one directly at A.
        fun directionDegrees(from: TouchTemplateControl, to: TouchTemplateControl): Float =
            Math.toDegrees(
                kotlin.math.atan2(
                    (to.geometry.referenceY - from.geometry.referenceY).toDouble(),
                    (to.geometry.referenceX - from.geometry.referenceX).toDouble(),
                ),
            ).toFloat()
        assertEquals(directionDegrees(x, a), 165.58f + x.visual.rotationDegrees, 0.15f)
        assertEquals(directionDegrees(y, a), 71.88f + y.visual.rotationDegrees, 0.15f)
        val faceScale = a.geometry.widthUnits / 88f
        assertEquals(1.00963f, faceScale, 1e-6f)
        assertEquals(77f * faceScale, x.geometry.groupOffsetXUnits - a.geometry.groupOffsetXUnits, 1e-5f)
        assertEquals(-5f * faceScale, x.geometry.groupOffsetYUnits - a.geometry.groupOffsetYUnits, 1e-5f)
        assertEquals(-37.5f * faceScale, y.geometry.groupOffsetXUnits - a.geometry.groupOffsetXUnits, 1e-5f)
        assertEquals(-67.25f * faceScale, y.geometry.groupOffsetYUnits - a.geometry.groupOffsetYUnits, 1e-5f)
        assertEquals(-68.5f * faceScale, b.geometry.groupOffsetXUnits - a.geometry.groupOffsetXUnits, 1e-5f)
        assertEquals(57.5f * faceScale, b.geometry.groupOffsetYUnits - a.geometry.groupOffsetYUnits, 1e-5f)
        val faceGaps = listOf(b, x, y).map { it.testVisualGapFrom(a) }
        assertTrue(
            "GameCube face gaps are not balanced: $faceGaps",
            faceGaps.max() - faceGaps.min() <= 0.3f,
        )

        val main = template.control(TouchOutputControl.PrimaryStick)
        val cStick = template.control(TouchOutputControl.SecondaryStick)
        val dpad = template.control(TouchOutputControl.Dpad)
        assertEquals(TouchVisualRole.AnalogStick, main.visual.role)
        assertEquals(TouchVisualRole.AnalogStick, cStick.visual.role)
        assertEquals(TouchVisualRole.UnifiedDpad, dpad.visual.role)
        assertEquals(140f, main.geometry.widthUnits, 0f)
        assertEquals(140f, cStick.geometry.widthUnits, 0f)
        assertEquals(0f, main.geometry.hitMarginUnits, 0f)
        assertEquals(0f, cStick.geometry.hitMarginUnits, 0f)
        val pro2Main = pro2.control(TouchOutputControl.PrimaryStick)
        val pro2Dpad = pro2.control(TouchOutputControl.Dpad)
        val pro2Right = pro2.control(TouchOutputControl.SecondaryStick)
        assertEquals(pro2Main.geometry.referenceX, main.geometry.referenceX, 0f)
        assertEquals(pro2Main.geometry.referenceY, main.geometry.referenceY, 0f)
        assertEquals(pro2Dpad.geometry.referenceY, dpad.geometry.referenceY, 0f)
        assertEquals(pro2Right.geometry.referenceY, cStick.geometry.referenceY, 0f)

        val faceBounds = listOf(a, b, x, y).compoundVisualBounds()
        assertEquals(
            "approved GameCube compound face centre x",
            682.737f,
            faceBounds.centerX,
            0.05f,
        )
        assertEquals(
            "approved GameCube compound face centre y",
            163.02f,
            faceBounds.centerY,
            0.05f,
        )
        assertEquals(main.geometry.widthUnits, dpad.geometry.widthUnits, 0f)
        assertEquals(main.geometry.heightUnits, dpad.geometry.heightUnits, 0f)
        assertEquals(main.geometry.hitMarginUnits, dpad.geometry.hitMarginUnits, 0f)
        val plus = template.control(TouchOutputControl.Plus)
        assertFalse(template.controls.any { it.output == TouchOutputControl.Minus })
        assertEquals(
            TouchLayoutResolver.REFERENCE_WIDTH_UNITS / 2f,
            plus.geometry.referenceX,
            0f,
        )

        val centerX = TouchLayoutResolver.REFERENCE_WIDTH_UNITS / 2f
        assertEquals(dpad.geometry.widthUnits, cStick.geometry.widthUnits, 0f)
        assertEquals(dpad.geometry.heightUnits, cStick.geometry.heightUnits, 0f)
        assertEquals(dpad.geometry.hitMarginUnits, cStick.geometry.hitMarginUnits, 0f)
        assertEquals(dpad.geometry.referenceY, cStick.geometry.referenceY, 0f)
        assertEquals(centerX, (dpad.geometry.referenceX + cStick.geometry.referenceX) / 2f, 0f)
        assertTrue(centerX - dpad.geometry.referenceX < centerX - pro2Dpad.geometry.referenceX)
        assertEquals(dpad.editGroupId, cStick.editGroupId)
        assertNotNull(dpad.editGroupId)

        val topUtilities = listOf(
            template.control(TouchOutputControl.Capture),
            template.control(TouchOutputControl.Home),
            template.control(TouchOutputControl.C),
        )
        assertEquals(centerX, topUtilities[1].geometry.referenceX, 0f)
        assertEquals(
            topUtilities[1].geometry.referenceX - topUtilities[0].geometry.referenceX,
            topUtilities[2].geometry.referenceX - topUtilities[1].geometry.referenceX,
            0f,
        )
        assertEquals(1, topUtilities.map { it.geometry.referenceY }.distinct().size)
        assertEquals(1, topUtilities.map { it.geometry.widthUnits }.distinct().size)
        assertEquals(1, topUtilities.map { it.editGroupId }.distinct().size)
        assertNotNull(topUtilities.first().editGroupId)
    }

    @Test fun `approved GameCube geometry is the shipped no-override default`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val composed = TouchLayoutComposer.compose(profile, null)
        assertFalse(composed.customized)
        assertNull(composed.warning)

        // Actual tablet interaction region used for the approved gc-final render.
        // These assertions protect the immutable template itself so a persisted
        // editor override can never mask a default-layout regression again.
        val resolved = TouchLayoutResolver.resolve(
            composed.layout,
            TouchLayoutRegion(
                left = 50f,
                top = 60f,
                right = 1474f,
                bottom = 947f,
                unitScale = 267f / 160f,
            ),
            TouchLayoutAuditMode.ShippedTemplate,
        )
        assertTrue(resolved.problem.orEmpty(), resolved.fits)
        val controls = resolved.controls.associateBy { it.spec.output }

        data class ApprovedControl(
            val output: TouchOutputControl,
            val centerX: Float,
            val centerY: Float,
            val halfWidth: Float,
            val halfHeight: Float,
        )
        listOf(
            ApprovedControl(TouchOutputControl.A, 1260.38f, 433.08f, 79.07f, 79.07f),
            ApprovedControl(TouchOutputControl.B, 1137.28f, 536.42f, 50.32f, 50.32f),
            ApprovedControl(TouchOutputControl.X, 1398.76f, 424.10f, 46.73f, 75.48f),
            ApprovedControl(TouchOutputControl.Y, 1192.99f, 312.23f, 75.48f, 48.52f),
        ).forEach { approved ->
            val actual = controls.getValue(approved.output)
            assertEquals("${approved.output} center x", approved.centerX, actual.centerX, 0.02f)
            assertEquals("${approved.output} center y", approved.centerY, actual.centerY, 0.05f)
            assertEquals("${approved.output} half width", approved.halfWidth, actual.halfWidth, 0.02f)
            assertEquals("${approved.output} half height", approved.halfHeight, actual.halfHeight, 0.02f)
        }
    }

    @Test fun `Joy-Con sticks and four-button clusters share a visual centerline`() {
        val cases = listOf(
            TouchProfileId.JoyConLeft to listOf(
                TouchOutputControl.DirectionUp,
                TouchOutputControl.DirectionLeft,
                TouchOutputControl.DirectionRight,
                TouchOutputControl.DirectionDown,
            ),
            TouchProfileId.JoyConRight to listOf(
                TouchOutputControl.X,
                TouchOutputControl.Y,
                TouchOutputControl.A,
                TouchOutputControl.B,
            ),
        )

        cases.forEach { (profileId, clusterOutputs) ->
            val template = TouchProfileCatalog.require(profileId).defaultTemplate
            val stick = template.control(TouchOutputControl.PrimaryStick)
            val clusterBounds = clusterOutputs.map(template::control).compoundVisualBounds()
            assertEquals(
                "$profileId primary controls centerline",
                stick.geometry.referenceY,
                clusterBounds.centerY,
                0.01f,
            )
            assertEquals(
                "$profileId primary controls edge margins",
                stick.testVisualBounds().left,
                TouchLayoutResolver.REFERENCE_WIDTH_UNITS - clusterBounds.right,
                0.01f,
            )
        }
    }

    @Test fun `joystick authored sizes and hit bounds remain unchanged`() {
        data class Expected(val profile: TouchProfileId, val output: TouchOutputControl, val size: Float)
        val expected = listOf(
            Expected(TouchProfileId.Pro2, TouchOutputControl.PrimaryStick, 146f),
            Expected(TouchProfileId.Pro2, TouchOutputControl.SecondaryStick, 146f),
            Expected(TouchProfileId.GameCube, TouchOutputControl.PrimaryStick, 140f),
            Expected(TouchProfileId.GameCube, TouchOutputControl.SecondaryStick, 140f),
            Expected(TouchProfileId.JoyConLeft, TouchOutputControl.PrimaryStick, 150f),
            Expected(TouchProfileId.JoyConRight, TouchOutputControl.PrimaryStick, 150f),
        )
        expected.forEach { case ->
            val geometry = TouchProfileCatalog.require(case.profile).defaultTemplate
                .control(case.output).geometry
            assertEquals("${case.profile} ${case.output} width", case.size, geometry.widthUnits, 0f)
            assertEquals("${case.profile} ${case.output} height", case.size, geometry.heightUnits, 0f)
            assertEquals("${case.profile} ${case.output} hit margin", 0f, geometry.hitMarginUnits, 0f)
        }
    }

    @Test fun `GameCube Y wraps A visually without creating an ambiguous touch region`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val layout = TouchLayoutComposer.compose(profile).layout
        val region = TouchLayoutRegion(50f, 60f, 1474f, 947f, 267f / 160f)
        val resolved = TouchLayoutResolver.resolve(layout, region, TouchLayoutAuditMode.ShippedTemplate)
        val a = resolved.controls.single { it.spec.output == TouchOutputControl.A }
        val y = resolved.controls.single { it.spec.output == TouchOutputControl.Y }

        assertTrue(kotlin.math.abs(a.centerX - y.centerX) < a.hitHalfWidth + y.hitHalfWidth)
        assertTrue(kotlin.math.abs(a.centerY - y.centerY) < a.hitHalfHeight + y.hitHalfHeight)
        assertTrue(
            resolved.problem ?: "shape-aware contour audit should accept the intentional box intersection",
            resolved.fits,
        )
        assertFalse(
            TouchLayoutAudit.audit(layout, resolved.controls, region, profile, TouchLayoutAuditMode.ShippedTemplate)
                .any { it.message.contains("'a'") && it.message.contains("'y'") },
        )
    }

    @Test fun `Joy-Con orientation trigger order direction art and stick labels are fixed`() {
        val left = TouchProfileCatalog.require(TouchProfileId.JoyConLeft).defaultTemplate
        val right = TouchProfileCatalog.require(TouchProfileId.JoyConRight).defaultTemplate
        fun topOrder(template: TouchLayoutTemplate, expected: List<TouchOutputControl>) {
            val actual = expected.map(template::control).sortedBy { it.geometry.referenceX }.map { it.output }
            assertEquals(expected, actual)
        }
        topOrder(
            left,
            listOf(TouchOutputControl.SL, TouchOutputControl.L, TouchOutputControl.ZL, TouchOutputControl.SR),
        )
        topOrder(
            right,
            listOf(TouchOutputControl.SL, TouchOutputControl.R, TouchOutputControl.ZR, TouchOutputControl.SR),
        )
        assertEquals("L3", left.control(TouchOutputControl.PrimaryStickClick).visual.label)
        assertEquals("R3", right.control(TouchOutputControl.PrimaryStickClick).visual.label)

        val leftStick = left.control(TouchOutputControl.PrimaryStick)
        val leftDirections = listOf(
            TouchOutputControl.DirectionUp, TouchOutputControl.DirectionLeft,
            TouchOutputControl.DirectionRight, TouchOutputControl.DirectionDown,
        ).map(left::control)
        assertTrue(leftStick.geometry.referenceX < leftDirections.map { it.geometry.referenceX }.average())
        leftDirections.forEach {
            assertEquals(TouchVisualRole.JoyConDirectionButton, it.visual.role)
            assertEquals("", it.visual.label)
        }
        assertTrue(
            right.control(TouchOutputControl.PrimaryStick).geometry.referenceX <
                right.control(TouchOutputControl.A).geometry.referenceX,
        )
    }

    @Test fun `Pro2 left stick and D-pad are swapped with equal complete footprints`() {
        val template = TouchProfileCatalog.require(TouchProfileId.Pro2).defaultTemplate
        val dpad = template.control(TouchOutputControl.Dpad)
        val leftStick = template.control(TouchOutputControl.PrimaryStick)
        val rightStick = template.control(TouchOutputControl.SecondaryStick)
        assertEquals(TouchVisualRole.UnifiedDpad, dpad.visual.role)
        assertEquals(100f, leftStick.geometry.referenceX, 0f)
        assertEquals(164f, leftStick.geometry.referenceY, 0f)
        assertEquals(216f, dpad.geometry.referenceX, 1e-4f)
        assertEquals(312f, dpad.geometry.referenceY, 1e-4f)
        assertEquals(leftStick.geometry.widthUnits, dpad.geometry.widthUnits, 0f)
        assertEquals(rightStick.geometry.widthUnits, dpad.geometry.widthUnits, 0f)
        assertEquals(leftStick.geometry.hitMarginUnits, dpad.geometry.hitMarginUnits, 0f)
        val controls = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(TouchProfileCatalog.require(TouchProfileId.Pro2)).layout,
            profileRegion(800f, 400f),
            TouchLayoutAuditMode.ShippedTemplate,
        ).controls
        val resolvedDpad = controls.single { it.spec.output == TouchOutputControl.Dpad }
        val resolvedStick = controls.single { it.spec.output == TouchOutputControl.PrimaryStick }
        assertEquals(resolvedStick.halfWidth, resolvedDpad.halfWidth, 0f)
        assertEquals(resolvedStick.hitHalfWidth, resolvedDpad.hitHalfWidth, 0f)
    }

    @Test fun `shared face golden fixture exactly covers catalog labels and HID usages`() {
        val rows = Files.readAllLines(touchFaceFixture()).filterNot { it.isBlank() || it.startsWith("#") }
            .map { line ->
                val fields = line.split(',')
                assertEquals("fixture field count: $line", 8, fields.size)
                TouchFaceGolden(
                    profile = fields[0],
                    template = fields[1],
                    presentation = fields[2],
                    controlId = fields[3],
                    label = fields[4],
                    usage = fields[5].toInt(),
                )
            }

        val expectedKeys = TouchProfileCatalog.profiles.values.flatMap { profile ->
            val outputs = when (profile.id) {
                TouchProfileId.Pro2 -> setOf(
                    TouchOutputControl.FaceSouth, TouchOutputControl.FaceEast,
                    TouchOutputControl.FaceWest, TouchOutputControl.FaceNorth,
                )
                TouchProfileId.GameCube, TouchProfileId.JoyConRight -> setOf(
                    TouchOutputControl.A, TouchOutputControl.B,
                    TouchOutputControl.X, TouchOutputControl.Y,
                )
                TouchProfileId.JoyConLeft -> setOf(
                    TouchOutputControl.DirectionUp, TouchOutputControl.DirectionLeft,
                    TouchOutputControl.DirectionRight, TouchOutputControl.DirectionDown,
                )
            }
            val presentations = if (profile.id == TouchProfileId.Pro2) {
                listOf("nintendo", "xbox")
            } else listOf("fixed")
            profile.defaultTemplate.controls.filter { it.output in outputs }.flatMap { control ->
                presentations.map { presentation ->
                    TouchFaceGoldenKey(profile.id.key, profile.defaultTemplate.id, presentation, control.id)
                }
            }
        }.toSet()
        assertEquals(expectedKeys, rows.map { it.key }.toSet())
        assertEquals(expectedKeys.size, rows.size)

        rows.forEach { row ->
            val profileId = requireNotNull(TouchProfileId.fromKey(row.profile))
            val profile = TouchProfileCatalog.require(profileId)
            val control = requireNotNull(profile.defaultTemplate.controls.firstOrNull { it.id == row.controlId })
            val action = profile.bindings.getValue(control.output)
            val presentation = when (row.presentation) {
                "nintendo" -> ControllerFaceLayout.Nintendo
                "xbox", "fixed" -> ControllerFaceLayout.Xbox
                else -> error("Unknown presentation ${row.presentation}")
            }
            val button = when (action) {
                is TouchControlAction.Face -> ControllerLayoutResolver.mapTouchFacePosition(
                    action.position.positional,
                    presentation,
                )
                is TouchControlAction.Logical -> action.button
                else -> error("Face golden ${row.key} does not bind to a button")
            }
            val label = when (action) {
                is TouchControlAction.Face -> ControllerLayoutResolver.faceLabel(action.position, presentation)
                else -> if (control.visual.role == TouchVisualRole.JoyConDirectionButton) {
                    when (control.output) {
                        TouchOutputControl.DirectionUp -> "triangle-up"
                        TouchOutputControl.DirectionLeft -> "triangle-left"
                        TouchOutputControl.DirectionRight -> "triangle-right"
                        TouchOutputControl.DirectionDown -> "triangle-down"
                        else -> error("${control.id} has a direction role without a direction output")
                    }
                } else control.visual.label
            }
            assertEquals("${row.key} label", row.label, label)
            assertEquals("${row.key} Android HID usage", row.usage, button.ordinal + 1)
        }
    }
}

private data class TouchFaceGoldenKey(
    val profile: String,
    val template: String,
    val presentation: String,
    val controlId: String,
)

private data class TouchFaceGolden(
    val profile: String,
    val template: String,
    val presentation: String,
    val controlId: String,
    val label: String,
    val usage: Int,
) {
    val key = TouchFaceGoldenKey(profile, template, presentation, controlId)
}

private fun touchFaceFixture(): Path {
    var cursor: Path? = Path.of("").toAbsolutePath()
    while (cursor != null) {
        val candidate = cursor.resolve("tools/fixtures/touch_face_mapping.csv")
        if (Files.isRegularFile(candidate)) return candidate
        cursor = cursor.parent
    }
    error("Cannot find tools/fixtures/touch_face_mapping.csv from ${Path.of("").toAbsolutePath()}")
}

class TouchLayoutOverrideTest {
    private val profile = TouchProfileCatalog.require(TouchProfileId.Pro2)

    @Test fun `codec round trips a sparse override deterministically`() {
        val value = TouchLayoutOverride(
            profileId = profile.id,
            templateId = profile.defaultTemplate.id,
            basedOnRevision = profile.defaultTemplate.templateRevision,
            controls = linkedMapOf(
                "future-control" to TouchControlOverride(visible = false),
                TouchLayoutV1.STICK_LEFT to TouchControlOverride(
                    anchorX = 0.31f,
                    anchorY = 0.72f,
                    scale = 1.2f,
                    visible = true,
                    groupOffsetScale = 1.1f,
                ),
            ),
        )
        val encoded = TouchLayoutOverrideJsonCodec.encode(value)
        val decoded = TouchLayoutOverrideJsonCodec.decode(encoded)
        assertEquals(TouchOverrideDecodeResult.Valid(value), decoded)
        assertEquals(encoded, TouchLayoutOverrideJsonCodec.encode((decoded as TouchOverrideDecodeResult.Valid).value))
    }

    @Test fun `invalid and future documents fail without inventing a layout`() {
        assertTrue(TouchLayoutOverrideJsonCodec.decode("not json") is TouchOverrideDecodeResult.Invalid)
        assertTrue(
            TouchLayoutOverrideJsonCodec.decode(
                """{"schemaVersion":99,"profileId":"pro2","templateId":"x","basedOnRevision":1,"controls":{}}""",
            ) is TouchOverrideDecodeResult.Invalid,
        )
        assertTrue(
            TouchLayoutOverrideJsonCodec.decode(
                """{"schemaVersion":1,"profileId":"pro2","templateId":"x","basedOnRevision":1,"controls":{"a":{"visible":{}}}}""",
            ) is TouchOverrideDecodeResult.Invalid,
        )
        assertTrue(
            TouchLayoutOverrideJsonCodec.decode(
                """{"schemaVersion":1,"profileId":"pro2","templateId":"x","basedOnRevision":1,"controls":{"a":{"scale":0.1}}}""",
            ) is TouchOverrideDecodeResult.Invalid,
        )
        assertTrue(
            TouchLayoutOverrideJsonCodec.decode(
                """{"schemaVersion":1,"profileId":"pro2","templateId":"x","basedOnRevision":1,"controls":{"a":{"groupOffsetScale":9}}}""",
            ) is TouchOverrideDecodeResult.Invalid,
        )
    }

    @Test fun `composition applies known instances and drops ones with no catalog entry`() {
        val document = authored(profile).let { base ->
            base.copy(
                controls = TouchLayoutEditor.place(
                    base, setOf(TouchLayoutV1.FACE_SOUTH), 0.82f, 0.4f,
                ).controls + TouchControlInstance(
                    instanceId = "ghost",
                    catalogId = "removed-by-template-v2",
                    anchorX = 0.1f,
                    anchorY = 0.1f,
                ),
            )
        }
        val result = TouchLayoutComposer.compose(profile, document)
        assertTrue(result.customized)
        assertTrue(result.degraded)
        assertEquals(
            0.82f,
            result.layout.controls.single { it.id == TouchLayoutV1.FACE_SOUTH }.anchorX,
        )
        assertFalse(result.layout.controls.any { it.id == "ghost" })
        // Everything else survived: one dangling reference costs one control.
        assertEquals(
            TouchLayoutV1.layout.controls.size,
            result.layout.controls.size,
        )
    }

    @Test fun `an older template revision applies by stable id and new defaults survive`() {
        val revisedProfile = profile.copy(
            defaultTemplate = profile.defaultTemplate.copy(templateRevision = 2),
        )
        val old = TouchLayoutEditor.setScale(
            authored(profile), setOf(TouchLayoutV1.HOME), 1.1f, editGroup = false,
        ).copy(basedOnRevision = 1)
        val result = TouchLayoutComposer.compose(revisedProfile, old)
        assertTrue(result.customized)
        // The catalog's optional controls stay off screen: a document that never
        // instantiated them cannot make them appear.
        assertEquals(
            profile.defaultTemplate.controls.count { it.inDefaultLayout },
            result.layout.controls.size,
        )
        assertEquals(
            1.1f * 54f,
            result.layout.controls.single { it.id == TouchLayoutV1.HOME }.widthUnits,
            1e-4f,
        )
    }

    @Test fun `a document from a future template revision cannot alter current defaults`() {
        val future = TouchLayoutEditor.setScale(
            authored(profile), setOf(TouchLayoutV1.HOME), 1.4f, editGroup = false,
        ).copy(basedOnRevision = profile.defaultTemplate.templateRevision + 1)
        val result = TouchLayoutComposer.compose(profile, future)
        assertFalse(result.customized)
        assertNotNull(result.warning)
        assertEquals(
            TouchLayoutV1.layout.controls,
            result.layout.controls.map { it.copy(zIndex = 0) },
        )
    }

    @Test fun `profile or template mismatch falls back to immutable default`() {
        val wrong = authored(profile).copy(templateId = "another-template")
        val result = TouchLayoutComposer.compose(profile, wrong)
        assertFalse(result.customized)
        assertNotNull(result.warning)
        assertEquals(
            TouchLayoutV1.layout.controls,
            result.layout.controls.map { it.copy(zIndex = 0) },
        )
    }

    @Test fun `deleted controls have no runtime geometry and only warn in a user draft`() {
        val hidden = TouchLayoutEditor.delete(
            authored(profile), setOf(TouchLayoutV1.HOME), editGroup = false,
        ).document
        val composed = TouchLayoutComposer.compose(profile, hidden).layout
        assertNull(composed.controls.firstOrNull { it.id == TouchLayoutV1.HOME })
        val resolved = TouchLayoutResolver.resolve(
            composed,
            profileRegion(),
            TouchLayoutAuditMode.UserDraft,
        )
        assertTrue(resolved.fits)
        val findings = TouchLayoutAudit.audit(
            composed,
            resolved.controls,
            resolved.region,
            profile,
            TouchLayoutAuditMode.UserDraft,
        )
        assertTrue(findings.any { !it.blocking && it.message.contains("Home", ignoreCase = true) })
    }

    @Test fun `editor moves and scales a group without changing its bindings`() {
        val base = authored(profile)
        val layout = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile, base).layout,
            profileRegion(),
            TouchLayoutAuditMode.UserDraft,
        )
        val moved = TouchLayoutEditor.move(
            base, layout, setOf(TouchLayoutV1.FACE_SOUTH), 8f, -16f, editGroup = true,
        )
        val scaled = TouchLayoutEditor.scaleBy(
            moved, layout, setOf(TouchLayoutV1.FACE_SOUTH), 1.15f, editGroup = true,
        )
        val faceIds = profile.catalog
            .filter { it.editGroupId == "face-cluster" }
            .mapTo(mutableSetOf()) { it.id }
        assertEquals(
            faceIds,
            base.controls.filter { scaled.instance(it.instanceId) != it }
                .mapTo(mutableSetOf()) { it.instanceId },
        )
        assertTrue(faceIds.all { scaled.instance(it)?.scale == 1.15f })

        val composed = TouchLayoutComposer.compose(profile, scaled).layout
        composed.controls.filter { it.id in faceIds }.forEach { control ->
            assertEquals(profile.bindings.getValue(control.output), control.action)
        }
        // Scaling a cluster spreads it: every member's displacement from the
        // shared anchor grew by the same factor, so the diamond stays a diamond.
        faceIds.forEach { id ->
            val before = requireNotNull(base.instance(id))
            val after = requireNotNull(scaled.instance(id))
            assertEquals(before.offsetXUnits * 1.15f, after.offsetXUnits, 1e-3f)
            assertEquals(before.offsetYUnits * 1.15f, after.offsetYUnits, 1e-3f)
        }
        assertEquals(
            base.controls.toSet(),
            TouchLayoutEditor.reset(
                scaled, profile, setOf(TouchLayoutV1.FACE_SOUTH), editGroup = true,
            ).controls.toSet(),
        )
    }

    @Test fun `individual scaling changes button size without moving it inside its group`() {
        val base = TouchLayoutComposer.compose(profile).layout
            .controls.single { it.id == TouchLayoutV1.FACE_EAST }
        val edited = TouchLayoutEditor.setScale(
            authored(profile), setOf(TouchLayoutV1.FACE_EAST), 1.25f, editGroup = false,
        )
        val changed = TouchLayoutComposer.compose(profile, edited).layout.controls
            .single { it.id == TouchLayoutV1.FACE_EAST }
        assertEquals(base.anchorX, changed.anchorX, 0f)
        assertEquals(base.groupOffsetXUnits, changed.groupOffsetXUnits, 0f)
        assertEquals(base.widthUnits * 1.25f, changed.widthUnits, 1e-5f)
    }

    @Test fun `group movement clamps once and preserves relative spacing at an edge`() {
        val base = authored(profile)
        val layout = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile, base).layout,
            profileRegion(),
            TouchLayoutAuditMode.UserDraft,
        )
        val faceIds = profile.catalog
            .filter { it.editGroupId == "face-cluster" }
            .map { it.id }
        val before = faceIds.associateWith { requireNotNull(base.instance(it)).anchorX }
        // Far more than the rectangle can take: the clamp is what is under test.
        val moved = TouchLayoutEditor.move(
            base, layout, setOf(TouchLayoutV1.FACE_EAST), layout.region.width, 0f, editGroup = true,
        )
        val after = faceIds.associateWith { requireNotNull(moved.instance(it)).anchorX }
        faceIds.forEach { a ->
            faceIds.forEach { b ->
                assertEquals(
                    "$a to $b",
                    before.getValue(a) - before.getValue(b),
                    after.getValue(a) - after.getValue(b),
                    1e-6f,
                )
            }
        }
        // The rightmost member ends flush against the safe edge, not past it.
        val east = requireNotNull(
            TouchLayoutResolver.resolve(
                TouchLayoutComposer.compose(profile, moved).layout,
                profileRegion(),
                TouchLayoutAuditMode.UserDraft,
            ).control(TouchLayoutV1.FACE_EAST),
        )
        assertEquals(layout.region.right, east.centerX + east.hitExtentX, 1e-2f)
    }
}
