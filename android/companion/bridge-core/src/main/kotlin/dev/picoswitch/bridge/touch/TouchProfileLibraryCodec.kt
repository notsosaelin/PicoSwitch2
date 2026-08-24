package dev.picoswitch.bridge.touch

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

sealed interface TouchProfileLibraryDecodeResult {
    data class Valid(val value: TouchProfileLibrary) : TouchProfileLibraryDecodeResult
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
 * Two documents, one control schema. Each profile's `controls` object is written
 * and read by [TouchLayoutOverrideJsonCodec]'s shared helpers, so there is
 * exactly one definition of what a stored control override may contain and one
 * set of range checks guarding it.
 *
 * ```text
 * library document                    exported profile document
 * {                                   {
 *   schemaVersion, personality,         schemaVersion, kind, personality,
 *   selectedProfileId,                  name, templateId, templateRevision,
 *   profiles: [ profile, ... ]          controls: { id: {...} }
 * }                                   }
 * ```
 *
 * The factory profile never appears in either. It is synthesized from the
 * shipped template, which is what makes it impossible for a stored document to
 * overwrite, rename or delete it.
 */
object TouchProfileLibraryJsonCodec {
    const val CURRENT_SCHEMA_VERSION = 1

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
            if (schema != CURRENT_SCHEMA_VERSION) {
                return TouchProfileLibraryDecodeResult.Invalid(
                    if (schema > CURRENT_SCHEMA_VERSION) "Layout profiles were written by a newer app"
                    else "Layout profile schema $schema has no sequential migration",
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
                when (val decoded = decodeProfileBody(objectValue, personality)) {
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
            if (schema != CURRENT_SCHEMA_VERSION) {
                return TouchProfileDecodeResult.Invalid(
                    if (schema > CURRENT_SCHEMA_VERSION) "That layout file was written by a newer app"
                    else "Layout file schema $schema has no sequential migration",
                )
            }
            if (root.string("kind") != EXPORT_KIND) {
                return TouchProfileDecodeResult.Invalid("That file is not an exported touch layout")
            }
            val personality = TouchProfileId.fromKey(root.string("personality"))
                ?: return TouchProfileDecodeResult.Invalid("That layout file names an unknown controller")
            return decodeProfileBody(root, personality)
        }
    }

    private fun encodeProfile(profile: TouchLayoutProfile): JsonObject = buildJsonObject {
        putProfileBody(profile)
    }

    private fun JsonObjectBuilder.putProfileBody(
        profile: TouchLayoutProfile,
    ) {
        put("id", profile.id)
        put("name", profile.name)
        put("templateId", profile.templateId)
        put("templateRevision", profile.templateRevision)
        put("createdAtEpochMs", profile.metadata.createdAtEpochMs)
        put("updatedAtEpochMs", profile.metadata.updatedAtEpochMs)
        profile.metadata.gameKey?.let { put("gameKey", it) }
        put("controls", TouchLayoutOverrideJsonCodec.encodeControls(profile.override.controls))
    }

    /**
     * One profile body, from either document.
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
            val controlsObject = root["controls"] as? JsonObject
                ?: return TouchProfileDecodeResult.Invalid("A layout profile has no controls object")
            val controls = when (
                val decoded = TouchLayoutOverrideJsonCodec.decodeControls(controlsObject)
            ) {
                is TouchLayoutOverrideJsonCodec.ControlsDecode.Bad ->
                    return TouchProfileDecodeResult.Invalid(decoded.problem)
                is TouchLayoutOverrideJsonCodec.ControlsDecode.Ok -> decoded.controls
            }
            return TouchProfileDecodeResult.Valid(
                TouchLayoutProfile(
                    id = id,
                    // Names are sanitized on the way IN as well as on the way out:
                    // a hand-edited document must not be able to put control
                    // characters or an unbounded string into the profile picker.
                    name = TouchProfileLibraryEditor.sanitizeName(name),
                    override = TouchLayoutOverride(
                        schemaVersion = TouchLayoutOverride.CURRENT_SCHEMA_VERSION,
                        profileId = personality,
                        templateId = templateId,
                        basedOnRevision = revision,
                        controls = controls,
                    ),
                    metadata = TouchProfileMetadata(
                        createdAtEpochMs = root.long("createdAtEpochMs")?.coerceAtLeast(0L) ?: 0L,
                        updatedAtEpochMs = root.long("updatedAtEpochMs")?.coerceAtLeast(0L) ?: 0L,
                        gameKey = root.string("gameKey")?.takeIf { it.isNotBlank() },
                    ),
                ),
            )
        }
    }

    /** Placeholder identity for an import; the receiving library allocates the real one. */
    private const val IMPORTED_ID = "imported"
}
