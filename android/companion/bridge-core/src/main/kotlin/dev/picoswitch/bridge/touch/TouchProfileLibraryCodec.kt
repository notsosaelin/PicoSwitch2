package dev.picoswitch.bridge.touch

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.floatOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

sealed interface TouchProfileLibraryDecodeResult {
    data class Valid(
        val value: TouchProfileLibrary,
        /** True when a schema-1 document was migrated on the way in. */
        val migrated: Boolean = false,
    ) : TouchProfileLibraryDecodeResult

    data class Invalid(val problem: String) : TouchProfileLibraryDecodeResult
}

sealed interface TouchProfileDecodeResult {
    data class Valid(val value: TouchLayoutProfile) : TouchProfileDecodeResult
    data class Invalid(val problem: String) : TouchProfileDecodeResult
}

/**
 * The persisted shape of a personality's profile set, and of a single exported
 * profile.
 *
 * ```text
 * library document                    exported profile document
 * {                                   {
 *   schemaVersion: 2, personality,      schemaVersion: 2, kind, personality,
 *   selectedProfileId,                  name, templateId, templateRevision,
 *   profiles: [ profile, ... ]          controls: [ instance, ... ]
 * }                                   }
 * ```
 *
 * ## Two schema versions, one direction
 *
 * Version 1 wrote each profile's `controls` as an OBJECT keyed by template
 * control id — the sparse override model. Version 2 writes an ARRAY of
 * independently identified instances. A version 1 document is still read, and is
 * migrated through [TouchLayoutMigration] as it is decoded; nothing writes
 * version 1 again. The distinction is visible in the JSON itself, which is what
 * makes a half-migrated document impossible rather than merely unlikely.
 *
 * The factory profile never appears in either document. It is synthesized from
 * the shipped template, which is what makes it impossible for stored data to
 * overwrite, rename or delete it.
 */
object TouchProfileLibraryJsonCodec {
    const val CURRENT_SCHEMA_VERSION = 2

    /** The retired sparse-override document, still readable for migration. */
    const val LEGACY_SCHEMA_VERSION = 1

    /** Marks an exported single profile so an unrelated JSON file is refused early. */
    const val EXPORT_KIND = "picoswitch.touch.profile"

    private val json = Json { ignoreUnknownKeys = true }

    fun encode(library: TouchProfileLibrary): String = buildJsonObject {
        put("schemaVersion", CURRENT_SCHEMA_VERSION)
        put("personality", library.personality.key)
        put("selectedProfileId", library.selectedProfileId)
        put("profiles", buildJsonArray { library.userProfiles.forEach { add(encodeProfile(it)) } })
    }.toString()

    fun decode(raw: String, personality: TouchProfileId): TouchProfileLibraryDecodeResult =
        runCatching { decodeLibrary(raw, personality) }.getOrElse {
            TouchProfileLibraryDecodeResult.Invalid("Layout profiles contain malformed JSON values")
        }

    /** A single profile as a standalone, shareable document. */
    fun encodeExport(profile: TouchLayoutProfile): String = buildJsonObject {
        put("schemaVersion", CURRENT_SCHEMA_VERSION)
        put("kind", EXPORT_KIND)
        put("personality", profile.personality.key)
        putProfileBody(profile)
    }.toString()

    fun decodeExport(raw: String): TouchProfileDecodeResult =
        runCatching { decodeExportDocument(raw) }.getOrElse {
            TouchProfileDecodeResult.Invalid("That layout file contains malformed JSON values")
        }

    private fun decodeLibrary(
        raw: String,
        personality: TouchProfileId,
    ): TouchProfileLibraryDecodeResult {
        val root = runCatching { json.parseToJsonElement(raw).jsonObject }.getOrElse {
            return TouchProfileLibraryDecodeResult.Invalid("Layout profiles are not valid JSON")
        }
        with(TouchLayoutOverrideJsonCodec) {
            val schema = root.int("schemaVersion")
                ?: return TouchProfileLibraryDecodeResult.Invalid("Layout profiles have no schema version")
            if (schema > CURRENT_SCHEMA_VERSION) {
                return TouchProfileLibraryDecodeResult.Invalid("Layout profiles were written by a newer app")
            }
            if (schema < LEGACY_SCHEMA_VERSION) {
                return TouchProfileLibraryDecodeResult.Invalid(
                    "Layout profile schema $schema has no sequential migration",
                )
            }
            val stored = TouchProfileId.fromKey(root.string("personality"))
                ?: return TouchProfileLibraryDecodeResult.Invalid("Layout profiles name an unknown controller")
            if (stored != personality) {
                return TouchProfileLibraryDecodeResult.Invalid(
                    "Layout profiles belong to another controller",
                )
            }
            val array = root["profiles"] as? JsonArray
                ?: return TouchProfileLibraryDecodeResult.Invalid("Layout profiles have no profile list")
            if (array.size > TouchProfileLibrary.MAX_USER_PROFILES) {
                return TouchProfileLibraryDecodeResult.Invalid("Layout profiles exceed the supported count")
            }
            val profiles = mutableListOf<TouchLayoutProfile>()
            val ids = mutableSetOf<String>()
            array.forEach { element ->
                val objectValue = element as? JsonObject
                    ?: return TouchProfileLibraryDecodeResult.Invalid("A layout profile is not an object")
                when (val decoded = decodeProfileBody(objectValue, personality, schema)) {
                    is TouchProfileDecodeResult.Invalid ->
                        return TouchProfileLibraryDecodeResult.Invalid(decoded.problem)
                    is TouchProfileDecodeResult.Valid -> {
                        if (!ids.add(decoded.value.id)) {
                            return TouchProfileLibraryDecodeResult.Invalid(
                                "Layout profiles contain a duplicate profile id",
                            )
                        }
                        profiles += decoded.value
                    }
                }
            }
            // A selection naming a profile that is not here resolves to the
            // factory default rather than failing the whole document: the
            // layouts themselves are still perfectly usable.
            val requested = root.string("selectedProfileId")
            val selected = requested?.takeIf { id ->
                id == TouchProfileLibrary.FACTORY_PROFILE_ID || profiles.any { it.id == id }
            } ?: TouchProfileLibrary.FACTORY_PROFILE_ID
            return TouchProfileLibraryDecodeResult.Valid(
                TouchProfileLibrary(personality, profiles, selected),
                migrated = schema != CURRENT_SCHEMA_VERSION,
            )
        }
    }

    private fun decodeExportDocument(raw: String): TouchProfileDecodeResult {
        val root = runCatching { json.parseToJsonElement(raw).jsonObject }.getOrElse {
            return TouchProfileDecodeResult.Invalid("That layout file is not valid JSON")
        }
        with(TouchLayoutOverrideJsonCodec) {
            val schema = root.int("schemaVersion")
                ?: return TouchProfileDecodeResult.Invalid("That layout file has no schema version")
            if (schema > CURRENT_SCHEMA_VERSION) {
                return TouchProfileDecodeResult.Invalid("That layout file was written by a newer app")
            }
            if (schema < LEGACY_SCHEMA_VERSION) {
                return TouchProfileDecodeResult.Invalid(
                    "Layout file schema $schema has no sequential migration",
                )
            }
            if (root.string("kind") != EXPORT_KIND) {
                return TouchProfileDecodeResult.Invalid("That file is not an exported touch layout")
            }
            val personality = TouchProfileId.fromKey(root.string("personality"))
                ?: return TouchProfileDecodeResult.Invalid("That layout file names an unknown controller")
            return decodeProfileBody(root, personality, schema)
        }
    }

    private fun encodeProfile(profile: TouchLayoutProfile): JsonObject = buildJsonObject {
        putProfileBody(profile)
    }

    private fun JsonObjectBuilder.putProfileBody(profile: TouchLayoutProfile) {
        put("id", profile.id)
        put("name", profile.name)
        put("templateId", profile.templateId)
        put("templateRevision", profile.templateRevision)
        put("createdAtEpochMs", profile.metadata.createdAtEpochMs)
        put("updatedAtEpochMs", profile.metadata.updatedAtEpochMs)
        profile.metadata.gameKey?.let { put("gameKey", it) }
        put("controls", encodeInstances(profile.document.controls))
    }

    /**
     * Instances, in document order, with defaulted fields omitted.
     *
     * Omitting defaults keeps a stored layout readable and small: a control the
     * user only moved writes an anchor and nothing else. The decoder supplies
     * exactly the same defaults, so a round trip is lossless.
     */
    internal fun encodeInstances(controls: List<TouchControlInstance>): JsonArray = buildJsonArray {
        controls.forEach { instance ->
            add(
                buildJsonObject {
                    put("instanceId", instance.instanceId)
                    put("catalogId", instance.catalogId)
                    put("anchorX", instance.anchorX)
                    put("anchorY", instance.anchorY)
                    if (instance.offsetXUnits != 0f) put("offsetXUnits", instance.offsetXUnits)
                    if (instance.offsetYUnits != 0f) put("offsetYUnits", instance.offsetYUnits)
                    if (instance.scale != 1f) put("scale", instance.scale)
                    if (instance.rotationDegrees != 0f) {
                        put("rotationDegrees", instance.rotationDegrees)
                    }
                    put("zIndex", instance.zIndex)
                    instance.groupId?.let { put("groupId", it) }
                    instance.latch?.let { put("latch", it) }
                },
            )
        }
    }

    internal sealed interface InstancesDecode {
        data class Ok(val controls: List<TouchControlInstance>) : InstancesDecode
        data class Bad(val problem: String) : InstancesDecode
    }

    /**
     * One instance array.
     *
     * Range checks live here, once, so a hand-edited or imported document has to
     * satisfy exactly what an editor operation would have enforced. Structural
     * repair — duplicate identities, catalog entries that no longer exist — is
     * deliberately NOT done here: it needs the personality catalog, and
     * [TouchLayoutDocumentValidator] owns it.
     */
    internal fun decodeInstances(array: JsonArray): InstancesDecode {
        val controls = mutableListOf<TouchControlInstance>()
        array.forEach { element ->
            val value = element as? JsonObject
                ?: return InstancesDecode.Bad("A layout control is not an object")
            val instanceId = value.text("instanceId")
                ?: return InstancesDecode.Bad("A layout control has no identity")
            val catalogId = value.text("catalogId")
                ?: return InstancesDecode.Bad("Control '$instanceId' names no control type")
            val anchorX = value.finite("anchorX")
                ?: return InstancesDecode.Bad("Control '$instanceId' has an invalid anchorX")
            val anchorY = value.finite("anchorY")
                ?: return InstancesDecode.Bad("Control '$instanceId' has an invalid anchorY")
            if (anchorX !in TouchLayoutLimits.ANCHOR_RANGE ||
                anchorY !in TouchLayoutLimits.ANCHOR_RANGE
            ) {
                return InstancesDecode.Bad("Control '$instanceId' has an out-of-range anchor")
            }
            val offsetX = value.optionalFinite("offsetXUnits")
                ?: return InstancesDecode.Bad("Control '$instanceId' has an invalid offsetXUnits")
            val offsetY = value.optionalFinite("offsetYUnits")
                ?: return InstancesDecode.Bad("Control '$instanceId' has an invalid offsetYUnits")
            if (kotlin.math.abs(offsetX) > TouchLayoutLimits.MAX_OFFSET_UNITS ||
                kotlin.math.abs(offsetY) > TouchLayoutLimits.MAX_OFFSET_UNITS
            ) {
                return InstancesDecode.Bad("Control '$instanceId' has an out-of-range offset")
            }
            val scale = value.optionalFinite("scale", fallback = 1f)
                ?: return InstancesDecode.Bad("Control '$instanceId' has an invalid scale")
            if (scale !in TouchLayoutLimits.MIN_SCALE..TouchLayoutLimits.MAX_SCALE) {
                return InstancesDecode.Bad("Control '$instanceId' has an out-of-range scale")
            }
            val rotation = value.optionalFinite("rotationDegrees")
                ?: return InstancesDecode.Bad("Control '$instanceId' has an invalid rotation")
            val zIndex = value["zIndex"]?.jsonPrimitive?.content?.toIntOrNull() ?: controls.size
            val latch = value["latch"]?.jsonPrimitive?.booleanOrNull
                ?: if (value.containsKey("latch")) {
                    return InstancesDecode.Bad("Control '$instanceId' has an invalid latch flag")
                } else null
            controls += TouchControlInstance(
                instanceId = instanceId,
                catalogId = catalogId,
                anchorX = anchorX,
                anchorY = anchorY,
                offsetXUnits = offsetX,
                offsetYUnits = offsetY,
                scale = scale,
                rotationDegrees = TouchLayoutLimits.normalizeRotation(rotation),
                zIndex = zIndex,
                groupId = value.text("groupId"),
                latch = latch,
            )
        }
        return InstancesDecode.Ok(controls)
    }

    /**
     * One profile body, from either document and either schema version.
     *
     * An exported document has no `id` of its own worth trusting — importing is
     * always an insert into some other library — so a missing id is accepted and
     * the caller re-allocates one. A LIBRARY document without ids would be a
     * library whose selection cannot mean anything, so ids are required there;
     * that distinction is enforced by [decodeLibrary] rejecting duplicates and
     * by the import path never reusing what it read.
     */
    private fun decodeProfileBody(
        root: JsonObject,
        personality: TouchProfileId,
        schema: Int,
    ): TouchProfileDecodeResult {
        with(TouchLayoutOverrideJsonCodec) {
            val id = root.string("id")?.takeIf { it.isNotBlank() } ?: IMPORTED_ID
            if (id == TouchProfileLibrary.FACTORY_PROFILE_ID) {
                return TouchProfileDecodeResult.Invalid(
                    "A stored layout profile claims the reserved default id",
                )
            }
            val name = root.string("name")?.takeIf { it.isNotBlank() }
                ?: return TouchProfileDecodeResult.Invalid("A layout profile has no name")
            val templateId = root.string("templateId")?.takeIf { it.isNotBlank() }
                ?: return TouchProfileDecodeResult.Invalid("A layout profile has no template id")
            val revision = root.int("templateRevision")?.takeIf { it >= 1 }
                ?: return TouchProfileDecodeResult.Invalid("A layout profile has an invalid template revision")
            val document = when (schema) {
                LEGACY_SCHEMA_VERSION -> when (
                    val migrated = decodeLegacyDocument(root, personality, templateId, revision)
                ) {
                    is TouchProfileDecodeResult.Invalid -> return migrated
                    is TouchProfileDecodeResult.Valid -> migrated.value.document
                }
                else -> {
                    val array = root["controls"] as? JsonArray
                        ?: return TouchProfileDecodeResult.Invalid("A layout profile has no controls list")
                    when (val decoded = decodeInstances(array)) {
                        is InstancesDecode.Bad ->
                            return TouchProfileDecodeResult.Invalid(decoded.problem)
                        is InstancesDecode.Ok -> TouchLayoutDocument(
                            profileId = personality,
                            templateId = templateId,
                            basedOnRevision = revision,
                            controls = decoded.controls,
                        )
                    }
                }
            }
            return TouchProfileDecodeResult.Valid(
                TouchLayoutProfile(
                    id = id,
                    // Names are sanitized on the way IN as well as on the way out:
                    // a hand-edited document must not be able to put control
                    // characters or an unbounded string into the profile picker.
                    name = TouchProfileLibraryEditor.sanitizeName(name),
                    document = document,
                    metadata = TouchProfileMetadata(
                        createdAtEpochMs = root.long("createdAtEpochMs")?.coerceAtLeast(0L) ?: 0L,
                        updatedAtEpochMs = root.long("updatedAtEpochMs")?.coerceAtLeast(0L) ?: 0L,
                        gameKey = root.string("gameKey")?.takeIf { it.isNotBlank() },
                    ),
                ),
            )
        }
    }

    /**
     * A schema-1 profile body, migrated on the way in.
     *
     * Reuses the retired override decoder — including its range checks — so a
     * legacy document has to have been valid legacy data before it becomes a
     * valid instance document. Migration is applied against the CURRENT catalog,
     * which is what lets a control that has since been retired disappear from
     * the migrated layout instead of becoming a dangling instance.
     */
    private fun decodeLegacyDocument(
        root: JsonObject,
        personality: TouchProfileId,
        templateId: String,
        revision: Int,
    ): TouchProfileDecodeResult {
        val controlsObject = root["controls"] as? JsonObject
            ?: return TouchProfileDecodeResult.Invalid("A layout profile has no controls object")
        val controls = when (
            val decoded = TouchLayoutOverrideJsonCodec.decodeControls(controlsObject)
        ) {
            is TouchLayoutOverrideJsonCodec.ControlsDecode.Bad ->
                return TouchProfileDecodeResult.Invalid(decoded.problem)
            is TouchLayoutOverrideJsonCodec.ControlsDecode.Ok -> decoded.controls
        }
        val profile = TouchProfileCatalog.require(personality)
        val document = TouchLayoutMigration.fromOverride(
            profile,
            TouchLayoutOverride(
                schemaVersion = TouchLayoutOverride.CURRENT_SCHEMA_VERSION,
                profileId = personality,
                templateId = templateId,
                basedOnRevision = revision,
                controls = controls,
            ),
        )
        return TouchProfileDecodeResult.Valid(
            TouchLayoutProfile(id = IMPORTED_ID, name = IMPORTED_ID, document = document),
        )
    }

    private fun JsonObject.text(key: String): String? =
        this[key]?.jsonPrimitive?.content?.takeIf { it.isNotBlank() }

    private fun JsonObject.finite(key: String): Float? =
        this[key]?.jsonPrimitive?.floatOrNull?.takeIf { it.isFinite() }

    /** Absent means the default; present-but-unreadable is an error, so null is the failure. */
    private fun JsonObject.optionalFinite(key: String, fallback: Float = 0f): Float? =
        if (containsKey(key)) finite(key) else fallback

    /** Placeholder identity for an import; the receiving library allocates the real one. */
    private const val IMPORTED_ID = "imported"
}
