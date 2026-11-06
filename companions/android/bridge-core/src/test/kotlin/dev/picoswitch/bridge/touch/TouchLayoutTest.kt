package dev.picoswitch.bridge.touch

import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.FaceButtonPosition
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Representative window shapes, expressed the way the layout sees them.
 *
 * These are LAYOUT PROBES, not a device whitelist. Each one is a shape the
 * layout has to survive — a short landscape phone, a tall-aspect handheld, a
 * 16:10 tablet, a 4:3 tablet, a large window, and a narrow freeform one — and
 * the point is that correctness never depends on recognising a device.
 */
private val PROBES = listOf(
    "compact 16:9 landscape" to Pair(640f, 360f),
    "tall-aspect landscape" to Pair(800f, 360f),
    "large phone landscape" to Pair(915f, 412f),
    "small tablet 16:10" to Pair(1024f, 600f),
    "tablet 4:3" to Pair(1024f, 768f),
    "large tablet" to Pair(1280f, 800f),
    "handheld landscape" to Pair(832f, 440f),
)

private fun region(width: Float, height: Float, scale: Float = 1f, left: Float = 0f, top: Float = 0f) =
    TouchLayoutRegion(left, top, left + width * scale, top + height * scale, scale)

class TouchLayoutResolverTest {

    @Test fun `the default layout resolves cleanly at every probe`() {
        PROBES.forEach { (name, size) ->
            val resolved = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(size.first, size.second))
            assertTrue("$name: ${resolved.problem}", resolved.fits)
            assertNull(name, resolved.problem)
            assertEquals(name, TouchLayoutV1.layout.controls.size, resolved.controls.size)
        }
    }

    /**
     * The same probes at a raised display density. Sizes are in logical units, so
     * a denser display must produce the identical arrangement in more pixels —
     * if it does not, something is comparing pixels to units somewhere.
     */
    @Test fun `density does not change the arrangement`() {
        PROBES.forEach { (name, size) ->
            val single = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(size.first, size.second, 1f))
            val dense = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(size.first, size.second, 2.75f))
            assertTrue("$name", dense.fits)
            assertEquals("$name scale", single.scale, dense.scale, 1e-4f)
            single.controls.zip(dense.controls).forEach { (a, b) ->
                assertEquals("$name ${a.id} x", a.centerX * 2.75f, b.centerX, 1e-2f)
                assertEquals("$name ${a.id} size", a.halfWidth * 2.75f, b.halfWidth, 1e-2f)
            }
        }
    }

    /** A layout resolved into an offset rectangle must sit in that rectangle. */
    @Test fun `controls stay inside an inset interaction area`() {
        val resolved = TouchLayoutResolver.resolve(
            TouchLayoutV1.layout,
            TouchLayoutRegion(left = 96f, top = 48f, right = 96f + 832f, bottom = 48f + 420f, unitScale = 1f),
        )
        assertTrue(resolved.problem ?: "", resolved.fits)
        resolved.controls.forEach { control ->
            assertTrue(
                "${control.id} crosses the left edge",
                control.centerX - control.halfWidth >= 96f - 0.5f,
            )
            assertTrue(
                "${control.id} crosses the top edge",
                control.centerY - control.halfHeight >= 48f - 0.5f,
            )
            assertTrue(
                "${control.id} crosses the right edge",
                control.centerX + control.halfWidth <= 96f + 832f + 0.5f,
            )
            assertTrue(
                "${control.id} crosses the bottom edge",
                control.centerY + control.halfHeight <= 48f + 420f + 0.5f,
            )
        }
    }

    /**
     * Past the ergonomic ceiling the extra room becomes GUTTER, not larger
     * buttons. A twelve-inch tablet does not come with larger thumbs.
     */
    @Test fun `controls stop growing while the gaps keep growing`() {
        val modest = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(900f, 460f))
        val huge = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(2400f, 1400f))
        assertEquals(TouchLayoutResolver.MAX_SCALE, huge.scale, 1e-4f)

        val modestFace = modest.control(TouchLayoutV1.FACE_SOUTH)!!
        val hugeFace = huge.control(TouchLayoutV1.FACE_SOUTH)!!
        assertTrue(hugeFace.halfWidth / modestFace.halfWidth < 1.3f)

        fun gap(layout: ResolvedTouchLayout) =
            kotlin.math.abs(
                layout.control(TouchLayoutV1.STICK_LEFT)!!.centerX -
                    layout.control(TouchLayoutV1.DPAD)!!.centerX,
            )
        assertTrue("gutters must absorb the extra room", gap(huge) > gap(modest) * 2f)
    }

    @Test fun `a window too small to play in is refused rather than overlapped`() {
        val portraitPhone = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(412f, 915f))
        assertFalse(portraitPhone.fits)
        assertNotNull(portraitPhone.problem)

        val sliver = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(900f, 180f))
        assertFalse(sliver.fits)
    }

    @Test fun `an unmeasured area is refused`() {
        val zero = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(0f, 0f))
        assertFalse(zero.fits)
        assertTrue(zero.controls.isEmpty())
        assertFalse(ResolvedTouchLayout.Empty.fits)
    }

    @Test fun `hit testing respects the declared shape`() {
        val resolved = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(832f, 440f))
        val face = resolved.control(TouchLayoutV1.FACE_SOUTH)!!
        assertTrue(face.hitTest(face.centerX, face.centerY))
        // The corner of the bounding box is outside a circular control.
        assertFalse(
            face.hitTest(
                face.centerX + face.hitHalfWidth * 0.99f,
                face.centerY + face.hitHalfHeight * 0.99f,
            ),
        )
        val shoulder = resolved.control(TouchLayoutV1.SHOULDER_LEFT)!!
        assertTrue(
            shoulder.hitTest(
                shoulder.centerX + shoulder.hitHalfWidth * 0.99f,
                shoulder.centerY + shoulder.hitHalfHeight * 0.99f,
            ),
        )
    }
}

class TouchLayoutAuditTest {

    @Test fun `the default layout has every control a controller needs`() {
        val resolved = TouchLayoutResolver.resolve(TouchLayoutV1.layout, region(832f, 440f))
        val findings = TouchLayoutAudit.audit(resolved.controls, resolved.region)
        assertEquals("unexpected findings: $findings", emptyList<TouchLayoutFinding>(), findings)
    }

    @Test fun `every face position, both sticks and the D-pad are present`() {
        val actions = TouchLayoutV1.layout.controls.map { it.action }
        FaceButtonPosition.entries.forEach { position ->
            assertTrue(
                "$position missing",
                actions.any { it is TouchControlAction.Face && it.position == position },
            )
        }
        ControlSide.entries.forEach { side ->
            assertTrue(
                "$side stick missing",
                actions.any { it is TouchControlAction.Stick && it.side == side },
            )
            assertTrue(
                "$side trigger missing",
                actions.any { it is TouchControlAction.Trigger && it.side == side },
            )
        }
        assertTrue(actions.any { it is TouchControlAction.Directions })

        val logical = actions.filterIsInstance<TouchControlAction.Logical>().map { it.button }.toSet()
        assertTrue(
            "missing logical actions",
            logical.containsAll(
                listOf(
                    ControllerButton.L1, ControllerButton.R1,
                    ControllerButton.Select, ControllerButton.Start,
                    ControllerButton.LeftStick, ControllerButton.RightStick,
                    ControllerButton.Home, ControllerButton.Capture, ControllerButton.C,
                ),
            ),
        )
    }

    @Test fun `utility controls are rounded squares with fixed portable content`() {
        val capture = TouchLayoutV1.layout.controls.single { it.id == TouchLayoutV1.CAPTURE }
        val home = TouchLayoutV1.layout.controls.single { it.id == TouchLayoutV1.HOME }
        val chat = TouchLayoutV1.layout.controls.single { it.id == TouchLayoutV1.CHAT }

        listOf(capture, home, chat).forEach {
            assertEquals(TouchControlShape.Rectangle, it.shape)
        }
        assertEquals(TouchControlGlyph.Capture, capture.glyph)
        assertEquals("", capture.label)
        assertEquals(TouchControlGlyph.Home, home.glyph)
        assertEquals("", home.label)
        assertNull(chat.glyph)
        assertEquals("C", chat.label)
    }

    @Test fun `overlapping artwork is reported as blocking`() {
        val overlapping = TouchLayout(
            id = "overlap", schemaVersion = 1,
            controls = listOf(
                button("a", ControllerButton.L1, 0.5f, 0.5f, 80f),
                button("b", ControllerButton.R1, 0.52f, 0.5f, 80f),
            ),
        )
        val resolved = TouchLayoutResolver.resolve(overlapping, region(832f, 440f))
        assertFalse(resolved.fits)
        val findings = TouchLayoutAudit.audit(resolved.controls, resolved.region)
        assertTrue(findings.any { it.blocking && it.message.contains("overlap") })
    }

    /**
     * Margins meeting is not artwork colliding.
     *
     * Both controls stay reliably pressable by aiming at what is drawn, so the
     * layout is reported and still played. The shipped GameCube arrangement
     * relies on exactly this between `z` and the `Y` bean.
     */
    @Test fun `touching hit margins are reported without blocking`() {
        // 60-unit circles 66 apart: the drawn discs clear each other by 6 units,
        // and a 6-unit margin each closes that gap exactly.
        val touching = TouchLayout(
            id = "margins", schemaVersion = 1,
            controls = listOf(
                button("a", ControllerButton.L1, 0.40f, 0.5f, 60f, margin = 6f),
                button("b", ControllerButton.R1, 0.40f + 66f / 832f, 0.5f, 60f, margin = 6f),
            ),
        )
        val resolved = TouchLayoutResolver.resolve(touching, region(832f, 440f))
        val findings = TouchLayoutAudit.audit(resolved.controls, resolved.region)
        val overlap = findings.filter { it.message.contains("margin") }
        assertEquals(1, overlap.size)
        assertFalse("margins meeting must not refuse the layout", overlap.single().blocking)
        assertTrue(findings.none { it.blocking })
    }

    @Test fun `disjoint circular hit regions are not rejected for overlapping boxes`() {
        val diagonal = TouchLayout(
            id = "diagonal-circles", schemaVersion = 1,
            controls = listOf(
                button("upper", ControllerButton.L1, 0.40f, 0.40f, 78f),
                button("lower", ControllerButton.R1, 0.48f, 0.535f, 50f, margin = 4f),
            ),
        )
        val resolved = TouchLayoutResolver.resolve(diagonal, region(800f, 400f))
        val findings = TouchLayoutAudit.audit(resolved.controls, resolved.region)
        assertFalse(findings.any { it.message.contains("overlapping") })
    }

    @Test fun `a target below the accessible minimum is reported`() {
        val tiny = TouchLayout(
            id = "tiny", schemaVersion = 1,
            controls = listOf(
                button("a", ControllerButton.L1, 0.5f, 0.5f, 20f),
            ),
        )
        val resolved = TouchLayoutResolver.resolve(tiny, region(832f, 440f))
        val findings = TouchLayoutAudit.audit(resolved.controls, resolved.region)
        assertTrue(findings.any { it.blocking && it.message.contains("answers to only") })
    }

    @Test fun `duplicate control ids are reported`() {
        val duplicated = TouchLayout(
            id = "dup", schemaVersion = 1,
            controls = listOf(
                button("same", ControllerButton.L1, 0.2f, 0.2f, 80f),
                button("same", ControllerButton.R1, 0.8f, 0.8f, 80f),
            ),
        )
        val resolved = TouchLayoutResolver.resolve(duplicated, region(832f, 440f))
        val findings = TouchLayoutAudit.audit(resolved.controls, resolved.region)
        assertTrue(findings.any { it.blocking && it.message.contains("Duplicate") })
    }

    /** Every probe, audited on real geometry rather than on the authored numbers. */
    @Test fun `no probe produces a blocking finding`() {
        PROBES.forEach { (name, size) ->
            listOf(1f, 2f, 2.75f, 3.5f).forEach { density ->
                val resolved = TouchLayoutResolver.resolve(
                    TouchLayoutV1.layout,
                    region(size.first, size.second, density),
                )
                val blocking = TouchLayoutAudit.audit(resolved.controls, resolved.region).filter { it.blocking }
                assertEquals("$name at density $density: $blocking", emptyList<TouchLayoutFinding>(), blocking)
            }
        }
    }

    @Test fun `the persisted schema version is stated`() {
        assertEquals(TouchLayoutV1.SCHEMA_VERSION, TouchLayoutV1.layout.schemaVersion)
        assertEquals(1, TouchLayoutV1.SCHEMA_VERSION)
    }

    /**
     * A synthetic digital button, for the audit cases that need a shape rather
     * than a real control. Named parameters everywhere it matters, so adding a
     * field to [TouchControlSpec] cannot silently shift these arguments.
     */
    private fun button(
        id: String,
        output: ControllerButton,
        anchorX: Float,
        anchorY: Float,
        size: Float,
        margin: Float = 0f,
    ) = TouchControlSpec(
        id = id,
        kind = TouchControlKind.Button,
        action = TouchControlAction.Logical(output),
        anchorX = anchorX,
        anchorY = anchorY,
        widthUnits = size,
        heightUnits = size,
        hitMarginUnits = margin,
    )
}
