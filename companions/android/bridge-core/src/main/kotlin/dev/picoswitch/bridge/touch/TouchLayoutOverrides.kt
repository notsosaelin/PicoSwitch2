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

/**
 * The RETIRED schema-1 layout document: a sparse override map over an immutable
 * template.
 *
 * Kept for exactly one reason — reading what earlier builds wrote, so
 * [TouchLayoutMigration] can turn it into an instance document. Nothing writes
 * it as a user's live layout any more, and nothing should: the model it encodes
 * cannot express a duplicate control, a deleted one, a free rotation or an
 * arbitrary group.
 *
 * The ENCODER survives for one reason: it is what pins the decoder. A round trip
 * through both is the only check that this reader still accepts exactly what
 * earlier builds wrote, and losing that would mean discovering on somebody's
 * device that an upgrade had quietly thrown their layout away. It is not a way
 * to produce new documents, and nothing outside the tests calls it.
 */
data class TouchLayoutOverride(
    val schemaVersion: Int = CURRENT_SCHEMA_VERSION,
    val profileId: TouchProfileId,
    val templateId: String,
    val basedOnRevision: Int,
    /** Unknown ids remain here and are ignored by migration. */
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

/**
 * Read-only access to whatever a pre-2.0 build left behind.
 *
 * No write side, deliberately: this schema is a migration SOURCE, and a store
 * that could still write it would eventually be written to. Invalid raw
 * documents are reported, never deleted — a later build may understand one, and
 * the runtime is safe in the meantime because the authored default needs
 * nothing from storage.
 */
interface TouchLayoutOverrideStore {
    fun load(profileId: TouchProfileId): TouchOverrideDecodeResult?
}
