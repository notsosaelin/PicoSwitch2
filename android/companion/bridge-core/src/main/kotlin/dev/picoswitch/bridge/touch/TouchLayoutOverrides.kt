package dev.picoswitch.bridge.touch

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.floatOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

data class TouchLayoutOverride(
    val schemaVersion: Int = CURRENT_SCHEMA_VERSION,
    val profileId: TouchProfileId,
    val templateId: String,
    val basedOnRevision: Int,
    /** Unknown ids remain here and are ignored by composition. */
    val controls: Map<String, TouchControlOverride> = emptyMap(),
) {
    companion object { const val CURRENT_SCHEMA_VERSION = 1 }
}

data class TouchControlOverride(
    val anchorX: Float? = null,
    val anchorY: Float? = null,
    val scale: Float? = null,
    val visible: Boolean? = null,
    /** Scales a template's group-relative offset; written only by group scaling. */
    val groupOffsetScale: Float? = null,
    /**
     * Hold-to-latch for this control: `null` follows the global setting.
     *
     * CONFIGURATION, never the hold itself. Nothing here says a control is
     * currently down; a document that could would be a document that presses a
     * button on the console the moment a dead process comes back.
     */
    val latch: Boolean? = null,
) {
    val isEmpty: Boolean get() = anchorX == null && anchorY == null && scale == null &&
        visible == null && groupOffsetScale == null && latch == null
}

sealed interface TouchOverrideDecodeResult {
    data class Valid(val value: TouchLayoutOverride) : TouchOverrideDecodeResult
    data class Invalid(val problem: String) : TouchOverrideDecodeResult
}

/** Kotlin JSON reference codec for the platform-neutral persisted document schema. */
object TouchLayoutOverrideJsonCodec {
    private val json = Json { ignoreUnknownKeys = true }

    fun encode(value: TouchLayoutOverride): String = buildJsonObject {
        put("schemaVersion", value.schemaVersion)
        put("profileId", value.profileId.key)
        put("templateId", value.templateId)
        put("basedOnRevision", value.basedOnRevision)
        put("controls", encodeControls(value.controls))
    }.toString()

    /** Shared with the profile-library codec so one schema has exactly one writer. */
    internal fun encodeControls(controls: Map<String, TouchControlOverride>): JsonObject =
        buildJsonObject {
            controls.toSortedMap().forEach { (id, control) ->
                put(id, buildJsonObject {
                    control.anchorX?.let { put("anchorX", it) }
                    control.anchorY?.let { put("anchorY", it) }
                    control.scale?.let { put("scale", it) }
                    control.visible?.let { put("visible", it) }
                    control.groupOffsetScale?.let { put("groupOffsetScale", it) }
                    control.latch?.let { put("latch", it) }
                })
            }
        }

    fun decode(raw: String): TouchOverrideDecodeResult = runCatching {
        decodeDocument(raw)
    }.getOrElse {
        TouchOverrideDecodeResult.Invalid("Layout override contains malformed JSON values")
    }

    private fun decodeDocument(raw: String): TouchOverrideDecodeResult {
        val root = runCatching { json.parseToJsonElement(raw).jsonObject }.getOrElse {
            return TouchOverrideDecodeResult.Invalid("Layout override is not valid JSON")
        }
        val schema = root.int("schemaVersion")
            ?: return TouchOverrideDecodeResult.Invalid("Layout override has no schema version")
        if (schema != TouchLayoutOverride.CURRENT_SCHEMA_VERSION) {
            return TouchOverrideDecodeResult.Invalid(
                if (schema > TouchLayoutOverride.CURRENT_SCHEMA_VERSION) {
                    "Layout override was written by a newer app"
                } else {
                    "Layout override schema $schema has no sequential migration"
                },
            )
        }
        val profile = TouchProfileId.fromKey(root.string("profileId"))
            ?: return TouchOverrideDecodeResult.Invalid("Layout override has an unknown profile")
        val templateId = root.string("templateId")?.takeIf { it.isNotBlank() }
            ?: return TouchOverrideDecodeResult.Invalid("Layout override has no template id")
        val revision = root.int("basedOnRevision")?.takeIf { it >= 1 }
            ?: return TouchOverrideDecodeResult.Invalid("Layout override has an invalid template revision")
        val controlsObject = root["controls"] as? JsonObject
            ?: return TouchOverrideDecodeResult.Invalid("Layout override has no controls object")
        return when (val controls = decodeControls(controlsObject)) {
            is ControlsDecode.Bad -> TouchOverrideDecodeResult.Invalid(controls.problem)
            is ControlsDecode.Ok -> TouchOverrideDecodeResult.Valid(
                TouchLayoutOverride(schema, profile, templateId, revision, controls.controls),
            )
        }
    }

    internal sealed interface ControlsDecode {
        data class Ok(val controls: Map<String, TouchControlOverride>) : ControlsDecode
        data class Bad(val problem: String) : ControlsDecode
    }

    /**
     * Shared with the profile-library codec.
     *
     * Every range check a stored control has to pass lives here exactly once, so
     * a profile document cannot smuggle in geometry a bare override document
     * would have been refused for.
     */
    internal fun decodeControls(controlsObject: JsonObject): ControlsDecode {
        val controls = linkedMapOf<String, TouchControlOverride>()
        controlsObject.forEach { (id, element) ->
            if (id.isBlank()) return ControlsDecode.Bad("Layout override has a blank control id")
            val objectValue = element as? JsonObject
                ?: return ControlsDecode.Bad("Override '$id' is not an object")
            val anchorX = objectValue.optionalFloat("anchorX")
                ?: if (objectValue.containsKey("anchorX")) {
                    return ControlsDecode.Bad("Override '$id' has an invalid anchorX")
                } else null
            val anchorY = objectValue.optionalFloat("anchorY")
                ?: if (objectValue.containsKey("anchorY")) {
                    return ControlsDecode.Bad("Override '$id' has an invalid anchorY")
                } else null
            val scale = objectValue.optionalFloat("scale")
                ?: if (objectValue.containsKey("scale")) {
                    return ControlsDecode.Bad("Override '$id' has an invalid scale")
                } else null
            val visible = objectValue["visible"]?.jsonPrimitive?.booleanOrNull
                ?: if (objectValue.containsKey("visible")) {
                    return ControlsDecode.Bad("Override '$id' has an invalid visible flag")
                } else null
            val groupOffsetScale = objectValue.optionalFloat("groupOffsetScale")
                ?: if (objectValue.containsKey("groupOffsetScale")) {
                    return ControlsDecode.Bad("Override '$id' has an invalid groupOffsetScale")
                } else null
            if (anchorX != null && anchorX !in 0f..1f || anchorY != null && anchorY !in 0f..1f) {
                return ControlsDecode.Bad("Override '$id' has an out-of-range anchor")
            }
            if (scale != null && scale !in TouchLayoutEditor.MIN_SCALE..TouchLayoutEditor.MAX_SCALE) {
                return ControlsDecode.Bad("Override '$id' has an out-of-range scale")
            }
            if (groupOffsetScale != null &&
                groupOffsetScale !in TouchLayoutEditor.MIN_SCALE..TouchLayoutEditor.MAX_SCALE
            ) {
                return ControlsDecode.Bad("Override '$id' has an out-of-range groupOffsetScale")
            }
            val latch = objectValue["latch"]?.jsonPrimitive?.booleanOrNull
                ?: if (objectValue.containsKey("latch")) {
                    return ControlsDecode.Bad("Override '$id' has an invalid latch flag")
                } else null
            val control = TouchControlOverride(
                anchorX = anchorX,
                anchorY = anchorY,
                scale = scale,
                visible = visible,
                groupOffsetScale = groupOffsetScale,
                latch = latch,
            )
            if (!control.isEmpty) controls[id] = control
        }
        return ControlsDecode.Ok(controls)
    }

    internal fun JsonObject.string(key: String): String? = this[key]?.jsonPrimitive?.contentOrNull
    internal fun JsonObject.int(key: String): Int? = this[key]?.jsonPrimitive?.intOrNull
    internal fun JsonObject.long(key: String): Long? = this[key]?.jsonPrimitive?.longOrNull
    private fun JsonObject.optionalFloat(key: String): Float? =
        this[key]?.jsonPrimitive?.floatOrNull?.takeIf { it.isFinite() }
}

data class TouchCompositionResult(
    val layout: TouchLayout,
    val overrideApplied: Boolean,
    val warning: String? = null,
)

/** Immutable template plus valid sparse override -> effective authored runtime layout. */
object TouchLayoutComposer {
    fun compose(
        profile: TouchControllerProfile,
        override: TouchLayoutOverride? = null,
    ): TouchCompositionResult {
        val template = profile.defaultTemplate
        val mismatch = when {
            override == null -> null
            override.schemaVersion != TouchLayoutOverride.CURRENT_SCHEMA_VERSION ->
                "Stored layout schema is not supported"
            override.profileId != profile.id -> "Stored layout belongs to another controller profile"
            override.templateId != template.id -> "Stored layout belongs to another template"
            override.basedOnRevision > template.templateRevision ->
                "Stored layout was written for a newer template revision"
            else -> null
        }
        val accepted = override?.takeIf { mismatch == null }
        val knownIds = template.controls.mapTo(mutableSetOf()) { it.id }
        val removedIds = accepted?.controls?.keys.orEmpty() - knownIds
        val revisionWarning = accepted?.takeIf {
            it.basedOnRevision < template.templateRevision && removedIds.isNotEmpty()
        }?.let {
            "Layout came from an older template revision; ${removedIds.size} removed control override(s) were ignored"
        }
        val controls = template.controls.mapNotNull { control ->
            val change = accepted?.controls?.get(control.id)
            if (change?.visible == false) return@mapNotNull null
            val geometry = control.geometry
            val scale = change?.scale ?: 1f
            val groupOffsetScale = change?.groupOffsetScale ?: 1f
            val action = profile.bindings[control.output]
                ?: return TouchCompositionResult(
                    emptyLayout(profile, template),
                    overrideApplied = false,
                    warning = "Profile has no binding for ${control.output}",
                )
            TouchControlSpec(
                id = control.id,
                kind = control.interaction,
                action = action,
                anchorX = change?.anchorX ?: geometry.anchorX,
                anchorY = change?.anchorY ?: geometry.anchorY,
                widthUnits = geometry.widthUnits * scale,
                heightUnits = geometry.heightUnits * scale,
                shape = geometry.shape,
                hitMarginUnits = geometry.hitMarginUnits * scale,
                priority = geometry.priority,
                label = control.visual.label,
                glyph = control.visual.glyph,
                output = control.output,
                visualRole = control.visual.role,
                visualRotationDegrees = control.visual.rotationDegrees,
                editGroupId = control.editGroupId,
                groupOffsetXUnits = geometry.groupOffsetXUnits * groupOffsetScale,
                groupOffsetYUnits = geometry.groupOffsetYUnits * groupOffsetScale,
                latch = change?.latch,
            )
        }
        return TouchCompositionResult(
            layout = TouchLayout(
                id = template.id,
                schemaVersion = template.schemaVersion,
                controls = controls,
                profileId = profile.id,
                templateId = template.id,
                templateRevision = template.templateRevision,
            ),
            overrideApplied = accepted != null,
            warning = mismatch ?: revisionWarning,
        )
    }

    private fun emptyLayout(profile: TouchControllerProfile, template: TouchLayoutTemplate) = TouchLayout(
        id = template.id,
        schemaVersion = template.schemaVersion,
        controls = emptyList(),
        profileId = profile.id,
        templateId = template.id,
        templateRevision = template.templateRevision,
    )
}

/**
 * Pure operations used by any host's editor.
 *
 * Every entry point takes a SELECTION — a set of control ids — rather than one
 * id, because "move these four buttons together" and "move this button" differ
 * only in the size of that set. The single-id conveniences below exist so the
 * common case reads naturally; they delegate to the same code, so a rule proven
 * for one selection holds for all of them.
 *
 * Orthogonal to the selection, [editGroup] expands each selected control to its
 * authored [TouchTemplateControl.editGroupId] cluster. A group is an EDITING
 * unit only: it never changes what a control sends.
 */
object TouchLayoutEditor {
    const val MIN_SCALE = 0.55f
    const val MAX_SCALE = 1.75f

    fun empty(profile: TouchControllerProfile) = TouchLayoutOverride(
        profileId = profile.id,
        templateId = profile.defaultTemplate.id,
        basedOnRevision = profile.defaultTemplate.templateRevision,
    )

    fun move(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        deltaX: Float,
        deltaY: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride {
        if (!deltaX.isFinite() || !deltaY.isFinite()) return current
        val byId = profile.defaultTemplate.controls.associateBy { it.id }
        val targets = targetIds(profile.defaultTemplate, selection, editGroup).mapNotNull(byId::get)
        if (targets.isEmpty()) return current
        // Clamp one delta for the whole selection. Clamping each member after the
        // move would compress the cluster against an edge and silently destroy
        // its authored relative spacing.
        val allowedX = targets.map { control ->
            val change = current.controls[control.id]
            val anchor = change?.anchorX ?: control.geometry.anchorX
            val offsetScale = change?.groupOffsetScale ?: 1f
            val center = anchor + control.geometry.groupOffsetXUnits * offsetScale /
                TouchLayoutResolver.REFERENCE_WIDTH_UNITS
            val visualScale = change?.scale ?: 1f
            val extent = (control.geometry.widthUnits / 2f + control.geometry.hitMarginUnits) *
                visualScale / TouchLayoutResolver.REFERENCE_WIDTH_UNITS
            center to extent
        }
        val allowedY = targets.map { control ->
            val change = current.controls[control.id]
            val anchor = change?.anchorY ?: control.geometry.anchorY
            val offsetScale = change?.groupOffsetScale ?: 1f
            val center = anchor + control.geometry.groupOffsetYUnits * offsetScale /
                TouchLayoutResolver.REFERENCE_HEIGHT_UNITS
            val visualScale = change?.scale ?: 1f
            val extent = (control.geometry.heightUnits / 2f + control.geometry.hitMarginUnits) *
                visualScale / TouchLayoutResolver.REFERENCE_HEIGHT_UNITS
            center to extent
        }
        val appliedX = deltaX.coerceIn(
            minimumValue = allowedX.maxOf { (center, extent) -> extent - center },
            maximumValue = allowedX.minOf { (center, extent) -> 1f - extent - center },
        )
        val appliedY = deltaY.coerceIn(
            minimumValue = allowedY.maxOf { (center, extent) -> extent - center },
            maximumValue = allowedY.minOf { (center, extent) -> 1f - extent - center },
        )
        return updateTargets(profile, current, selection, editGroup) { template, override ->
            override.copy(
                anchorX = (override.anchorX ?: template.geometry.anchorX) + appliedX,
                anchorY = (override.anchorY ?: template.geometry.anchorY) + appliedY,
            )
        }
    }

    fun move(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        deltaX: Float,
        deltaY: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride = move(profile, current, setOf(selectedId), deltaX, deltaY, editGroup)

    /** Set an absolute size multiplier; used by the slider and by numeric entry. */
    fun scale(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        scale: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride {
        if (!scale.isFinite()) return current
        return updateTargets(profile, current, selection, editGroup) { template, override ->
            val applied = scale.coerceIn(MIN_SCALE, MAX_SCALE)
            override.copy(
                scale = applied,
                groupOffsetScale = applied.takeIf { editGroup && template.editGroupId != null }
                    ?: override.groupOffsetScale,
            )
        }
    }

    fun scale(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        scale: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride = scale(profile, current, setOf(selectedId), scale, editGroup)

    /**
     * Multiply each target's current size, used by pinch.
     *
     * Relative rather than absolute because a pinch has no absolute value to
     * report and because a multi-selection has no single current size: applying
     * one absolute scale to controls the user deliberately sized differently
     * would flatten that difference on the first pinch.
     */
    fun scaleBy(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        factor: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride {
        if (!factor.isFinite() || factor <= 0f) return current
        return updateTargets(profile, current, selection, editGroup) { template, override ->
            val applied = ((override.scale ?: 1f) * factor).coerceIn(MIN_SCALE, MAX_SCALE)
            override.copy(
                scale = applied,
                groupOffsetScale = (
                    ((override.groupOffsetScale ?: 1f) * factor).coerceIn(MIN_SCALE, MAX_SCALE)
                    ).takeIf { editGroup && template.editGroupId != null }
                    ?: override.groupOffsetScale,
            )
        }
    }

    /**
     * Show or hide controls.
     *
     * Visible is the template's own answer, so showing a control DROPS the
     * override rather than storing `visible = true`. Otherwise hiding a control
     * and putting it back would leave a profile that reports itself as customized
     * while describing exactly the shipped layout — and "Reset" would appear to
     * do something when there is nothing left to reset.
     */
    fun setVisible(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        visible: Boolean,
        editGroup: Boolean,
    ): TouchLayoutOverride = updateTargets(profile, current, selection, editGroup) { _, override ->
        override.copy(visible = if (visible) null else false)
    }

    fun setVisible(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        visible: Boolean,
        editGroup: Boolean,
    ): TouchLayoutOverride = setVisible(profile, current, setOf(selectedId), visible, editGroup)

    /**
     * Choose whether these controls answer to the hold gestures.
     *
     * `null` is "follow the global setting" and, like [setVisible]'s `true`,
     * DROPS the override rather than storing an answer. Storing the value the
     * setting happens to have today would freeze it: turning the global setting
     * off later would leave this control latching on its own, for no reason the
     * user could see.
     *
     * Applied to whatever the selection expands to, including a group — a group
     * is an editing convenience and this is a per-control property, so "these
     * four buttons" is a perfectly ordinary thing to say. Nothing is shared:
     * each control still carries its own answer.
     */
    fun setLatch(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        latch: Boolean?,
        editGroup: Boolean,
    ): TouchLayoutOverride = updateTargets(profile, current, selection, editGroup) { template, override ->
        // A control that cannot latch must not carry a stored opinion about it;
        // it would be a setting the editor never shows and nothing ever reads.
        if (!template.interaction.supportsLatch) override else override.copy(latch = latch)
    }

    fun setLatch(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        latch: Boolean?,
        editGroup: Boolean,
    ): TouchLayoutOverride = setLatch(profile, current, setOf(selectedId), latch, editGroup)

    fun reset(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        editGroup: Boolean,
    ): TouchLayoutOverride {
        val ids = targetIds(profile.defaultTemplate, selection, editGroup)
        return current.copy(controls = current.controls - ids)
    }

    fun reset(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        editGroup: Boolean,
    ): TouchLayoutOverride = reset(profile, current, setOf(selectedId), editGroup)

    fun resetAll(profile: TouchControllerProfile): TouchLayoutOverride = empty(profile)

    /**
     * The controls an operation on this selection actually touches.
     *
     * Public because a host has to DRAW it: showing a bounding indicator around
     * one button while an edit silently moves four is the kind of surprise that
     * makes a direct-manipulation editor untrustworthy.
     */
    fun targetIds(
        template: TouchLayoutTemplate,
        selection: Set<String>,
        editGroup: Boolean,
    ): Set<String> {
        val byId = template.controls.associateBy { it.id }
        val result = linkedSetOf<String>()
        selection.forEach { id ->
            val selected = byId[id] ?: return@forEach
            val group = selected.editGroupId?.takeIf { editGroup }
            if (group == null) {
                result += id
            } else {
                template.controls.filterTo(mutableListOf()) { it.editGroupId == group }
                    .forEach { result += it.id }
            }
        }
        return result
    }

    private fun updateTargets(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selection: Set<String>,
        editGroup: Boolean,
        transform: (TouchTemplateControl, TouchControlOverride) -> TouchControlOverride,
    ): TouchLayoutOverride {
        if (current.profileId != profile.id || current.templateId != profile.defaultTemplate.id) {
            return current
        }
        val byId = profile.defaultTemplate.controls.associateBy { it.id }
        val next = current.controls.toMutableMap()
        targetIds(profile.defaultTemplate, selection, editGroup).forEach { id ->
            val template = byId[id] ?: return@forEach
            val changed = transform(template, next[id] ?: TouchControlOverride())
            if (changed.isEmpty) next.remove(id) else next[id] = changed
        }
        return current.copy(controls = next)
    }
}

/** Storage boundary implemented by each host.  Invalid raw documents must not be deleted on read. */
interface TouchLayoutOverrideStore {
    fun load(profileId: TouchProfileId): TouchOverrideDecodeResult?
    fun save(value: TouchLayoutOverride)
    fun delete(profileId: TouchProfileId)
}
