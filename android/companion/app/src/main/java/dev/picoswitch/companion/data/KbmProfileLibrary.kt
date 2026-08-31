package dev.picoswitch.companion.data

import dev.picoswitch.management.KbmBinding
import dev.picoswitch.management.KbmDestination
import dev.picoswitch.management.KbmMouseConfig
import dev.picoswitch.management.KbmProfile
import dev.picoswitch.management.KbmSource
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put

/**
 * One profile in the APP'S OWN library.
 *
 * THE DISTINCTION THIS TYPE EXISTS TO MAKE.
 *
 * The adapter holds six resident profiles — three positions in each of two
 * layout banks — because those must work with no companion attached. That is the
 * adapter's WORKING SET, not the user's collection. The library here is where a
 * user keeps as many profiles as they like; assigning one to a bank position is
 * a separate, explicit act.
 *
 * Treating the adapter's capacity as the user's capacity was the original
 * mistake, and it made "Save" mean "write to the adapter", which in turn made
 * every edit a flash write.
 *
 * [id] is a stable UUID that survives renames and is independent of any adapter
 * identity. Windows and Android libraries are separate and their ids are NOT
 * shared — the resident copy on the adapter is the only bridge, which is why
 * [fingerprint] is what gets compared across platforms rather than an id.
 *
 * No transient UI state is stored: no selection, no dirty flag, no draft.
 */
data class KbmLocalProfile(
    val id: String,
    val layout: KbmProfile,
    val name: String,
    /**
     * Sparse overrides against the layout's canonical default — the same
     * representation the adapter stores, so an assignment is a copy rather than a
     * translation.
     */
    val bindings: List<KbmBinding> = emptyList(),
    /** Profile-owned tuning. Switching profiles switches this too. */
    val mouse: KbmMouseConfig = KbmMouseConfig(),
    val fingerprint: Long = 0L,
    val modifiedMillis: Long = 0L,
)

/**
 * The user's local profiles. Deliberately unbounded.
 *
 * There is no six-profile cap here and there must never be one: six is how many
 * the ADAPTER can hold resident, and conflating the two is the defect this
 * replaces. A user with twenty profiles and an adapter holding six of them is
 * the intended shape.
 */
data class KbmProfileLibrary(val profiles: List<KbmLocalProfile> = emptyList()) {
    fun forLayout(layout: KbmProfile): List<KbmLocalProfile> =
        profiles.filter { it.layout == layout }.sortedBy { it.name.lowercase() }

    fun find(id: String): KbmLocalProfile? = profiles.firstOrNull { it.id == id }

    fun with(profile: KbmLocalProfile): KbmProfileLibrary =
        copy(profiles = profiles.filterNot { it.id == profile.id } + profile)

    fun without(id: String): KbmProfileLibrary =
        copy(profiles = profiles.filterNot { it.id == id })

    /**
     * A name not already taken in this layout, so New and Duplicate never produce
     * two rows a user cannot tell apart.
     */
    fun suggestName(layout: KbmProfile, basis: String): String {
        val taken = forLayout(layout).map { it.name.lowercase() }.toSet()
        if (basis.lowercase() !in taken) return basis
        for (n in 2..999) {
            val candidate = "$basis $n"
            if (candidate.lowercase() !in taken) return candidate
        }
        return basis
    }

    companion object {
        val EMPTY = KbmProfileLibrary()

        /** Bumped when the on-disk shape changes, so a migration can be written. */
        const val SCHEMA = 1
    }
}

/**
 * The library's on-disk form.
 *
 * Pure and Android-free, like every other codec here, so the parts that can be
 * got wrong — schema, tolerance of a damaged document, refusal to invent a
 * binding — are covered by ordinary JVM tests.
 */
object KbmProfileLibraryCodec {
    fun encode(library: KbmProfileLibrary): String {
        val root = buildJsonObject {
            put("schema", KbmProfileLibrary.SCHEMA)
            put(
                "profiles",
                buildJsonArray {
                    for (profile in library.profiles) {
                        add(
                            buildJsonObject {
                                put("id", profile.id)
                                put("layout", profile.layout.wire)
                                put("name", profile.name)
                                put("fingerprint", profile.fingerprint)
                                put("modified", profile.modifiedMillis)
                                put(
                                    "bindings",
                                    buildJsonArray {
                                        for (binding in profile.bindings) {
                                            add(
                                                buildJsonObject {
                                                    put("src", binding.source.wire)
                                                    put("dst", binding.destination.wire)
                                                },
                                            )
                                        }
                                    },
                                )
                                // Only the PROFILE-OWNED mouse values. The
                                // adapter-reported ranges belong to the firmware,
                                // not the profile, and persisting them would let
                                // a stale range outlive the adapter.
                                put(
                                    "mouse",
                                    buildJsonObject {
                                        put("sensitivityX", profile.mouse.sensitivityX)
                                        put("sensitivityY", profile.mouse.sensitivityY)
                                        put("velocityWindowMs", profile.mouse.velocityWindowMs)
                                        put("antiDeadzone", profile.mouse.antiDeadzone)
                                        put("invertX", profile.mouse.invertX)
                                        put("invertY", profile.mouse.invertY)
                                    },
                                )
                            },
                        )
                    }
                },
            )
        }
        return root.toString()
    }

    /**
     * Never throws, and never returns a partially-trusted profile.
     *
     * A row this build cannot read is SKIPPED rather than repaired into something
     * plausible: a profile with an unreadable layout or an invented binding would
     * be offered to the user as theirs and then behave in a way they never
     * configured. Losing one row loudly is better than keeping a wrong one
     * silently.
     */
    fun decode(text: String?): KbmProfileLibrary {
        if (text.isNullOrBlank()) return KbmProfileLibrary.EMPTY
        return runCatching {
            val root = Json.parseToJsonElement(text).jsonObject

            // A future schema is not readable by this build. Returning empty
            // loses the library; a misparse would silently rewrite it on the next
            // save, which is worse.
            val schema = root["schema"]?.jsonPrimitive?.int ?: 0
            if (schema > KbmProfileLibrary.SCHEMA) return KbmProfileLibrary.EMPTY

            val rows = root["profiles"] as? JsonArray ?: return KbmProfileLibrary.EMPTY
            val seen = mutableSetOf<String>()
            val profiles = rows.mapNotNull { decodeProfile(it.jsonObject) }
                // Duplicate ids would make find() and without() ambiguous.
                .filter { seen.add(it.id) }
            KbmProfileLibrary(profiles)
        }.getOrElse { KbmProfileLibrary.EMPTY }
    }

    private fun decodeProfile(row: JsonObject): KbmLocalProfile? {
        val id = row["id"]?.jsonPrimitive?.content?.takeIf { it.isNotBlank() } ?: return null
        val name = row["name"]?.jsonPrimitive?.content?.takeIf { it.isNotBlank() } ?: return null
        val layout = KbmProfile.fromWire(row["layout"]?.jsonPrimitive?.content) ?: return null

        val bindings = mutableListOf<KbmBinding>()
        for (element in row["bindings"]?.jsonArray.orEmpty()) {
            val item = element.jsonObject
            val source = item["src"]?.jsonPrimitive?.content?.let(KbmSource::parse)
            val destination = KbmDestination.fromWire(item["dst"]?.jsonPrimitive?.content)
            // An unreadable binding invalidates the whole profile: a mapping
            // missing one key is not the mapping the user saved.
            if (source == null || destination == null) return null
            bindings += KbmBinding(source, destination, custom = true)
        }

        val m = row["mouse"]?.jsonObject
        val mouse = KbmMouseConfig(
            sensitivityX = m?.get("sensitivityX")?.jsonPrimitive?.int ?: 0,
            sensitivityY = m?.get("sensitivityY")?.jsonPrimitive?.int ?: 0,
            velocityWindowMs = m?.get("velocityWindowMs")?.jsonPrimitive?.int ?: 0,
            invertX = m?.get("invertX")?.jsonPrimitive?.boolean ?: false,
            invertY = m?.get("invertY")?.jsonPrimitive?.boolean ?: false,
            antiDeadzone = m?.get("antiDeadzone")?.jsonPrimitive?.int ?: 0,
        )

        return KbmLocalProfile(
            id = id,
            layout = layout,
            name = name,
            bindings = bindings,
            mouse = mouse,
            fingerprint = row["fingerprint"]?.jsonPrimitive?.long ?: 0L,
            modifiedMillis = row["modified"]?.jsonPrimitive?.long ?: 0L,
        )
    }

    private fun JsonArray?.orEmpty(): List<JsonElement> = this ?: emptyList()
}
