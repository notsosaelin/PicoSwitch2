package dev.picoswitch.bridge.touch

/**
 * Bounds every stored instance transform has to satisfy.
 *
 * Here rather than on the editor because the CODEC needs them too: a document
 * arriving from storage or an import must pass exactly the checks an editor
 * operation would have enforced, or a hand-edited file becomes a way to
 * construct geometry the editor refuses to make.
 */
object TouchLayoutLimits {
    const val MIN_SCALE = 0.55f
    const val MAX_SCALE = 1.75f

    /**
     * How far a control's own anchor may sit outside the interaction rectangle.
     *
     * Zero: the anchor is the control's group origin and the audit already
     * refuses a control whose answerable region leaves the safe area, so a
     * document that stored an out-of-range anchor could only ever describe a
     * layout that refuses to load.
     */
    val ANCHOR_RANGE = 0f..1f

    /**
     * The largest offset a grouped member may carry from its anchor, in logical
     * units.
     *
     * Generous — a full reference width either way — because a cluster is
     * allowed to be as wide as the controller. It exists to reject nonsense
     * (a document claiming 10^9 units) rather than to constrain composition.
     */
    const val MAX_OFFSET_UNITS = TouchLayoutResolver.REFERENCE_WIDTH_UNITS

    /** Normalize a rotation to `[-180, 180)`, the one representation stored. */
    fun normalizeRotation(degrees: Float): Float {
        if (!degrees.isFinite()) return 0f
        var value = degrees % 360f
        if (value >= 180f) value -= 360f
        if (value < -180f) value += 360f
        // -0f compares equal to 0f but serializes differently; pin it.
        return if (value == 0f) 0f else value
    }
}

/**
 * One independently identifiable on-screen object.
 *
 * ## Instance identity is not logical button identity
 *
 * [instanceId] is the object; [catalogId] is what kind of object it is. Two
 * instances may name the same catalog entry, and therefore the same logical
 * binding, and remain completely separate things: separate hit region, separate
 * transform, separate latch, separate contact. That separation is the whole
 * point of the instance model and the one invariant most likely to be
 * accidentally undone by a future "simplification" that keys something by
 * binding.
 *
 * ## Position
 *
 * ```text
 * centre = region.left + anchorX * region.width + offsetXUnits * unit
 * ```
 *
 * Two terms because they behave differently under a change of window shape.
 * [anchorX]/[anchorY] are NORMALIZED, so they follow the rectangle and keep a
 * control at the edge a thumb can reach. [offsetXUnits]/[offsetYUnits] are in
 * LOGICAL UNITS, so they are immune to aspect-ratio distortion — which is what
 * keeps a square button diamond square on a display the layout was not authored
 * for. An ungrouped control normally carries only an anchor; a member of a
 * cluster shares its anchor with its siblings and differs by its offset.
 *
 * Group transforms exploit exactly that: grouping and ungrouping are pure
 * [groupId] changes and never touch geometry, while a group scale or rotation
 * writes the displacement into the offsets, where it stays rigid.
 *
 * ## Rotation
 *
 * [rotationDegrees] is the USER's rotation, relative to the catalog entry's
 * authored orientation. Stored that way so "reset orientation" means the
 * authored value rather than a blind zero, and so re-authoring a control's art
 * direction moves every existing instance with it.
 *
 * It is VISUAL AND HIT geometry only. Rotating a control never changes what it
 * sends, never re-labels a D-pad direction, and never turns an analog trigger's
 * position-derived travel axis.
 */
data class TouchControlInstance(
    val instanceId: String,
    val catalogId: String,
    /** Group origin, or the control's own centre when it has no offset. */
    val anchorX: Float,
    val anchorY: Float,
    /** Aspect-independent displacement from the anchor, in logical units. */
    val offsetXUnits: Float = 0f,
    val offsetYUnits: Float = 0f,
    val scale: Float = 1f,
    /** Clockwise, relative to the catalog entry's authored orientation. */
    val rotationDegrees: Float = 0f,
    /** Higher draws and hit-tests in front. */
    val zIndex: Int = 0,
    /** At most one group; membership is a property OF the instance, so an
     *  instance cannot be in two groups and a group cannot dangle. */
    val groupId: String? = null,
    /** `null` follows the global hold-to-latch setting. */
    val latch: Boolean? = null,
)

/**
 * A complete user layout: a scene of instances, not a patch over a stencil.
 *
 * ## Default layout membership is not personality capability
 *
 * A control absent from [controls] is not on screen. It is still perfectly
 * available — the personality's catalog says what may be instantiated, and Add
 * Control reads that. The two questions are separate and must stay separate;
 * conflating them is what the pre-2.0 "hidden template control" model did, and
 * it is why an optional control could not be genuinely deleted.
 *
 * [templateId] and [basedOnRevision] record which authored catalog the document
 * was written against, so a catalog that later drops an entry degrades one
 * instance rather than the whole layout.
 */
data class TouchLayoutDocument(
    val schemaVersion: Int = CURRENT_SCHEMA_VERSION,
    val profileId: TouchProfileId,
    val templateId: String,
    val basedOnRevision: Int,
    val controls: List<TouchControlInstance> = emptyList(),
) {
    private val byInstanceId: Map<String, TouchControlInstance> =
        controls.associateBy { it.instanceId }

    fun instance(instanceId: String): TouchControlInstance? = byInstanceId[instanceId]

    /** Instance ids in each group, in document order. Derived; never stored twice. */
    val groups: Map<String, List<String>>
        get() = controls.filter { it.groupId != null }
            .groupBy({ requireNotNull(it.groupId) }, { it.instanceId })

    /** Members of the group [instanceId] belongs to, or just itself. */
    fun groupMembers(instanceId: String): Set<String> {
        val group = byInstanceId[instanceId]?.groupId ?: return setOfNotNull(
            byInstanceId[instanceId]?.instanceId,
        )
        return controls.filter { it.groupId == group }.mapTo(linkedSetOf()) { it.instanceId }
    }

    companion object {
        /**
         * Instance-based documents.
         *
         * Version 1 was the sparse override map over an immutable template.
         * Nothing writes it any more; [TouchLayoutMigration] converts one, and
         * the legacy decoder exists only to feed that conversion.
         */
        const val CURRENT_SCHEMA_VERSION = 2

        /**
         * The shipped starting point for [profile], as an ordinary document.
         *
         * A normal value with no special status at runtime — Reset Layout is
         * literally "replace the document with a fresh copy of this one", which
         * is why the factory layout can never be damaged by an edit.
         *
         * Instance ids are the catalog ids. Deterministic, readable in a stored
         * document, and stable across app versions, which is what makes the
         * migration below and the golden fixtures meaningful.
         */
        fun authoredDefault(profile: TouchControllerProfile): TouchLayoutDocument {
            val template = profile.defaultTemplate
            return TouchLayoutDocument(
                profileId = profile.id,
                templateId = template.id,
                basedOnRevision = template.templateRevision,
                controls = template.controls
                    .filter { it.inDefaultLayout }
                    .mapIndexed { index, control ->
                        TouchControlInstance(
                            instanceId = control.id,
                            catalogId = control.id,
                            anchorX = control.geometry.anchorX,
                            anchorY = control.geometry.anchorY,
                            offsetXUnits = control.geometry.groupOffsetXUnits,
                            offsetYUnits = control.geometry.groupOffsetYUnits,
                            zIndex = index,
                            groupId = control.editGroupId,
                        )
                    },
            )
        }
    }
}

/** One thing wrong with a stored document, and whether it cost an instance. */
data class TouchDocumentFinding(val message: String, val dropped: Boolean)

/**
 * A document after validation, plus what had to be changed to make it usable.
 *
 * Repair rather than refusal wherever a repair is unambiguous: one corrupt
 * instance must not cost the user a whole layout. [degraded] is true whenever
 * anything was dropped, so a surface can say so instead of silently presenting
 * a layout that is missing a control.
 */
data class TouchDocumentValidation(
    val document: TouchLayoutDocument,
    val findings: List<TouchDocumentFinding> = emptyList(),
) {
    val degraded: Boolean get() = findings.any { it.dropped }
    val problem: String? get() = findings.firstOrNull()?.message
}

/**
 * Structural validation and repair for a stored or imported document.
 *
 * Everything here is about the DOCUMENT being well formed — identity, catalog
 * references, numeric sanity. Whether the resulting arrangement is playable is a
 * separate question answered by [TouchLayoutAudit] against resolved geometry,
 * and the two must not be merged: a layout can be perfectly well formed and
 * still have two controls on top of each other.
 */
object TouchLayoutDocumentValidator {

    fun validate(
        document: TouchLayoutDocument,
        profile: TouchControllerProfile,
    ): TouchDocumentValidation {
        val findings = mutableListOf<TouchDocumentFinding>()
        if (document.profileId != profile.id) {
            return TouchDocumentValidation(
                TouchLayoutDocument.authoredDefault(profile),
                listOf(
                    TouchDocumentFinding(
                        "That layout belongs to another controller",
                        dropped = true,
                    ),
                ),
            )
        }
        val seen = linkedSetOf<String>()
        val repaired = mutableListOf<TouchControlInstance>()
        document.controls.forEach { instance ->
            if (instance.instanceId.isBlank()) {
                findings += TouchDocumentFinding("A control has no identity", dropped = true)
                return@forEach
            }
            if (!seen.add(instance.instanceId)) {
                findings += TouchDocumentFinding(
                    "Control '${instance.instanceId}' appears more than once",
                    dropped = true,
                )
                return@forEach
            }
            val entry = profile.catalogEntry(instance.catalogId)
            if (entry == null) {
                // The catalog no longer has this control -- an app downgrade, or
                // a personality that dropped it. Only this instance is lost; the
                // rest of the document, and every OTHER instance id, is intact.
                findings += TouchDocumentFinding(
                    "This controller no longer has a '${instance.catalogId}' control",
                    dropped = true,
                )
                return@forEach
            }
            val clean = repair(instance, entry)
            if (clean == null) {
                findings += TouchDocumentFinding(
                    "Control '${instance.instanceId}' has an impossible position",
                    dropped = true,
                )
                return@forEach
            }
            repaired += clean
        }
        return TouchDocumentValidation(document.copy(controls = repaired), findings)
    }

    /**
     * Bring one instance inside the stored limits, or reject it.
     *
     * Values that are merely out of range are clamped: a document written by a
     * build with different limits is still describing something the user made.
     * A value that is not a number at all is rejected, because there is no
     * defensible position to clamp it to and drawing at NaN takes the whole
     * layout with it.
     */
    private fun repair(
        instance: TouchControlInstance,
        entry: TouchTemplateControl,
    ): TouchControlInstance? {
        val finite = instance.anchorX.isFinite() && instance.anchorY.isFinite() &&
            instance.offsetXUnits.isFinite() && instance.offsetYUnits.isFinite() &&
            instance.scale.isFinite() && instance.rotationDegrees.isFinite()
        if (!finite) return null
        return instance.copy(
            anchorX = instance.anchorX.coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
            anchorY = instance.anchorY.coerceIn(TouchLayoutLimits.ANCHOR_RANGE),
            offsetXUnits = instance.offsetXUnits
                .coerceIn(-TouchLayoutLimits.MAX_OFFSET_UNITS, TouchLayoutLimits.MAX_OFFSET_UNITS),
            offsetYUnits = instance.offsetYUnits
                .coerceIn(-TouchLayoutLimits.MAX_OFFSET_UNITS, TouchLayoutLimits.MAX_OFFSET_UNITS),
            scale = instance.scale.coerceIn(TouchLayoutLimits.MIN_SCALE, TouchLayoutLimits.MAX_SCALE),
            rotationDegrees = TouchLayoutLimits.normalizeRotation(instance.rotationDegrees),
            groupId = instance.groupId?.takeIf { it.isNotBlank() },
            // A control that cannot hold must not carry a stored opinion about
            // holding: it would be a setting the editor never shows and nothing
            // ever reads.
            latch = instance.latch?.takeIf { entry.interaction.supportsLatch },
        )
    }
}

/**
 * Version 1 sparse override -> version 2 instance document.
 *
 * Deterministic and one-way. Given the same template and the same stored
 * override this always produces the same document, including the same instance
 * ids, which is what makes the golden migration fixtures worth having.
 *
 * The rules, in the order they matter:
 *
 * ```text
 * legacy control hidden      -> no instance at all; Add Control can bring it back
 * legacy control visible     -> one instance, id = the template control id
 * anchor override            -> the instance's anchor
 * scale override             -> the instance's scale
 * groupOffsetScale override  -> baked into the instance's offset
 * latch override             -> the instance's latch
 * template editGroupId       -> the instance's group
 * ```
 *
 * Overrides naming a control the template no longer has are dropped, exactly as
 * the version 1 composer already ignored them.
 */
object TouchLayoutMigration {

    fun fromOverride(
        profile: TouchControllerProfile,
        override: TouchLayoutOverride,
    ): TouchLayoutDocument {
        val template = profile.defaultTemplate
        if (override.profileId != profile.id || override.templateId != template.id) {
            return TouchLayoutDocument.authoredDefault(profile)
        }
        val controls = mutableListOf<TouchControlInstance>()
        template.controls.forEachIndexed { index, control ->
            val change = override.controls[control.id]
            // The version 1 visibility rule, unchanged: absent means the
            // template's own answer, and for an optional control that answer is
            // "not placed".
            val visible = change?.visible ?: control.inDefaultLayout
            if (!visible) return@forEachIndexed
            val geometry = control.geometry
            val offsetScale = change?.groupOffsetScale ?: 1f
            controls += TouchControlInstance(
                instanceId = control.id,
                catalogId = control.id,
                anchorX = change?.anchorX ?: geometry.anchorX,
                anchorY = change?.anchorY ?: geometry.anchorY,
                offsetXUnits = geometry.groupOffsetXUnits * offsetScale,
                offsetYUnits = geometry.groupOffsetYUnits * offsetScale,
                scale = change?.scale ?: 1f,
                zIndex = index,
                groupId = control.editGroupId,
                latch = change?.latch,
            )
        }
        return TouchLayoutDocument(
            profileId = profile.id,
            templateId = template.id,
            basedOnRevision = override.basedOnRevision.coerceAtMost(template.templateRevision),
            controls = controls,
        )
    }
}
