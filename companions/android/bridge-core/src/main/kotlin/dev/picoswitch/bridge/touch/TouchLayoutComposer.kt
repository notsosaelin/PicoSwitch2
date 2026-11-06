package dev.picoswitch.bridge.touch

/**
 * The result of turning a user document into a runtime layout.
 *
 * [customized] answers "is this the shipped arrangement?" and nothing more;
 * [warning] is the one thing a surface should tell the user when a stored
 * document could not be honoured exactly.
 */
data class TouchCompositionResult(
    val layout: TouchLayout,
    val customized: Boolean,
    val warning: String? = null,
    /** True when validation had to drop an instance to make the document usable. */
    val degraded: Boolean = false,
)

/**
 * Personality catalog + user document -> the layout the runtime plays.
 *
 * ```text
 * TouchControllerProfile      what this controller can produce
 *        + catalog entry      what such a control looks like
 * TouchLayoutDocument         which instances exist and where
 *        |
 *        v
 * TouchLayout                 a flat list of placed instances
 *        |  TouchLayoutResolver
 *        v
 * ResolvedTouchLayout         real coordinates, audited
 * ```
 *
 * The composer is the ONLY place a catalog entry and an instance transform are
 * combined, so the rules for how a user's scale, rotation and offset apply to
 * authored geometry exist exactly once.
 */
object TouchLayoutComposer {

    fun compose(
        profile: TouchControllerProfile,
        document: TouchLayoutDocument? = null,
    ): TouchCompositionResult {
        val template = profile.defaultTemplate
        val authored = TouchLayoutDocument.authoredDefault(profile)
        val mismatch = document?.let { mismatch(it, profile, template) }
        val accepted = document?.takeIf { mismatch == null } ?: authored
        val validation = TouchLayoutDocumentValidator.validate(accepted, profile)
        val effective = validation.document

        val controls = effective.controls
            .sortedBy { it.zIndex }
            .mapNotNull { instance -> place(profile, instance) }

        return TouchCompositionResult(
            layout = TouchLayout(
                id = template.id,
                schemaVersion = template.schemaVersion,
                controls = controls,
                profileId = profile.id,
                templateId = template.id,
                templateRevision = template.templateRevision,
            ),
            customized = effective.controls != authored.controls,
            warning = mismatch ?: validation.problem,
            degraded = validation.degraded,
        )
    }

    /**
     * Why this document cannot be used with this profile, if it cannot.
     *
     * A revision NEWER than the shipped catalog is refused rather than
     * best-efforted: it was written against controls this build may not have,
     * and quietly dropping them would present a layout that is missing pieces
     * without saying so.
     */
    private fun mismatch(
        document: TouchLayoutDocument,
        profile: TouchControllerProfile,
        template: TouchLayoutTemplate,
    ): String? = when {
        document.schemaVersion != TouchLayoutDocument.CURRENT_SCHEMA_VERSION ->
            "Stored layout schema is not supported"
        document.profileId != profile.id -> "Stored layout belongs to another controller"
        document.templateId != template.id -> "Stored layout belongs to another template"
        document.basedOnRevision > template.templateRevision ->
            "Stored layout was written for a newer template revision"
        else -> null
    }

    /**
     * One instance placed against its catalog entry.
     *
     * Returns null only when the personality has no binding for the entry's
     * output, which the profile's own `require(outputs == bindings.keys)` makes
     * unreachable; it is handled rather than asserted so a future catalog edit
     * costs one control instead of the whole layout.
     */
    private fun place(
        profile: TouchControllerProfile,
        instance: TouchControlInstance,
    ): TouchControlSpec? {
        val entry = profile.catalogEntry(instance.catalogId) ?: return null
        val action = profile.bindings[entry.output] ?: return null
        val geometry = entry.geometry
        val scale = instance.scale
        return TouchControlSpec(
            id = instance.instanceId,
            catalogId = entry.id,
            kind = entry.interaction,
            action = action,
            anchorX = instance.anchorX,
            anchorY = instance.anchorY,
            widthUnits = geometry.widthUnits * scale,
            heightUnits = geometry.heightUnits * scale,
            shape = geometry.shape,
            hitMarginUnits = geometry.hitMarginUnits * scale,
            priority = geometry.priority,
            zIndex = instance.zIndex,
            label = entry.visual.label,
            glyph = entry.visual.glyph,
            output = entry.output,
            visualRole = entry.visual.role,
            // Authored art direction plus the user's own turn. One total, so the
            // renderer and the hit tester cannot read different angles.
            visualRotationDegrees = TouchLayoutLimits.normalizeRotation(
                entry.visual.rotationDegrees + instance.rotationDegrees,
            ),
            authoredRotationDegrees = entry.visual.rotationDegrees,
            editGroupId = instance.groupId,
            // Already absolute: a group scale writes the scaled displacement
            // into the instance, so nothing multiplies it again here.
            groupOffsetXUnits = instance.offsetXUnits,
            groupOffsetYUnits = instance.offsetYUnits,
            latch = instance.latch,
        )
    }
}
