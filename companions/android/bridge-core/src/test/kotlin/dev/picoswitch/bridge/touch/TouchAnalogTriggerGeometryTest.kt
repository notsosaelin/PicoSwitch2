package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * The invisible travel axis, as arithmetic.
 *
 * Everything the analog trigger gesture decides about DIRECTION and DISTANCE is
 * in [TouchTriggerTravel], with no contacts, no state and no rendering, and this
 * suite is why it was separated: the claim "moving a trigger changes the
 * direction of the next pull" is a claim about geometry, and proving it through
 * a Compose surface would prove almost nothing about it.
 *
 * The invariant under all of it is that direction comes from POSITION and never
 * from identity. Nothing here mentions `L` or `R`, because the implementation
 * must not either — a user who drags the left trigger to the bottom-right corner
 * has to get a sensible gesture without anyone having anticipated them.
 */
class TouchAnalogTriggerGeometryTest {

    private val config = TouchTriggerConfig()

    /** A 1200x600 rectangle at the origin; centre (600, 300). */
    private val region = TouchLayoutRegion(0f, 0f, 1200f, 600f, unitScale = 1f)

    private fun axisAt(x: Float, y: Float) =
        TouchTriggerTravel.inwardAxis(x, y, region, config.centerEpsilonUnits)

    /** The full-travel distance for [axis], which is what the axis now decides. */
    private fun fullTravel(axis: TouchVector, of: TouchLayoutRegion = region) =
        TouchTriggerTravel.fullTravelPx(of, axis, config.travelFraction, config.verticalTravelRatio)

    private fun value(
        axis: TouchVector,
        dx: Float,
        dy: Float,
        fullTravel: Float = fullTravel(axis),
    ) = TouchTriggerTravel.analogValue(dx, dy, axis, fullTravel)

    // --------------------------------------------------------------- direction

    @Test fun `a trigger above centre pulls downward`() {
        val axis = axisAt(300f, 60f)
        assertTrue("inward.y > 0", axis.y > 0f)
        assertTrue("inward.x > 0 toward the centre", axis.x > 0f)
        assertTrue(value(axis, 0f, 120f) > 0f)
        assertEquals(0f, value(axis, 0f, -120f), 0f)
    }

    @Test fun `a trigger below centre pulls upward`() {
        val axis = axisAt(300f, 540f)
        assertTrue("inward.y < 0", axis.y < 0f)
        assertTrue(value(axis, 0f, -120f) > 0f)
        assertEquals(0f, value(axis, 0f, 120f), 0f)
    }

    @Test fun `a trigger left of centre pulls rightward`() {
        val axis = axisAt(40f, 300f)
        assertEquals(1f, axis.x, 1e-4f)
        assertEquals(0f, axis.y, 1e-4f)
        assertTrue(value(axis, 120f, 0f) > 0f)
        assertEquals(0f, value(axis, -120f, 0f), 0f)
    }

    @Test fun `a trigger right of centre pulls leftward`() {
        val axis = axisAt(1160f, 300f)
        assertEquals(-1f, axis.x, 1e-4f)
        assertTrue(value(axis, -120f, 0f) > 0f)
        assertEquals(0f, value(axis, 120f, 0f), 0f)
    }

    /**
     * The four corners, as one statement: every axis points at the middle, and
     * the diagonal that follows it is what produces travel.
     */
    @Test fun `each corner pulls diagonally toward the centre`() {
        listOf(
            Triple(80f, 60f, 1f to 1f),
            Triple(1120f, 60f, -1f to 1f),
            Triple(80f, 540f, 1f to -1f),
            Triple(1120f, 540f, -1f to -1f),
        ).forEach { (x, y, expected) ->
            val axis = axisAt(x, y)
            val (sx, sy) = expected
            assertTrue("x sign at ($x,$y)", axis.x * sx > 0f)
            assertTrue("y sign at ($x,$y)", axis.y * sy > 0f)
            assertEquals("unit length at ($x,$y)", 1f, sqrt(axis.x * axis.x + axis.y * axis.y), 1e-4f)
            // Along the axis: travel. Straight back out along it: nothing.
            assertTrue(value(axis, axis.x * 200f, axis.y * 200f) > 0f)
            assertEquals(0f, value(axis, -axis.x * 200f, -axis.y * 200f), 0f)
        }
    }

    /**
     * The reason the axis is derived rather than declared. Nothing about the
     * control changed except where it is.
     */
    @Test fun `moving a control reverses the direction that increases its value`() {
        val top = axisAt(300f, 60f)
        val bottom = axisAt(300f, 540f)
        assertNotEquals(top.y > 0f, bottom.y > 0f)
        val downward = 150f
        assertTrue(value(top, 0f, downward) > 0f)
        assertEquals(0f, value(bottom, 0f, downward), 0f)
    }

    // ------------------------------------------------------------- degenerate

    /**
     * A control parked on the exact middle has no inward vector at all. It must
     * still produce ONE stable, finite, unit-length answer rather than noise —
     * an axis that flipped between frames would be a trigger that moved on its
     * own while a thumb was on it.
     */
    @Test fun `a control at the exact centre still gets a stable unit axis`() {
        val axis = axisAt(600f, 300f)
        assertTrue(axis.x.isFinite() && axis.y.isFinite())
        assertEquals(1f, sqrt(axis.x * axis.x + axis.y * axis.y), 1e-4f)
        // Nearest edge of a landscape rectangle is the top or the bottom, so the
        // fallback is vertical -- which is also what the shipped placement gives.
        assertEquals(0f, axis.x, 1e-4f)
        repeat(4) { assertEquals(axis, axisAt(600f, 300f)) }
    }

    @Test fun `a control just inside the centre epsilon uses the edge fallback`() {
        val nudged = axisAt(600f + config.centerEpsilonUnits / 2f, 300f)
        assertEquals(0f, nudged.x, 1e-4f)
        assertEquals(1f, abs(nudged.y), 1e-4f)
        // Just OUTSIDE it, the ordinary derivation takes over again.
        val clear = axisAt(600f + config.centerEpsilonUnits * 4f, 300f)
        assertEquals(-1f, clear.x, 1e-4f)
    }

    @Test fun `a region with no size never produces a zero or non-finite axis`() {
        val empty = TouchLayoutRegion(0f, 0f, 0f, 0f, unitScale = 1f)
        val axis = TouchTriggerTravel.inwardAxis(0f, 0f, empty, config.centerEpsilonUnits)
        assertTrue(axis.x.isFinite() && axis.y.isFinite())
        assertEquals(1f, sqrt(axis.x * axis.x + axis.y * axis.y), 1e-4f)
        assertEquals(0f, TouchTriggerTravel.analogValue(50f, 50f, axis, 0f), 0f)
    }

    // ----------------------------------------------------------------- travel

    /**
     * The two budgets, on the 1200x600 rectangle above: a full pull may spend
     * `Rx` across or `Ry` down, and ends the moment it spends either.
     *
     * `Ry` is deliberately half of `Rx` because the same pixels are a quarter of
     * a landscape screen's width but half of its height, and the thumb has
     * correspondingly less room and less mechanical range vertically.
     */
    @Test fun `the horizontal budget is twice the vertical one`() {
        val horizontal = fullTravel(axisAt(40f, 300f))
        val vertical = fullTravel(axisAt(600f, 60f))
        assertEquals("Rx", min(region.width, region.height) * config.travelFraction, horizontal, 1e-3f)
        assertEquals("Ry", horizontal * config.verticalTravelRatio, vertical, 1e-3f)
        assertEquals("a quarter of the width", region.width * 0.25f, horizontal, 1e-3f)
        assertEquals("a quarter of the height", region.height * 0.25f, vertical, 1e-3f)
    }

    @Test fun `a pure horizontal axis reaches full travel across a quarter of the width`() {
        val axis = axisAt(40f, 300f)
        assertEquals(1f, abs(axis.x), 1e-4f)
        val full = region.width * 0.25f
        assertEquals(0.5f, value(axis, full / 2f, 0f), 1e-3f)
        assertEquals(1f, value(axis, full, 0f), 1e-3f)
        // EXACTLY the distance the original single shared reference asked for,
        // so the placements that already felt right did not move.
        assertEquals(min(region.width, region.height) * config.travelFraction, full, 1e-3f)
    }

    @Test fun `a pure vertical axis reaches full travel down a quarter of the height`() {
        val axis = axisAt(600f, 60f)
        assertEquals(1f, axis.y, 1e-4f)
        val full = region.height * 0.25f
        assertEquals(0.5f, value(axis, 0f, full / 2f), 1e-3f)
        assertEquals(1f, value(axis, 0f, full), 1e-3f)
    }

    /**
     * The invariant the two-budget rule exists to state, and the one the
     * weighted blend it replaced could not: whatever direction the axis points,
     * completing a pull never moves the finger further than one budget in either
     * screen direction.
     *
     * The blend charged the horizontal budget in proportion to how much of the
     * AXIS lay along X, so a diagonal axis inflated the distance with width the
     * gesture never spent, and the vertical cost grew without bound as the axis
     * tilted. Measured on device, the shipped placement had reached 96% of the
     * usable height for a downward stroke.
     */
    @Test fun `no full pull spends more than one budget in either direction`() {
        val horizontal = min(region.width, region.height) * config.travelFraction
        val vertical = horizontal * config.verticalTravelRatio
        (0..90).forEach { degrees ->
            val radians = degrees.toDouble() / 180.0 * PI
            val axis = TouchVector(sin(radians).toFloat(), cos(radians).toFloat())
            val travel = fullTravel(axis)
            assertTrue(
                "axis at $degrees deg spends ${travel * axis.x} across",
                travel * abs(axis.x) <= horizontal + 1e-2f,
            )
            assertTrue(
                "axis at $degrees deg spends ${travel * axis.y} down",
                travel * abs(axis.y) <= vertical + 1e-2f,
            )
            // And one of the two is always exactly spent, or the pull would be
            // longer than it needed to be.
            assertTrue(
                "axis at $degrees deg spends neither budget in full",
                abs(travel * abs(axis.x) - horizontal) < 1e-2f ||
                    abs(travel * abs(axis.y) - vertical) < 1e-2f,
            )
        }
    }

    @Test fun `excess travel clamps and never exceeds one`() {
        val axis = axisAt(600f, 60f)
        assertEquals(1f, value(axis, 0f, 3000f), 0f)
        assertEquals(1f, value(axis, 0f, 300_000f), 0f)
    }

    /**
     * A thumb sweeps an arc, not a line. Projection is what makes the invisible
     * axis usable without an equally invisible corridor to stay inside -- in
     * BOTH orientations, now that they no longer share a distance.
     */
    @Test fun `large perpendicular drift barely moves the value`() {
        val vertical = axisAt(600f, 60f)
        val onVertical = value(vertical, 0f, region.height * 0.125f)
        assertEquals("a partial pull, not a clamped one", 0.5f, onVertical, 1e-3f)
        listOf(-400f, -150f, 150f, 400f).forEach { drift ->
            assertEquals("drift $drift", onVertical, value(vertical, drift, region.height * 0.125f), 1e-4f)
        }

        val horizontal = axisAt(40f, 300f)
        val onHorizontal = value(horizontal, region.width * 0.125f, 0f)
        assertEquals(0.5f, onHorizontal, 1e-3f)
        listOf(-250f, -80f, 80f, 250f).forEach { drift ->
            assertEquals("drift $drift", onHorizontal, value(horizontal, region.width * 0.125f, drift), 1e-4f)
        }
    }

    @Test fun `movement opposite the axis clamps to zero rather than going negative`() {
        val vertical = axisAt(600f, 60f)
        assertEquals(0f, value(vertical, 0f, -1f), 0f)
        assertEquals(0f, value(vertical, 0f, -9000f), 0f)
        assertEquals(0f, TouchTriggerTravel.projectedTravelPx(0f, -9000f, vertical), 0f)

        // The shorter vertical budget must not make backing out mean anything
        // either: opposite is opposite whichever way the axis points.
        val horizontal = axisAt(40f, 300f)
        assertEquals(0f, value(horizontal, -1f, 0f), 0f)
        assertEquals(0f, value(horizontal, -9000f, 0f), 0f)
        assertEquals(0f, TouchTriggerTravel.projectedTravelPx(-9000f, 0f, horizontal), 0f)
    }

    /**
     * The diagonals are the placements the shipped layout actually uses.
     *
     * The DISTANCE along a diagonal axis is allowed to exceed the horizontal
     * budget, by up to `sqrt(1 + ratio^2)` -- 12% at the shipped half, reached
     * where the two branches cross -- because a diagonal spends part of itself
     * in each direction. What may never exceed a budget is either COMPONENT, and
     * that is the claim the test above makes.
     */
    @Test fun `diagonal placements sit between the two budgets`() {
        val horizontal = fullTravel(axisAt(40f, 300f))
        val vertical = fullTravel(axisAt(600f, 60f))
        val longest = horizontal * sqrt(1f + config.verticalTravelRatio * config.verticalTravelRatio)

        listOf(
            80f to 60f, 1120f to 60f, 80f to 540f, 1120f to 540f,
            300f to 60f, 900f to 540f, 160f to 200f,
        ).forEach { (x, y) ->
            val axis = axisAt(x, y)
            assertTrue("($x,$y) is a diagonal", abs(axis.x) > 0.05f && abs(axis.y) > 0.05f)
            val travel = fullTravel(axis)
            assertTrue("($x,$y) at least the vertical budget", travel >= vertical - 1e-3f)
            assertTrue("($x,$y) within the crossover bound", travel <= longest + 1e-3f)
            assertEquals(1f, value(axis, axis.x * travel, axis.y * travel), 1e-3f)
        }
    }

    /**
     * Continuity through the crossover, which is why the rule is a minimum of
     * two smooth branches rather than a choice between two constants: the value
     * is continuous everywhere and only its slope has a corner.
     */
    @Test fun `travel changes smoothly as the axis rotates`() {
        var previous = fullTravel(TouchVector(0f, 1f))
        val steps = 900
        var largestStep = 0f
        (1..steps).forEach { step ->
            val radians = step.toDouble() / steps * PI / 2.0
            val axis = TouchVector(sin(radians).toFloat(), cos(radians).toFloat())
            val travel = fullTravel(axis)
            largestStep = maxOf(largestStep, abs(travel - previous))
            previous = travel
        }
        assertEquals("a full sweep ends on the horizontal budget", 300f, previous, 1e-2f)
        // A tenth of a degree moves full travel by about a pixel, steepest at
        // the crossover. A dominance branch would step by the whole 150 px
        // difference between the budgets at once.
        assertTrue("largest single step was " + largestStep + " px", largestStep < 2f)
    }

    /**
     * The value is a fraction of a distance derived from the REGION, so a larger
     * window makes a full pull a longer swipe rather than changing what any
     * given swipe means relative to the screen.
     */
    @Test fun `travel scales with the region, not with the control`() {
        val large = TouchLayoutRegion(0f, 0f, 2400f, 1200f, unitScale = 1f)
        val axis = TouchTriggerTravel.inwardAxis(1200f, 120f, large, config.centerEpsilonUnits)
        assertEquals("pure vertical", 1f, axis.y, 1e-4f)
        val full = fullTravel(axis, large)
        assertEquals(large.height * 0.25f, full, 1e-3f)
        assertEquals(0.5f, TouchTriggerTravel.analogValue(0f, full / 2f, axis, full), 1e-3f)
        // Twice the region, twice the swipe, in both orientations.
        assertEquals(2f * fullTravel(axisAt(600f, 60f)), fullTravel(TouchVector(0f, 1f), large), 1e-3f)
        assertEquals(2f * fullTravel(axisAt(40f, 300f)), fullTravel(TouchVector(1f, 0f), large), 1e-3f)
    }

    // ------------------------------------------------- direction is the layout's

    /**
     * The axis comes from the layout's NORMALIZED space, so the same authored
     * control produces the same gesture on every window shape.
     *
     * Derived in pixels instead, the shipped GameCube `L` measured `(0.818,
     * 0.575)` on a 1920x1025 handheld, `(0.772, 0.635)` on a 16:10 tablet and
     * `(0.712, 0.703)` on a 4:3 one -- the pull rotated by ten degrees between
     * devices because a wider window puts its centre further to the right, and
     * on the widest of them a downward thumb stroke recovered barely half of its
     * own travel.
     */
    @Test fun `the pull direction is the same on every window shape`() {
        val anchorX = 0.2f
        val anchorY = 0.105f
        val shapes = listOf(
            TouchLayoutRegion(0f, 0f, 1920f, 1025f, unitScale = 1f),
            TouchLayoutRegion(0f, 0f, 2560f, 1600f, unitScale = 1f),
            TouchLayoutRegion(0f, 0f, 2048f, 1536f, unitScale = 1f),
            TouchLayoutRegion(0f, 0f, 1200f, 600f, unitScale = 1f),
        )
        val axes = shapes.map { shape ->
            TouchTriggerTravel.inwardAxis(
                shape.left + anchorX * shape.width,
                shape.top + anchorY * shape.height,
                shape,
                config.centerEpsilonUnits,
            )
        }
        axes.forEach { axis ->
            assertEquals("x agrees across shapes", axes.first().x, axis.x, 1e-3f)
            assertEquals("y agrees across shapes", axes.first().y, axis.y, 1e-3f)
        }
        // And it leans DOWN rather than across, which is the direction a thumb
        // pulls a trigger placed at the top of the screen.
        assertTrue("more vertical than horizontal", abs(axes.first().y) > abs(axes.first().x))
    }

    /**
     * The device measurement this pass was built from, as arithmetic.
     *
     * Region, control centre, axis and full travel were captured from a real
     * Odin 2 Mini in landscape running the shipped GameCube layout; a downward
     * stroke needed 985 px of a 1025 px usable height to reach full travel. The
     * numbers below are that same placement, and the claim is the one the user
     * can feel: a thumb pulling straight down finishes the pull well inside the
     * screen.
     */
    @Test fun `a downward stroke on the shipped placement stays well inside the screen`() {
        val device = TouchLayoutRegion(0f, 55f, 1920f, 1080f, unitScale = 2.30625f)
        assertEquals(1920f, device.width, 1e-3f)
        assertEquals(1025f, device.height, 1e-3f)

        val axis = TouchTriggerTravel.inwardAxis(
            384f,
            device.top + 0.105f * device.height,
            device,
            config.centerEpsilonUnits,
        )
        val travel = fullTravel(axis, device)

        // Straight down, which is what a thumb does to a trigger at the top of
        // the screen and what the previous geometry punished.
        val downward = travel / abs(axis.y)
        assertTrue(
            "a downward stroke needs $downward px of ${device.height}",
            downward < device.height * 0.45f,
        )
        assertEquals(1f, TouchTriggerTravel.analogValue(0f, downward, axis, travel), 1e-3f)
        // The pull along the axis spends exactly the vertical budget and no more.
        val verticalBudget = min(device.width, device.height) * config.travelFraction *
            config.verticalTravelRatio
        assertEquals(verticalBudget, travel * abs(axis.y), 1e-2f)
    }

    /**
     * The landscape-TABLET case, which is where the symptom was reported.
     *
     * A near-pure vertical placement must reach full travel after the intended
     * fraction of the usable Y dimension -- a quarter of it -- in ACTUAL POINTER
     * DISPLACEMENT rather than in projected travel, which is the number a thumb
     * experiences.
     */
    @Test fun `a near vertical placement on a landscape tablet needs a quarter of the height`() {
        listOf(
            TouchLayoutRegion(0f, 0f, 2560f, 1600f, unitScale = 2f),
            TouchLayoutRegion(0f, 0f, 2048f, 1536f, unitScale = 2f),
            TouchLayoutRegion(0f, 24f, 2000f, 1200f, unitScale = 2f),
        ).forEach { tablet ->
            // A few pixels off the horizontal centre: NEAR-pure vertical, so the
            // claim cannot depend on hitting the degenerate case exactly.
            val centreX = (tablet.left + tablet.right) / 2f + 8f
            val axis = TouchTriggerTravel.inwardAxis(
                centreX,
                tablet.top + 0.1f * tablet.height,
                tablet,
                config.centerEpsilonUnits,
            )
            assertTrue("near-pure vertical on $tablet", abs(axis.y) > 0.99f)

            val travel = fullTravel(axis, tablet)
            // Walk the pointer down until the published value reaches 1.0, and
            // assert on the DISPLACEMENT that took, not on the projection.
            var displacement = 0f
            while (TouchTriggerTravel.analogValue(0f, displacement, axis, travel) < 1f &&
                displacement < tablet.height
            ) {
                displacement += 1f
            }
            assertEquals(
                "a quarter of the usable height on $tablet",
                tablet.height * 0.25f,
                displacement,
                2f,
            )
        }
    }

    // --------------------------------------------------- the RESTING fill

    private fun fillAt(x: Float, y: Float) = TouchTriggerTravel.fillDirection(axisAt(x, y))

    /**
     * Everything in this section is the fill a trigger shows when NOBODY IS
     * SWIPING it -- at rest, or during the part of a press before the drag slop
     * is crossed. Once a swipe exists the picture follows the swipe instead, and
     * `TouchAnalogTriggerOutputTest` owns that; the two are separate answers on
     * purpose, because a diagonal axis makes them disagree.
     *
     * The resting answer is the only statement available when there is no swipe
     * to read: where the control sits, and therefore which way it pulls. Nothing
     * here names a control.
     */
    @Test fun `each edge fills in the direction it pulls`() {
        assertEquals("above centre pulls down", TouchFillDirection.Down, fillAt(600f, 60f))
        assertEquals("below centre pulls up", TouchFillDirection.Up, fillAt(600f, 540f))
        assertEquals("left of centre pulls right", TouchFillDirection.Right, fillAt(40f, 300f))
        assertEquals("right of centre pulls left", TouchFillDirection.Left, fillAt(1160f, 300f))
    }

    /**
     * Mirrored placements produce mirrored fills, which is the property that
     * makes an asymmetric ANSWER a statement about the layout rather than about
     * the renderer.
     */
    @Test fun `mirrored placements fill in mirrored directions`() {
        listOf(60f, 200f, 380f).forEach { y ->
            listOf(40f, 160f, 300f, 500f).forEach { x ->
                val mirroredX = region.width - x
                val mirroredY = region.height - y
                assertEquals(
                    "left/right mirror at ($x,$y)",
                    mirror(fillAt(x, y)),
                    fillAt(mirroredX, y),
                )
                assertEquals(
                    "top/bottom mirror at ($x,$y)",
                    flip(fillAt(x, y)),
                    fillAt(x, mirroredY),
                )
            }
        }
    }

    /**
     * The shipped GameCube triggers' RESTING fills, pinned as the layout places
     * them.
     *
     * Both analog triggers take the OUTER slot on their own side — `trigger-l`
     * at anchor 0.075 and `trigger-r` at 0.925 — so each mirrors the other and
     * both lean ACROSS rather than down. `zl` and `z` hold the inner slots and
     * mirror each other in turn.
     *
     * The resting fill is still only a default: an axis that leans across while
     * a thumb pulling the trigger moves DOWN is exactly why the picture follows
     * the user as soon as they swipe.
     */
    @Test fun `the shipped GameCube triggers fill as their own placements dictate`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val resolved = TouchLayoutResolver.resolve(
            TouchLayoutComposer.compose(profile).layout,
            region,
        )
        assertTrue(resolved.problem ?: "", resolved.fits)

        fun fillOf(id: String): TouchFillDirection {
            val control = resolved.control(id)!!
            return TouchTriggerTravel.fillDirection(
                TouchTriggerTravel.inwardAxis(
                    control.centerX, control.centerY, resolved.region, config.centerEpsilonUnits,
                ),
            )
        }

        assertEquals("the outer-left slot leans across", TouchFillDirection.Right, fillOf("trigger-l"))
        assertEquals("the outer-right slot leans across", TouchFillDirection.Left, fillOf("trigger-r"))
        // The mirror of each one is the OTHER top-row control at that anchor, and
        // those agree -- which is what says the renderer is symmetric.
        assertEquals(mirror(fillOf("trigger-l")), fillOf("trigger-r"))
        assertEquals(mirror(fillOf("zl")), fillOf("z"))
    }

    /**
     * The editor's whole promise, restated for the picture: moving a trigger
     * re-presents it, with no special case for where it started.
     */
    @Test fun `moving a trigger changes which way it fills`() {
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)

        fun fillAfterMoving(dx: Float, dy: Float): TouchFillDirection {
            val moved = TouchLayoutResolver.resolve(
                TouchLayoutComposer.compose(profile, nudged(profile, "trigger-l", dx, dy)).layout,
                region,
                TouchLayoutAuditMode.UserDraft,
            )
            val control = moved.control("trigger-l")!!
            return TouchTriggerTravel.fillDirection(
                TouchTriggerTravel.inwardAxis(
                    control.centerX, control.centerY, moved.region, config.centerEpsilonUnits,
                ),
            )
        }

        assertEquals("where it ships", TouchFillDirection.Right, fillAfterMoving(0f, 0f))
        // Straight down the middle near the bottom edge: now pulled upward.
        assertEquals("dragged to the bottom centre", TouchFillDirection.Up, fillAfterMoving(0.425f, 0.8f))
        // Down the left edge to mid-height: now pulled across.
        assertEquals("dragged to the left edge", TouchFillDirection.Right, fillAfterMoving(-0.015f, 0.395f))
    }

    /**
     * A perfect 45-degree axis is genuinely ambiguous, so the answer is STATED
     * rather than left to whichever comparison ran first -- a control parked
     * exactly there must not pick a different direction per frame.
     */
    @Test fun `an exactly diagonal axis resolves the same way every time`() {
        val diagonal = TouchVector(0.70710677f, 0.70710677f)
        repeat(8) {
            assertEquals(TouchFillDirection.Down, TouchTriggerTravel.fillDirection(diagonal))
        }
        assertEquals(
            TouchFillDirection.Up,
            TouchTriggerTravel.fillDirection(TouchVector(0.70710677f, -0.70710677f)),
        )
        // A hair either side of the boundary picks the component that is larger.
        assertEquals(
            TouchFillDirection.Right,
            TouchTriggerTravel.fillDirection(TouchVector(0.708f, 0.706f)),
        )
        assertEquals(
            TouchFillDirection.Down,
            TouchTriggerTravel.fillDirection(TouchVector(0.706f, 0.708f)),
        )
    }

    /** The degenerate axis still names one direction rather than none. */
    @Test fun `a region with no size still yields a fill direction`() {
        val empty = TouchLayoutRegion(0f, 0f, 0f, 0f, unitScale = 1f)
        val axis = TouchTriggerTravel.inwardAxis(0f, 0f, empty, config.centerEpsilonUnits)
        assertEquals(TouchFillDirection.Down, TouchTriggerTravel.fillDirection(axis))
    }

    private fun mirror(direction: TouchFillDirection) = when (direction) {
        TouchFillDirection.Left -> TouchFillDirection.Right
        TouchFillDirection.Right -> TouchFillDirection.Left
        else -> direction
    }

    private fun flip(direction: TouchFillDirection) = when (direction) {
        TouchFillDirection.Up -> TouchFillDirection.Down
        TouchFillDirection.Down -> TouchFillDirection.Up
        else -> direction
    }

    // ----------------------------------------------------------------- detent

    /**
     * The band exists to stop a thumb resting on the boundary chattering the
     * terminal click, which on this personality is a gameplay button.
     */
    @Test fun `the detent has hysteresis in both directions`() {
        val engage = config.detentEngageFraction
        val release = config.detentReleaseFraction
        assertTrue(release < engage)

        var detent = TouchAnalogTriggerState.detentWithHysteresis(engage - 0.001f, false, config)
        assertTrue("just below engage stays open", !detent)
        detent = TouchAnalogTriggerState.detentWithHysteresis(engage, detent, config)
        assertTrue("at engage it clicks", detent)
        detent = TouchAnalogTriggerState.detentWithHysteresis(engage - 0.05f, detent, config)
        assertTrue("inside the band it stays clicked", detent)
        detent = TouchAnalogTriggerState.detentWithHysteresis(release, detent, config)
        assertTrue("below release it lets go", !detent)
    }

    /**
     * The band would be a local fiction without this. The firmware's GameCube
     * seam derives the click from the trigger BYTE alone (`> 224`) and discards
     * the button bit, so any sub-detent value above that ceiling would assert the
     * click on the wire no matter what this side believed.
     */
    @Test fun `sub-detent travel can never reach the byte the firmware calls clicked`() {
        val ceiling = config.subDetentCeiling
        assertTrue(ceiling < config.detentEngageFraction)
        assertTrue(
            "the detent must let go below the wire threshold",
            config.detentReleaseFraction < ceiling,
        )
        assertEquals(
            TouchTriggerConfig.SUB_DETENT_BYTE.toInt(),
            TouchAxis.triggerToBridge(ceiling),
        )
        // One byte above the ceiling is what the firmware reads as clicked.
        assertTrue(TouchAxis.triggerToBridge(ceiling) <= 224)
        assertTrue(TouchAxis.triggerToBridge(1f) > 224)
    }
}
