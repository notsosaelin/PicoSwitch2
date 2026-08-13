package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboCatalogEntry
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.*
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.UUID

/**
 * Cache-first AmiiboAPI enrichment. It stores compact catalog metadata only;
 * raw tag bytes, UIDs, keys, and decrypted fields never go to the network.
 * Network failure leaves the local library and adapter controls usable.
 */
class AmiiboCatalogStore(private val root: File) {
    private val cacheFile = File(root, "amiibo-catalog.json")
    private val lock = Mutex()
    private val json = Json { ignoreUnknownKeys = true }
    private var loaded = false
    private var cachedAtMillis = 0L
    private var lastNetworkAttempt = 0L
    private var byId: Map<String, AmiiboCatalogEntry> = emptyMap()
    private var titleNames: Map<String, String> = emptyMap()

    init {
        root.mkdirs()
        loadCache()
    }

    fun find(figureId: String): AmiiboCatalogEntry? {
        if (!loaded) loadCache()
        return byId[figureId.uppercase()]
    }

    fun gameNameForTitleId(titleId: String): String? = titleNames[titleId.uppercase()]

    /** Refresh stale/missing data in the background; returns whether any catalog is available. */
    suspend fun ensureLoaded(nowMillis: Long = System.currentTimeMillis()): Boolean = withContext(Dispatchers.IO) {
        lock.withLock {
            if (!loaded) loadCache()
            val fresh = byId.isNotEmpty() && nowMillis - cachedAtMillis < MAX_CACHE_AGE_MILLIS
            if (fresh) return@withLock true
            if (nowMillis - lastNetworkAttempt < RETRY_INTERVAL_MILLIS)
                return@withLock byId.isNotEmpty()
            lastNetworkAttempt = nowMillis
            val fetched = runCatching { fetchNetwork() }.getOrNull() ?: return@withLock byId.isNotEmpty()
            install(fetched, nowMillis)
            writeCache()
            true
        }
    }

    /** Test/support hook that uses the same compact cache format as a network response. */
    internal fun saveCached(entries: List<AmiiboCatalogEntry>, cachedAtMillis: Long = System.currentTimeMillis()) {
        install(entries, cachedAtMillis)
        writeCache()
    }

    private fun loadCache() {
        if (loaded) return
        loaded = true
        runCatching {
            val root = json.parseToJsonElement(cacheFile.readText()).jsonObject
            cachedAtMillis = root["cachedAtMillis"]?.jsonPrimitive?.longOrNull ?: 0L
            val entries = root["items"]?.jsonArray?.mapNotNull { parseCachedEntry(it.jsonObject) }.orEmpty()
            install(entries, cachedAtMillis)
        }.onFailure {
            cachedAtMillis = 0L
            byId = emptyMap()
            titleNames = emptyMap()
        }
    }

    private fun install(entries: List<AmiiboCatalogEntry>, timestamp: Long) {
        byId = entries.mapNotNull { entry ->
            val id = entry.id.uppercase().takeIf(::isFigureId) ?: return@mapNotNull null
            id to entry.copy(id = id)
        }.toMap()
        titleNames = entries.asSequence().flatMap { it.titleIds.entries.asSequence() }
            .filter { isTitleId(it.key) && it.value.isNotBlank() }
            .associate { it.key.uppercase() to it.value }
        cachedAtMillis = timestamp
    }

    private fun fetchNetwork(): List<AmiiboCatalogEntry> {
        var failure: Throwable? = null
        for (endpoint in ENDPOINTS) {
            val connection = runCatching { URL(endpoint).openConnection() as HttpURLConnection }.getOrElse {
                failure = it
                continue
            }
            try {
                connection.connectTimeout = CONNECT_TIMEOUT_MILLIS
                connection.readTimeout = READ_TIMEOUT_MILLIS
                connection.instanceFollowRedirects = true
                connection.setRequestProperty("Accept", "application/json")
                if (connection.responseCode !in 200..299) error("HTTP ${connection.responseCode}")
                val body = connection.inputStream.use { readBounded(it, MAX_RESPONSE_BYTES).toString(StandardCharsets.UTF_8) }
                val entries = parseCatalog(body)
                if (entries.isEmpty()) error("empty AmiiboAPI catalog")
                return entries
            } catch (error: Throwable) {
                failure = error
            } finally {
                connection.disconnect()
            }
        }
        throw IllegalStateException("AmiiboAPI unavailable", failure)
    }

    private fun writeCache() {
        val payload = buildJsonObject {
            put("version", 1)
            put("cachedAtMillis", cachedAtMillis)
            put("items", buildJsonArray { byId.values.forEach { add(it.toJson()) } })
        }.toString().toByteArray(StandardCharsets.UTF_8)
        val temporary = File(root, ".${cacheFile.name}.${UUID.randomUUID()}.tmp")
        try {
            temporary.outputStream().use { stream -> stream.write(payload); stream.fd.sync() }
            runCatching {
                Files.move(temporary.toPath(), cacheFile.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            }.getOrElse {
                Files.move(temporary.toPath(), cacheFile.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } finally {
            temporary.delete()
        }
    }

    private fun readBounded(input: java.io.InputStream, maxBytes: Int): ByteArray {
        val output = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) return output.toByteArray()
            if (output.size() + count > maxBytes) error("AmiiboAPI response is too large")
            output.write(buffer, 0, count)
        }
    }

    private fun parseCachedEntry(value: JsonObject): AmiiboCatalogEntry? = runCatching {
        AmiiboCatalogEntry(
            id = value.string("id").uppercase(), character = value.string("character"),
            gameSeries = value.string("gameSeries"), amiiboSeries = value.string("amiiboSeries"),
            type = value.string("type"), releaseDate = value.string("releaseDate"),
            imageUrl = value.string("imageUrl"), games = value["games"]?.jsonObject?.mapValues { pair ->
                pair.value.jsonArray.mapNotNull { it.jsonPrimitive.contentOrNull }
            }.orEmpty(), titleIds = value["titleIds"]?.jsonObject?.mapValues { pair ->
                pair.value.jsonPrimitive.contentOrNull.orEmpty()
            }.orEmpty(), name = value.string("name"),
        ).takeIf { isFigureId(it.id) }
    }.getOrNull()

    private fun AmiiboCatalogEntry.toJson() = buildJsonObject {
        put("id", id); put("character", character); put("gameSeries", gameSeries)
        put("amiiboSeries", amiiboSeries); put("type", type); put("releaseDate", releaseDate)
        put("imageUrl", imageUrl); put("name", name)
        put("games", buildJsonObject { games.forEach { (platform, names) -> put(platform, JsonArray(names.map { JsonPrimitive(it) })) } })
        put("titleIds", buildJsonObject { titleIds.forEach { (id, name) -> put(id, name) } })
    }

    private fun JsonObject.string(key: String) = this[key]?.jsonPrimitive?.contentOrNull.orEmpty()

    companion object {
        private const val CONNECT_TIMEOUT_MILLIS = 2_500
        private const val READ_TIMEOUT_MILLIS = 8_000
        private const val MAX_RESPONSE_BYTES = 4 * 1024 * 1024
        private const val RETRY_INTERVAL_MILLIS = 60_000L
        private const val MAX_CACHE_AGE_MILLIS = 7 * 24 * 60 * 60 * 1000L
        private val HEX_ID = Regex("[0-9A-F]{16}")
        private val ENDPOINTS = listOf(
            "https://amiiboapi.org/api/amiibo/?showgames",
            "https://www.amiiboapi.com/api/amiibo/?showgames",
        )

        /** Parse the documented AmiiboAPI response without any network access. */
        fun parseCatalog(payload: String): List<AmiiboCatalogEntry> {
            val root = runCatching { Json.parseToJsonElement(payload).jsonObject }.getOrNull() ?: return emptyList()
            return root["amiibo"]?.jsonArray?.mapNotNull { value ->
                runCatching { value.jsonObject }.getOrNull()?.let(::parseNetworkEntry)
            }.orEmpty()
        }

        private fun parseNetworkEntry(value: JsonObject): AmiiboCatalogEntry? = runCatching {
            val head = value["head"]?.jsonPrimitive?.contentOrNull.orEmpty()
            val tail = value["tail"]?.jsonPrimitive?.contentOrNull.orEmpty()
            // The portal deliberately keys catalog records by head+tail, not by
            // a server-side display field. Keep that mapping so IDs from local
            // tag bytes match even if an API mirror adds a different `id`.
            val suppliedId = value["id"]?.jsonPrimitive?.contentOrNull.orEmpty()
            val id = (if (head.isNotBlank() || tail.isNotBlank()) head + tail else suppliedId).uppercase()
            if (!isFigureId(id)) return@runCatching null
            val release = value["release"]?.jsonObject
            val releaseDate = listOf("na", "us", "jp", "eu", "au", "nz").asSequence()
                .mapNotNull { release?.get(it)?.jsonPrimitive?.contentOrNull }
                .filter { it.isNotBlank() }
                .sorted()
                .firstOrNull().orEmpty()
            val games = linkedMapOf<String, MutableList<String>>()
            val titleNames = linkedMapOf<String, String>()
            PLATFORM_FIELDS.forEach { (field, label) ->
                val names = games.getOrPut(label) { mutableListOf() }
                value[field]?.jsonArray?.forEach { gameValue ->
                    val game = gameValue.jsonObject
                    val name = game["gameName"]?.jsonPrimitive?.contentOrNull.orEmpty()
                    if (name.isNotBlank() && name !in names) names += name
                    game["gameID"]?.jsonArray?.forEach { title ->
                        val titleId = title.jsonPrimitive.contentOrNull?.uppercase().orEmpty()
                        if (titleId.length == 16 && name.isNotBlank()) titleNames[titleId] = name
                    }
                }
                if (names.isEmpty()) games.remove(label)
            }
            AmiiboCatalogEntry(
                id = id,
                name = value["name"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                character = value["character"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                gameSeries = value["gameSeries"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                amiiboSeries = value["amiiboSeries"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                type = value["type"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                releaseDate = releaseDate,
                imageUrl = value["image"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                games = games.mapValues { it.value.toList() },
                titleIds = titleNames,
            )
        }.getOrNull()

        private val PLATFORM_FIELDS = listOf(
            "gamesSwitch2" to "Switch 2",
            "gamesSwitch" to "Switch",
            "gamesWiiU" to "Wii U",
            "games3DS" to "3DS",
        )

        private fun isFigureId(value: String): Boolean = HEX_ID.matches(value.uppercase())

        private fun isTitleId(value: String): Boolean = HEX_ID.matches(value.uppercase())
    }
}
