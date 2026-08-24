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
) {
    val isEmpty: Boolean get() = anchorX == null && anchorY == null && scale == null &&
        visible == null && groupOffsetScale == null
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
        put("controls", buildJsonObject {
            value.controls.toSortedMap().forEach { (id, control) ->
                put(id, buildJsonObject {
                    control.anchorX?.let { put("anchorX", it) }
                    control.anchorY?.let { put("anchorY", it) }
                    control.scale?.let { put("scale", it) }
                    control.visible?.let { put("visible", it) }
                    control.groupOffsetScale?.let { put("groupOffsetScale", it) }
                })
            }
        })
    }.toString()

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
        val controls = linkedMapOf<String, TouchControlOverride>()
        controlsObject.forEach { (id, element) ->
            if (id.isBlank()) return TouchOverrideDecodeResult.Invalid("Layout override has a blank control id")
            val objectValue = element as? JsonObject
                ?: return TouchOverrideDecodeResult.Invalid("Override '$id' is not an object")
            val anchorX = objectValue.optionalFloat("anchorX")
                ?: if (objectValue.containsKey("anchorX")) {
                    return TouchOverrideDecodeResult.Invalid("Override '$id' has an invalid anchorX")
                } else null
            val anchorY = objectValue.optionalFloat("anchorY")
                ?: if (objectValue.containsKey("anchorY")) {
                    return TouchOverrideDecodeResult.Invalid("Override '$id' has an invalid anchorY")
                } else null
            val scale = objectValue.optionalFloat("scale")
                ?: if (objectValue.containsKey("scale")) {
                    return TouchOverrideDecodeResult.Invalid("Override '$id' has an invalid scale")
                } else null
            val visible = objectValue["visible"]?.jsonPrimitive?.booleanOrNull
                ?: if (objectValue.containsKey("visible")) {
                    return TouchOverrideDecodeResult.Invalid("Override '$id' has an invalid visible flag")
                } else null
            val groupOffsetScale = objectValue.optionalFloat("groupOffsetScale")
                ?: if (objectValue.containsKey("groupOffsetScale")) {
                    return TouchOverrideDecodeResult.Invalid("Override '$id' has an invalid groupOffsetScale")
                } else null
            if (anchorX != null && anchorX !in 0f..1f || anchorY != null && anchorY !in 0f..1f) {
                return TouchOverrideDecodeResult.Invalid("Override '$id' has an out-of-range anchor")
            }
            if (scale != null && scale !in TouchLayoutEditor.MIN_SCALE..TouchLayoutEditor.MAX_SCALE) {
                return TouchOverrideDecodeResult.Invalid("Override '$id' has an out-of-range scale")
            }
            if (groupOffsetScale != null &&
                groupOffsetScale !in TouchLayoutEditor.MIN_SCALE..TouchLayoutEditor.MAX_SCALE
            ) {
                return TouchOverrideDecodeResult.Invalid("Override '$id' has an out-of-range groupOffsetScale")
            }
            val control = TouchControlOverride(
                anchorX = anchorX,
                anchorY = anchorY,
                scale = scale,
                visible = visible,
                groupOffsetScale = groupOffsetScale,
            )
            if (!control.isEmpty) controls[id] = control
        }
        return TouchOverrideDecodeResult.Valid(
            TouchLayoutOverride(schema, profile, templateId, revision, controls),
        )
    }

    private fun JsonObject.string(key: String): String? = this[key]?.jsonPrimitive?.contentOrNull
    private fun JsonObject.int(key: String): Int? = this[key]?.jsonPrimitive?.intOrNull
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

/** Pure operations used by any host's editor. */
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
        selectedId: String,
        deltaX: Float,
        deltaY: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride {
        val byId = profile.defaultTemplate.controls.associateBy { it.id }
        val targets = targetIds(profile.defaultTemplate, selectedId, editGroup).mapNotNull(byId::get)
        if (targets.isEmpty()) return current
        // Clamp one delta for the whole group. Clamping each member after the
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
        return updateTargets(profile, current, selectedId, editGroup) { template, override ->
            override.copy(
                anchorX = (override.anchorX ?: template.geometry.anchorX) + appliedX,
                anchorY = (override.anchorY ?: template.geometry.anchorY) + appliedY,
            )
        }
    }

    fun scale(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        scale: Float,
        editGroup: Boolean,
    ): TouchLayoutOverride = updateTargets(profile, current, selectedId, editGroup) { template, override ->
        val applied = scale.coerceIn(MIN_SCALE, MAX_SCALE)
        override.copy(
            scale = applied,
            groupOffsetScale = applied.takeIf { editGroup && template.editGroupId != null }
                ?: override.groupOffsetScale,
        )
    }

    fun setVisible(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        visible: Boolean,
        editGroup: Boolean,
    ): TouchLayoutOverride = updateTargets(profile, current, selectedId, editGroup) { _, override ->
        override.copy(visible = visible)
    }

    fun reset(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        editGroup: Boolean,
    ): TouchLayoutOverride {
        val ids = targetIds(profile.defaultTemplate, selectedId, editGroup)
        return current.copy(controls = current.controls - ids)
    }

    fun resetAll(profile: TouchControllerProfile): TouchLayoutOverride = empty(profile)

    private fun updateTargets(
        profile: TouchControllerProfile,
        current: TouchLayoutOverride,
        selectedId: String,
        editGroup: Boolean,
        transform: (TouchTemplateControl, TouchControlOverride) -> TouchControlOverride,
    ): TouchLayoutOverride {
        if (current.profileId != profile.id || current.templateId != profile.defaultTemplate.id) {
            return current
        }
        val byId = profile.defaultTemplate.controls.associateBy { it.id }
        val next = current.controls.toMutableMap()
        targetIds(profile.defaultTemplate, selectedId, editGroup).forEach { id ->
            val template = byId[id] ?: return@forEach
            val changed = transform(template, next[id] ?: TouchControlOverride())
            if (changed.isEmpty) next.remove(id) else next[id] = changed
        }
        return current.copy(controls = next)
    }

    private fun targetIds(
        template: TouchLayoutTemplate,
        selectedId: String,
        editGroup: Boolean,
    ): Set<String> {
        val selected = template.controls.firstOrNull { it.id == selectedId } ?: return emptySet()
        val group = selected.editGroupId?.takeIf { editGroup } ?: return setOf(selectedId)
        return template.controls.filter { it.editGroupId == group }.mapTo(linkedSetOf()) { it.id }
    }
}

/** Storage boundary implemented by each host.  Invalid raw documents must not be deleted on read. */
interface TouchLayoutOverrideStore {
    fun load(profileId: TouchProfileId): TouchOverrideDecodeResult?
    fun save(value: TouchLayoutOverride)
    fun delete(profileId: TouchProfileId)
}
