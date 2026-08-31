package dev.picoswitch.management

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** A layout's canonical starting point. */
data class KbmDefaultMapping(
    val bindings: List<KbmBinding> = emptyList(),
    val mouse: KbmMouseConfig = KbmMouseConfig(),
)

/**
 * The firmware's canonical default mapping for each layout, shipped in the app.
 *
 * WHY THIS IS IN THE APP AT ALL.
 *
 * A local profile stores only SPARSE OVERRIDES — the same representation the
 * adapter stores, which is what makes an assignment a copy rather than a
 * translation. To draw one, or to compute what a key actually does, those
 * overrides have to be applied against the canonical table they are sparse
 * against.
 *
 * Fetching that table from the adapter would make creating and editing a profile
 * require a connection, which is wrong: the library belongs to the user, not to
 * a device, and the app stays useful with nothing paired — exactly as the Amiibo
 * library does.
 *
 * The table is generated from src/ns2_kbm.c by tools/test_ns2_kbm_commands.c and
 * shipped from tools/fixtures, so there is one authority and no second
 * hand-maintained copy. KbmDefaultsTest asserts the shipped copy still matches.
 */
object KbmDefaults {
    private const val RESOURCE = "/kbm-default-mappings.json"

    private val loaded: Map<KbmProfile, KbmDefaultMapping> by lazy { load() }

    fun forLayout(layout: KbmProfile): KbmDefaultMapping =
        loaded[layout] ?: KbmDefaultMapping()

    /**
     * A layout's full effective mapping: the canonical table with a profile's
     * overrides applied over it.
     *
     * The one place the two halves are combined, so the grid, the fingerprint and
     * an upload cannot disagree about what a profile means. An override whose
     * destination is [KbmDestination.None] is kept rather than dropped: "this key
     * does nothing" is a real answer and differs from the default the adapter
     * would otherwise apply.
     */
    fun effective(layout: KbmProfile, overrides: List<KbmBinding>): List<KbmBinding> {
        val byCode = LinkedHashMap<Pair<KbmSourceKind, Int>, KbmBinding>()
        for (binding in forLayout(layout).bindings) {
            byCode[binding.source.kind to binding.source.code] = binding
        }
        for (binding in overrides) {
            byCode[binding.source.kind to binding.source.code] = binding.copy(custom = true)
        }
        return byCode.values.sortedWith(
            compareBy(
                { KbmFingerprint.firmwareCode(it.source.kind) },
                { it.source.code },
            ),
        )
    }

    private fun load(): Map<KbmProfile, KbmDefaultMapping> {
        val text = KbmDefaults::class.java.getResourceAsStream(RESOURCE)
            ?.bufferedReader()?.use { it.readText() }
            // A build without the resource degrades to an empty table rather than
            // failing to start. The parity test is what stops that happening
            // silently in a shipped build.
            ?: return emptyMap()

        return runCatching {
            val layouts = Json.parseToJsonElement(text)
                .jsonObject["layouts"]?.jsonObject ?: return@runCatching emptyMap()

            buildMap {
                for ((name, value) in layouts) {
                    val layout = KbmProfile.fromWire(name) ?: continue
                    val entry = value.jsonObject

                    val bindings = entry["bindings"]?.jsonArray.orEmpty().mapNotNull { row ->
                        val item = row.jsonObject
                        val source = item["src"]?.jsonPrimitive?.content?.let(KbmSource::parse)
                        val destination =
                            KbmDestination.fromWire(item["dst"]?.jsonPrimitive?.content)
                        if (source == null || destination == null) {
                            null
                        } else {
                            KbmBinding(source, destination, custom = false)
                        }
                    }

                    val m = entry["mouse"]?.jsonObject
                    val mouse = if (m == null) {
                        KbmMouseConfig()
                    } else {
                        KbmMouseConfig(
                            sensitivityX = m["sensitivityX"]?.jsonPrimitive?.int ?: 0,
                            sensitivityY = m["sensitivityY"]?.jsonPrimitive?.int ?: 0,
                            velocityWindowMs = m["velocityWindowMs"]?.jsonPrimitive?.int ?: 0,
                            invertX = m["invertX"]?.jsonPrimitive?.boolean ?: false,
                            invertY = m["invertY"]?.jsonPrimitive?.boolean ?: false,
                            antiDeadzone = m["antiDeadzone"]?.jsonPrimitive?.int ?: 0,
                        )
                    }

                    put(layout, KbmDefaultMapping(bindings, mouse))
                }
            }
        }.getOrElse { emptyMap() }
    }
}
