package dev.picoswitch.bridge.touch

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

/** A document plus whatever the operation created, so a host can select it. */
data class TouchEditResult(
    val document: TouchLayoutDocument,
    /** Instances the operation brought into existence, in creation order. */
    val created: List<String> = emptyList(),
    /** Set when the operation could not be performed, for the surface to show. */
    val refusal: String? = null,
) {
    val changed: Boolean get() = refusal == null
}

/**
 * Every pure operation an editor performs on a layout document.
 *
 * ## Why there are no command objects
 *
 * Undo/redo is a stack of DOCUMENTS ([TouchEditorHistory]), not a stack of
 * invertible commands, because every operation here is already a pure total
 * function from one document to the next. A revision stack cannot desynchronize
 * from the thing it is undoing, needs no inverse to be written (and kept
 * correct) for each new operation, and makes gesture coalescing a matter of when
 * a revision is pushed rather than of merging command objects.
 *
 * ## Selections and groups
 *
 * Every entry point takes a SELECTION of instance ids. [expand] applies group
 * membership once, at the call site, so what a surface highlights and what an
 * edit actually moves are the same set — an editor that quietly moves four
 * things while outlining one is an editor nobody trusts.
 *
 * ## Where geometry needs the region
 *
 * Operations that are naturally expressed in what the user can see — drag by
 * this many pixels, scale this cluster about its centre — take the resolved
 * layout, because the answer genuinely depends on the rectangle on screen. The
 * result is still stored in the document's own aspect-independent terms. Purely
 * declarative operations (rotate by 15 degrees, set latch, reorder) do not need
 * it and do not take it.
 */
object TouchLayoutEditor {
    /** Kept as the editor's own names; the values live with the stored limits. */
    const val MIN_SCALE = TouchLayoutLimits.MIN_SCALE
    const val MAX_SCALE = TouchLayoutLimits.MAX_SCALE

    /** How far a duplicate or a newly added control sits from its origin, in logical units. */
    const val PLACEMENT_STEP_UNITS = 28f

    /** Rotation lands exactly on a snap target within this many degrees of it. */
    const val ROTATION_SNAP_DEGREES = 6f

    fun authoredDefault(profile: TouchControllerProfile): TouchLayoutDocument =
        TouchLayoutDocument.authoredDefault(profile)

    /**
     * The instances an operation on this selection actually touches.
     *
     * Public because a host has to draw it. With [editGroup] off, a selection is
     * literally itself, which is what makes editing one button of a cluster
     * possible at all.
     */
    fun expand(
        document: TouchLayoutDocument,
        selection: Set<String>,
        editGroup: Boolean,
    ): Set<String> {
        if (!editGroup) return selection.filterTo(linkedSetOf()) { document.instance(it) != null }
        val result = linkedSetOf<String>()
        selection.forEach { id -> result += document.groupMembers(id) }
        return result
    }

    // ------------------------------------------------------------------- composition

    /**
     * Create a new instance of a catalog entry.
     *
     * Placement has two cases, and the distinction matters:
     *
     * ```text
     * the authored spot is free   put it exactly there
     * something is already there  near fallbackAnchor, stepped clear
     * ```
     *
     * The first is what "add the control back" should mean — a personality's
     * authored position for its grips is a considered piece of layout design,
     * not an arbitrary starting point, and dropping a restored control in the
     * middle of the screen would throw that away. The second is what "give me
     * another one of these" should mean: a duplicate created exactly underneath
     * its twin looks like nothing happened.
     *
     * Group membership follows placement, and only placement:
     *
     * ```text
     * landed at the authored spot   joins the authored cluster
     * placed anywhere else          free-standing
     * ```
     *
     * Restoring a deleted control should put it back completely — geometry,
     * cluster and all — or "delete then undo by re-adding" would quietly leave
     * the face diamond one member short of a group. A control the user placed
     * somewhere else is a new object and must not be dragged around by a group
     * transform it never asked to be part of.
     *
     * The new instance is brought to the front and returned so the surface can
     * select it immediately.
     */
    fun add(
        document: TouchLayoutDocument,
        profile: TouchControllerProfile,
        catalogId: String,
        fallbackAnchorX: Float,
        fallbackAnchorY: Float,
    ): TouchEditResult {
        val entry = profile.catalogEntry(catalogId)
            ?: return TouchEditResult(document, refusal = "This controller has no '$catalogId'")
        val authored = entry.geometry
        val instanceId = allocateInstanceId(document, catalogId)
        val instance = if (isFree(document, authored.anchorX, authored.anchorY, authored)) {
            TouchControlInstance(
                instanceId = instanceId,
                catalogId = entry.id,
                anchorX = authored.anchorX,
                anchorY = authored.anchorY,
                offsetXUnits = authored.groupOffsetXUnits,
                offsetYUnits = authored.groupOffsetYUnits,
                zIndex = nextZIndex(document),
                // Only when the group still exists in this document. Recreating
                // a cluster that the user has entirely dissolved would resurrect
                // a grouping they deliberately took apart.
                groupId = entry.editGroupId?.takeIf { group ->
                    document.controls.any { it.groupId == group }
                },
            )
        } else {
            val step = PLACEMENT_STEP_UNITS * occupancy(document, fallbackAnchorX, fallbackAnchorY)
            TouchControlInstance(
                instanceId = instanceId,
                catalogId = entry.id,
                anchorX = fallbackAnchorX.coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
                anchorY = fallbackAnchorY.coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
                offsetXUnits = step,
                offsetYUnits = step,
                zIndex = nextZIndex(document),
            )
        }
        return TouchEditResult(
            document.copy(controls = document.controls + instance),
            created = listOf(instanceId),
        )
    }

    /**
     * Clone the selected instances.
     *
     * Each clone keeps its source's transform, behaviour and group membership
     * and is offset slightly so it is visibly a second object. Cloning a whole
     * group produces a whole new group, because copying half a cluster into the
     * original's group would silently change what the original group means.
     */
    fun duplicate(
        document: TouchLayoutDocument,
        selection: Set<String>,
        editGroup: Boolean,
    ): TouchEditResult {
        val targets = expand(document, selection, editGroup)
        if (targets.isEmpty()) return TouchEditResult(document, refusal = "Nothing is selected")
        var next = document
        val created = mutableListOf<String>()
        val groupMapping = mutableMapOf<String, String>()
        document.controls.filter { it.instanceId in targets }.forEach { source ->
            val instanceId = allocateInstanceId(next, source.catalogId)
            val group = source.groupId?.let { original ->
                groupMapping.getOrPut(original) { allocateGroupId(next) }
            }
            val clone = source.copy(
                instanceId = instanceId,
                offsetXUnits = source.offsetXUnits + PLACEMENT_STEP_UNITS,
                offsetYUnits = source.offsetYUnits + PLACEMENT_STEP_UNITS,
                zIndex = nextZIndex(next),
                groupId = group,
            )
            next = next.copy(controls = next.controls + clone)
            created += instanceId
        }
        return TouchEditResult(next, created)
    }

    /**
     * Remove instances from the layout.
     *
     * Genuinely removed, not hidden: an absent instance does not exist, and Add
     * Control is how one comes back. The whole selection goes in one operation
     * so a single undo restores it as one thing.
     */
    fun delete(
        document: TouchLayoutDocument,
        selection: Set<String>,
        editGroup: Boolean,
    ): TouchEditResult {
        val targets = expand(document, selection, editGroup)
        if (targets.isEmpty()) return TouchEditResult(document, refusal = "Nothing is selected")
        return TouchEditResult(
            document.copy(controls = document.controls.filterNot { it.instanceId in targets }),
        )
    }

    // ------------------------------------------------------------------------ groups

    /**
     * Make the selection one group.
     *
     * Pure membership: no geometry changes at all, which is what makes grouping
     * and ungrouping exactly reversible on any window shape. Members already in
     * other groups are moved into this one — an instance belongs to at most one
     * group, structurally, so there is no nested-group state to represent.
     */
    fun group(document: TouchLayoutDocument, selection: Set<String>): TouchEditResult {
        val targets = expand(document, selection, editGroup = false)
        if (targets.size < 2) {
            return TouchEditResult(document, refusal = "Select two or more controls to group them")
        }
        val groupId = allocateGroupId(document)
        return TouchEditResult(
            document.copy(
                controls = document.controls.map {
                    if (it.instanceId in targets) it.copy(groupId = groupId) else it
                },
            ),
        )
    }

    /** Drop group membership. Visually lossless, for the same reason [group] is. */
    fun ungroup(document: TouchLayoutDocument, selection: Set<String>): TouchEditResult {
        val targets = expand(document, selection, editGroup = true)
        if (targets.none { document.instance(it)?.groupId != null }) {
            return TouchEditResult(document, refusal = "Nothing in the selection is grouped")
        }
        return TouchEditResult(
            document.copy(
                controls = document.controls.map {
                    if (it.instanceId in targets) it.copy(groupId = null) else it
                },
            ),
        )
    }

    // --------------------------------------------------------------------- transforms

    /**
     * Drag the selection by a screen-space delta.
     *
     * One clamp for the WHOLE selection rather than per member: clamping each
     * member after the move would compress a cluster against an edge and
     * silently destroy the relative spacing that makes it a cluster.
     */
    fun move(
        document: TouchLayoutDocument,
        resolved: ResolvedTouchLayout,
        selection: Set<String>,
        deltaX: Float,
        deltaY: Float,
        editGroup: Boolean,
    ): TouchLayoutDocument {
        if (!deltaX.isFinite() || !deltaY.isFinite()) return document
        val targets = expand(document, selection, editGroup)
        if (targets.isEmpty()) return document
        val region = resolved.region
        if (region.width <= 0f || region.height <= 0f) return document
        val placed = targets.mapNotNull(resolved::control)
        if (placed.isEmpty()) return document

        val allowedX = deltaX.coerceIn(
            minimumValue = placed.maxOf { region.left + it.hitExtentX - it.centerX },
            maximumValue = placed.minOf { region.right - it.hitExtentX - it.centerX },
        )
        val allowedY = deltaY.coerceIn(
            minimumValue = placed.maxOf { region.top + it.hitExtentY - it.centerY },
            maximumValue = placed.minOf { region.bottom - it.hitExtentY - it.centerY },
        )
        val normalizedX = allowedX / region.width
        val normalizedY = allowedY / region.height
        return update(document, targets) { instance ->
            instance.copy(
                anchorX = (instance.anchorX + normalizedX)
                    .coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
                anchorY = (instance.anchorY + normalizedY)
                    .coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
            )
        }
    }

    /**
     * Put the selection at an absolute normalized anchor.
     *
     * The precise-entry counterpart to [move], and the operation a test or an
     * importer wants: no region, no clamping against neighbours, just "this
     * control's anchor is now here". The audit still has the final word on
     * whether the result is playable.
     */
    fun place(
        document: TouchLayoutDocument,
        selection: Set<String>,
        anchorX: Float,
        anchorY: Float,
    ): TouchLayoutDocument {
        if (!anchorX.isFinite() || !anchorY.isFinite()) return document
        return update(document, expand(document, selection, editGroup = false)) {
            it.copy(
                anchorX = anchorX.coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
                anchorY = anchorY.coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
            )
        }
    }

    /**
     * Multiply the selection's size, about the selection's own centroid.
     *
     * Relative rather than absolute because a pinch has no absolute value to
     * report, and because a multi-selection has no single current size: one
     * absolute scale would flatten sizes the user deliberately made different on
     * the very first pinch.
     *
     * A single control simply grows in place — its centroid is itself — while a
     * cluster's members also move apart, which is what makes scaling a group
     * behave like scaling one physical object. The displacement is written into
     * the aspect-independent offset, so the scaled cluster stays rigid on every
     * other window shape.
     */
    fun scaleBy(
        document: TouchLayoutDocument,
        resolved: ResolvedTouchLayout,
        selection: Set<String>,
        factor: Float,
        editGroup: Boolean,
    ): TouchLayoutDocument {
        if (!factor.isFinite() || factor <= 0f) return document
        val targets = expand(document, selection, editGroup)
        if (targets.isEmpty()) return document
        val unit = resolved.region.unitScale * resolved.scale
        val centroid = centroid(resolved, targets)
        return update(document, targets) { instance ->
            val applied = (instance.scale * factor).coerceIn(MIN_SCALE, MAX_SCALE)
            // The applied factor may be clipped by the size limits; move the
            // member by what actually happened, never by what was asked for, or
            // a cluster at its size limit slowly tears itself apart.
            val effective = if (instance.scale > 0f) applied / instance.scale else 1f
            val placed = resolved.control(instance.instanceId)
            if (placed == null || centroid == null || unit <= 0f) {
                instance.copy(scale = applied)
            } else {
                instance.copy(
                    scale = applied,
                    offsetXUnits = instance.offsetXUnits +
                        (placed.centerX - centroid.x) * (effective - 1f) / unit,
                    offsetYUnits = instance.offsetYUnits +
                        (placed.centerY - centroid.y) * (effective - 1f) / unit,
                )
            }
        }
    }

    /** Set an absolute size multiplier; used by numeric entry and the size buttons. */
    fun setScale(
        document: TouchLayoutDocument,
        selection: Set<String>,
        scale: Float,
        editGroup: Boolean,
    ): TouchLayoutDocument {
        if (!scale.isFinite()) return document
        val applied = scale.coerceIn(MIN_SCALE, MAX_SCALE)
        return update(document, expand(document, selection, editGroup)) { it.copy(scale = applied) }
    }

    /**
     * Turn the selection about its centroid.
     *
     * Both halves, together: each member's own orientation gains [degrees] and
     * its position rotates about the shared centre, so a grouped cluster behaves
     * like one turned object rather than like several independently spinning
     * ones. A lone control has no displacement to rotate and simply turns.
     *
     * Purely presentational. Nothing here can change a binding, a D-pad
     * direction, or an analog trigger's travel axis.
     */
    fun rotateBy(
        document: TouchLayoutDocument,
        resolved: ResolvedTouchLayout,
        selection: Set<String>,
        degrees: Float,
        editGroup: Boolean,
    ): TouchLayoutDocument {
        if (!degrees.isFinite() || degrees == 0f) return document
        val targets = expand(document, selection, editGroup)
        if (targets.isEmpty()) return document
        val unit = resolved.region.unitScale * resolved.scale
        val centroid = centroid(resolved, targets)
        val radians = degrees.toDouble() * PI / 180.0
        val cosine = cos(radians).toFloat()
        val sine = sin(radians).toFloat()
        return update(document, targets) { instance ->
            val turned = instance.copy(
                rotationDegrees = TouchLayoutLimits.normalizeRotation(
                    instance.rotationDegrees + degrees,
                ),
            )
            val placed = resolved.control(instance.instanceId)
            if (placed == null || centroid == null || unit <= 0f) {
                turned
            } else {
                val dx = placed.centerX - centroid.x
                val dy = placed.centerY - centroid.y
                turned.copy(
                    offsetXUnits = instance.offsetXUnits + (dx * cosine - dy * sine - dx) / unit,
                    offsetYUnits = instance.offsetYUnits + (dx * sine + dy * cosine - dy) / unit,
                )
            }
        }
    }

    /**
     * Set one control's own orientation, relative to its authored one.
     *
     * The precise-entry counterpart to [rotateBy], and deliberately NOT a group
     * operation: "every member of this cluster is now at 30 degrees" is a
     * different and much less useful statement than "turn this cluster by 30
     * degrees", and offering the first through the same control would make the
     * second unreachable.
     */
    fun setRotation(
        document: TouchLayoutDocument,
        selection: Set<String>,
        degrees: Float,
    ): TouchLayoutDocument {
        if (!degrees.isFinite()) return document
        val applied = TouchLayoutLimits.normalizeRotation(degrees)
        return update(document, expand(document, selection, editGroup = false)) {
            it.copy(rotationDegrees = applied)
        }
    }

    /** Put the selection back to the orientation the catalog authored for it. */
    fun resetRotation(
        document: TouchLayoutDocument,
        selection: Set<String>,
        editGroup: Boolean,
    ): TouchLayoutDocument = update(document, expand(document, selection, editGroup)) {
        it.copy(rotationDegrees = 0f)
    }

    /**
     * The nearest angle a live rotation should settle on, or the angle itself.
     *
     * Magnetic rather than discrete: the user keeps every angle in between and
     * only the useful ones — the authored orientation and its quarter turns —
     * pull. Expressed against the instance's own rotation, which is already
     * relative to the authored angle, so "0" IS the authored orientation and the
     * arithmetic needs no special case for a bean that ships at 10.7 degrees.
     */
    fun snapRotation(degrees: Float): Float {
        if (!degrees.isFinite()) return 0f
        val normalized = TouchLayoutLimits.normalizeRotation(degrees)
        val quarter = (normalized / 90f).roundToInt() * 90f
        return if (abs(normalized - quarter) <= ROTATION_SNAP_DEGREES) {
            TouchLayoutLimits.normalizeRotation(quarter)
        } else {
            normalized
        }
    }

    /**
     * The rotation delta to actually apply, so that [primaryId] lands exactly on
     * a snap target whenever the gesture brings it near one.
     *
     * Computed from ONE reference control and returned as a delta for the whole
     * selection, for the same reason movement snapping works that way: applying
     * a per-member snap would pull a cluster's members onto the same angle and
     * destroy the composition. The caller can compare the result with what it
     * asked for to know a snap was acquired, and buzz once.
     */
    fun snappedRotationDelta(
        document: TouchLayoutDocument,
        primaryId: String?,
        /** This frame's raw angle change. Applied as-is when there is no reference. */
        degrees: Float,
        /**
         * Where the GESTURE has turned to: the total raw angle it has described
         * since it began, relative to the authored orientation, with [degrees]
         * already included and every snap applied so far ignored.
         *
         * A separate number from the stored angle, and that separation is the
         * whole point. Deriving the target from `stored + degrees` instead means
         * a control sitting inside a snap zone can never leave it: each frame
         * proposes stored plus a fraction of a degree, the magnet pulls it back
         * to the same target, the stored angle therefore never moves, and the
         * next frame asks the identical question and gets the identical answer.
         * Rotation then only escapes if one single frame happens to carry more
         * than [ROTATION_SNAP_DEGREES] — which is why turning a control used to
         * need a whole-hand flick instead of a wrist.
         *
         * Keeping the raw intent makes the magnet a DETENT: the first few
         * degrees are absorbed, and the control releases as soon as the fingers
         * have genuinely asked for more than the snap radius.
         */
        intentDegrees: Float,
    ): Float {
        if (!degrees.isFinite() || !intentDegrees.isFinite()) return 0f
        val current = primaryId?.let { document.instance(it) }?.rotationDegrees ?: return degrees
        val target = snapRotation(intentDegrees)
        // Back through normalization so a snap across the +/-180 seam is the
        // short way round rather than a 359-degree spin.
        return TouchLayoutLimits.normalizeRotation(target - current)
    }

    // ----------------------------------------------------------------------- z-order

    fun bringToFront(document: TouchLayoutDocument, selection: Set<String>, editGroup: Boolean) =
        restack(document, expand(document, selection, editGroup), toFront = true)

    fun sendToBack(document: TouchLayoutDocument, selection: Set<String>, editGroup: Boolean) =
        restack(document, expand(document, selection, editGroup), toFront = false)

    fun bringForward(document: TouchLayoutDocument, selection: Set<String>, editGroup: Boolean) =
        nudgeStack(document, expand(document, selection, editGroup), forward = true)

    fun sendBackward(document: TouchLayoutDocument, selection: Set<String>, editGroup: Boolean) =
        nudgeStack(document, expand(document, selection, editGroup), forward = false)

    // ---------------------------------------------------------------------- behaviour

    /**
     * Choose whether these controls answer to the hold gestures.
     *
     * `null` means "follow the global setting" and DROPS the stored answer
     * rather than freezing whatever the setting happens to say today: a control
     * that kept latching after the user turned the setting off would be doing it
     * for no reason they could see.
     *
     * Per instance, not per binding. Two A buttons may hold differently, and
     * that is a coherent thing to want — one to lean on, one to tap.
     */
    fun setLatch(
        document: TouchLayoutDocument,
        profile: TouchControllerProfile,
        selection: Set<String>,
        latch: Boolean?,
        editGroup: Boolean,
    ): TouchLayoutDocument = update(document, expand(document, selection, editGroup)) { instance ->
        val entry = profile.catalogEntry(instance.catalogId)
        if (entry?.interaction?.supportsLatch != true) instance else instance.copy(latch = latch)
    }

    // -------------------------------------------------------------------------- reset

    /**
     * Restore the authored transform of the selected instances.
     *
     * Only instances whose id is still a catalog id can be reset — those are the
     * ones the authored default has an opinion about. A duplicate the user
     * created has no authored position, so it keeps the one it has; resetting it
     * to the original's place would silently stack the two.
     */
    fun reset(
        document: TouchLayoutDocument,
        profile: TouchControllerProfile,
        selection: Set<String>,
        editGroup: Boolean,
    ): TouchLayoutDocument {
        val authored = TouchLayoutDocument.authoredDefault(profile)
        return update(document, expand(document, selection, editGroup)) { instance ->
            authored.instance(instance.instanceId)?.copy(zIndex = instance.zIndex) ?: instance
        }
    }

    /** Replace the whole layout with a fresh copy of the shipped one. */
    fun resetAll(profile: TouchControllerProfile): TouchLayoutDocument =
        TouchLayoutDocument.authoredDefault(profile)

    // ---------------------------------------------------------------------- internals

    private fun update(
        document: TouchLayoutDocument,
        targets: Set<String>,
        transform: (TouchControlInstance) -> TouchControlInstance,
    ): TouchLayoutDocument {
        if (targets.isEmpty()) return document
        return document.copy(
            controls = document.controls.map {
                if (it.instanceId in targets) transform(it) else it
            },
        )
    }

    /** Centre of the selection's placed controls, in screen coordinates. */
    private fun centroid(resolved: ResolvedTouchLayout, targets: Set<String>): TouchVector? {
        val placed = targets.mapNotNull(resolved::control)
        if (placed.isEmpty()) return null
        return TouchVector(
            placed.sumOf { it.centerX.toDouble() }.toFloat() / placed.size,
            placed.sumOf { it.centerY.toDouble() }.toFloat() / placed.size,
        )
    }

    /**
     * A readable, deterministic instance id.
     *
     * `dpad`, then `dpad#2`, `dpad#3`. Derived from the document rather than
     * randomly generated so every operation here stays a pure function and the
     * migration fixtures mean something; readable so a stored document can be
     * understood by a person looking at it.
     */
    internal fun allocateInstanceId(document: TouchLayoutDocument, catalogId: String): String {
        val taken = document.controls.mapTo(mutableSetOf()) { it.instanceId }
        if (catalogId !in taken) return catalogId
        var suffix = 2
        while ("$catalogId#$suffix" in taken) suffix++
        return "$catalogId#$suffix"
    }

    internal fun allocateGroupId(document: TouchLayoutDocument): String {
        val taken = document.controls.mapNotNullTo(mutableSetOf()) { it.groupId }
        var suffix = 1
        while ("group-$suffix" in taken) suffix++
        return "group-$suffix"
    }

    private fun nextZIndex(document: TouchLayoutDocument): Int =
        (document.controls.maxOfOrNull { it.zIndex } ?: -1) + 1

    /**
     * How crowded a placement point already is, in units of one nudge.
     *
     * Counts what is near the target so the second, third and fourth copy of a
     * control step further out instead of piling up on each other.
     */
    private fun occupancy(document: TouchLayoutDocument, anchorX: Float, anchorY: Float): Float =
        document.controls.count { near(it, anchorX, anchorY, 0f, 0f) }.toFloat()

    /** True when nothing already sits at this approximate place. */
    private fun isFree(
        document: TouchLayoutDocument,
        anchorX: Float,
        anchorY: Float,
        geometry: TouchControlGeometry,
    ): Boolean = document.controls.none {
        near(it, anchorX, anchorY, geometry.groupOffsetXUnits, geometry.groupOffsetYUnits)
    }

    /**
     * Whether an instance's approximate centre coincides with a target place.
     *
     * Anchor AND offset, converted to one comparable normalized point through
     * the authoring reference shape. An anchor-only comparison would call every
     * member of the face diamond "the same place", because they share one.
     */
    private fun near(
        instance: TouchControlInstance,
        anchorX: Float,
        anchorY: Float,
        offsetXUnits: Float,
        offsetYUnits: Float,
    ): Boolean {
        val dx = (instance.anchorX - anchorX) +
            (instance.offsetXUnits - offsetXUnits) / TouchLayoutResolver.REFERENCE_WIDTH_UNITS
        val dy = (instance.anchorY - anchorY) +
            (instance.offsetYUnits - offsetYUnits) / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS
        return abs(dx) < NEARBY_ANCHOR && abs(dy) < NEARBY_ANCHOR
    }

    private fun restack(
        document: TouchLayoutDocument,
        targets: Set<String>,
        toFront: Boolean,
    ): TouchLayoutDocument {
        if (targets.isEmpty()) return document
        val ordered = document.controls.sortedBy { it.zIndex }
        val moved = ordered.filter { it.instanceId in targets }
        val rest = ordered.filterNot { it.instanceId in targets }
        val sequence = if (toFront) rest + moved else moved + rest
        return document.copy(controls = renumber(document, sequence))
    }

    private fun nudgeStack(
        document: TouchLayoutDocument,
        targets: Set<String>,
        forward: Boolean,
    ): TouchLayoutDocument {
        if (targets.isEmpty()) return document
        val ordered = document.controls.sortedBy { it.zIndex }.toMutableList()
        // Walk from the end the move is heading toward, so a contiguous run of
        // selected controls slides as a block instead of collapsing into itself.
        val indices = if (forward) ordered.indices.reversed() else ordered.indices
        indices.forEach { index ->
            if (ordered[index].instanceId !in targets) return@forEach
            val swap = if (forward) index + 1 else index - 1
            if (swap !in ordered.indices || ordered[swap].instanceId in targets) return@forEach
            val held = ordered[index]
            ordered[index] = ordered[swap]
            ordered[swap] = held
        }
        return document.copy(controls = renumber(document, ordered))
    }

    /** Reassign a dense z sequence and restore the document's own control order. */
    private fun renumber(
        document: TouchLayoutDocument,
        sequence: List<TouchControlInstance>,
    ): List<TouchControlInstance> {
        val z = sequence.withIndex().associate { (index, it) -> it.instanceId to index }
        return document.controls.map { it.copy(zIndex = z[it.instanceId] ?: it.zIndex) }
    }

    /** Anchors closer than this count as the same spot when placing a new control. */
    private const val NEARBY_ANCHOR = 0.06f
}

/**
 * Undo/redo for one editor session.
 *
 * A bounded stack of document revisions with a label each. Push once per
 * completed gesture rather than once per pointer frame — the working document
 * is authoritative during a drag and only the endpoints are worth remembering.
 *
 * Deliberately session-scoped: the layout itself is persisted normally, and
 * carrying a command history across process death would be a second thing that
 * can be corrupt for no benefit a user would notice.
 */
class TouchEditorHistory(
    initial: TouchLayoutDocument,
    private val limit: Int = DEFAULT_LIMIT,
) {
    private data class Revision(val document: TouchLayoutDocument, val label: String)

    private val past = ArrayDeque<Revision>()
    private val future = ArrayDeque<Revision>()

    var current: TouchLayoutDocument = initial
        private set

    val canUndo: Boolean get() = past.isNotEmpty()
    val canRedo: Boolean get() = future.isNotEmpty()

    /** What undo would take back, for a menu item that can name itself. */
    val undoLabel: String? get() = past.lastOrNull()?.label

    /**
     * Adopt [next] as a new revision.
     *
     * A no-op change pushes nothing, so a gesture that ended where it started
     * does not leave an undo step that appears to do nothing.
     */
    fun push(next: TouchLayoutDocument, label: String) {
        if (next == current) return
        past.addLast(Revision(current, label))
        while (past.size > limit) past.removeFirst()
        future.clear()
        current = next
    }

    fun undo(): TouchLayoutDocument? {
        val revision = past.removeLastOrNull() ?: return null
        future.addLast(Revision(current, revision.label))
        current = revision.document
        return current
    }

    fun redo(): TouchLayoutDocument? {
        val revision = future.removeLastOrNull() ?: return null
        past.addLast(Revision(current, revision.label))
        current = revision.document
        return current
    }

    /** Adopt a document from outside the editor, discarding the history it invalidates. */
    fun reset(document: TouchLayoutDocument) {
        past.clear()
        future.clear()
        current = document
    }

    companion object {
        /** Long enough for a real editing session, short enough to stay bounded. */
        const val DEFAULT_LIMIT = 64
    }
}
