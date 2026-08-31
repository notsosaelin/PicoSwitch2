package dev.picoswitch.companion.data

import android.content.Context
import dev.picoswitch.companion.model.AmiiboLibraryItem
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.*
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.UUID

data class AmiiboImportResult(val item: AmiiboLibraryItem, val duplicate: Boolean)

data class AmiiboArchiveImportResult(
    val items: List<AmiiboLibraryItem>,
    val selectedId: String?,
)

/** One file handed to a bulk import, before anything has looked at it. */
data class AmiiboImportSource(val name: String, val bytes: ByteArray)

/**
 * What a bulk import did, in the terms a one-line summary needs.
 *
 * SKIPPED IS NOT A FAILURE. Pointing this at a folder is expected to sweep up
 * readmes, cover art and other tools' metadata, and reporting those as errors
 * would make a successful import of four hundred tags look broken. The counts
 * are what the user sees; [problems] is for diagnostics.
 */
data class AmiiboBulkImportResult(
    val imported: List<AmiiboLibraryItem>,
    val duplicates: Int,
    val skipped: Int,
    val problems: List<String>,
) {
    val considered: Int get() = imported.size + duplicates + skipped

    /** One line, in the order a person cares about. */
    val summary: String
        get() {
            if (considered == 0) return "Nothing to import."
            val parts = buildList {
                if (imported.isNotEmpty()) add("${imported.size} added")
                if (duplicates > 0) add("$duplicates already in your library")
                if (skipped > 0) add("$skipped skipped")
            }
            return if (parts.isEmpty()) "Nothing to import." else parts.joinToString(", ") + "."
        }
}

/** Private, versioned, crash-resistant Amiibo storage. Raw dumps never leave app-private files. */
class AmiiboLibrary(context: Context) {
    private val store = AmiiboLibraryStore(File(context.filesDir, "amiibo-library"))
    val items: StateFlow<List<AmiiboLibraryItem>> = store.items
    val warnings: StateFlow<List<String>> = store.warnings

    suspend fun import(displayName: String, sourceName: String, data: ByteArray) = store.import(displayName, sourceName, data)
    suspend fun updateFromAdapter(id: String?, data: ByteArray) = store.updateFromAdapter(id, data)
    suspend fun rename(id: String, displayName: String) = store.rename(id, displayName)
    suspend fun delete(id: String) = store.delete(id)
    suspend fun bytes(id: String) = store.bytes(id)
    suspend fun exportArchive(selectedId: String? = null) = store.exportArchive(selectedId)
    suspend fun importArchive(raw: ByteArray) = store.importArchive(raw)
    suspend fun importMany(sources: List<AmiiboImportSource>) = store.importMany(sources)
}

/** File-root implementation is Android-independent so recovery and transaction behavior are host-testable. */
class AmiiboLibraryStore(private val root: File) {
    private val index = File(root, "library.json")
    private val lock = Mutex()
    private val _warnings = MutableStateFlow<List<String>>(emptyList())
    val warnings: StateFlow<List<String>> = _warnings.asStateFlow()
    private val _items: MutableStateFlow<List<AmiiboLibraryItem>>
    val items: StateFlow<List<AmiiboLibraryItem>> get() = _items.asStateFlow()

    init {
        root.mkdirs()
        val loaded = loadAndRecover()
        _items = MutableStateFlow(loaded)
        runCatching { persistIndex(loaded) }.onFailure { addWarning("Library index could not be repaired: ${it.safeMessage()}") }
    }

    suspend fun import(displayName: String, sourceName: String, raw: ByteArray): AmiiboImportResult = ioLocked {
        val normalized = AmiiboFiles.normalizeImport(raw)
        val crc = AmiiboFiles.crc32(normalized)
        val identity = AmiiboCrypto.identity(normalized)
        _items.value.firstOrNull { it.size == normalized.size && it.crc32.equals(crc, true) }?.let { candidate ->
            if (runCatching { fileFor(candidate.fileName).readBytes().contentEquals(normalized) }.getOrDefault(false)) {
                return@ioLocked AmiiboImportResult(candidate, duplicate = true)
            }
        }
        val id = UUID.randomUUID().toString()
        val now = System.currentTimeMillis()
        val fileName = "$id.bin"
        val destination = fileFor(fileName)
        writeAtomic(destination, normalized)
        val item = AmiiboLibraryItem(
            id = id,
            displayName = displayName.trim().ifBlank { safeDisplayName(sourceName) },
            fileName = fileName,
            size = normalized.size,
            crc32 = crc,
            uid = identity.uid,
            figureId = identity.figureId,
            importedAtMillis = now,
            characterGameCode = identity.characterGameCode,
            characterVariant = identity.characterVariant,
            tagType = identity.tagType,
            typeName = identity.typeName,
            modelNumber = identity.modelNumber,
            seriesCode = identity.seriesCode,
            formatVersion = identity.formatVersion,
            extendedVariant = identity.extendedVariant,
        )
        val next = listOf(item) + _items.value
        try {
            persistIndex(next)
        } catch (error: Throwable) {
            destination.delete()
            throw error
        }
        _items.value = next
        AmiiboImportResult(item, duplicate = false)
    }

    suspend fun updateFromAdapter(id: String?, data: ByteArray): AmiiboLibraryItem = ioLocked {
        AmiiboFiles.validate(data)
        val uid = AmiiboFiles.uid(data)
        val figureId = AmiiboFiles.figureId(data)
        val identity = AmiiboCrypto.identity(data)
        val existing = id?.let { wanted -> _items.value.firstOrNull { it.id == wanted } }
            ?.takeIf { it.uid == uid }
            ?: _items.value.firstOrNull { it.uid == uid && it.figureId == figureId }
        if (existing == null) return@ioLocked importUnlocked("Synced Amiibo", "adapter.bin", data).item

        val destination = fileFor(existing.fileName)
        val backup = File(root, "${existing.id}.rollback")
        if (destination.isFile) Files.copy(destination.toPath(), backup.toPath(), StandardCopyOption.REPLACE_EXISTING)
        writeAtomic(destination, data)
        val updated = existing.copy(
            size = data.size,
            crc32 = AmiiboFiles.crc32(data),
            uid = uid,
            figureId = figureId,
            characterGameCode = identity.characterGameCode,
            characterVariant = identity.characterVariant,
            tagType = identity.tagType,
            typeName = identity.typeName,
            modelNumber = identity.modelNumber,
            seriesCode = identity.seriesCode,
            formatVersion = identity.formatVersion,
            extendedVariant = identity.extendedVariant,
            updatedAtMillis = System.currentTimeMillis(),
            dirtyFromAdapter = false,
        )
        val next = _items.value.map { if (it.id == updated.id) updated else it }
        try {
            persistIndex(next)
            backup.delete()
        } catch (error: Throwable) {
            if (backup.isFile) moveReplace(backup, destination)
            throw error
        }
        _items.value = next
        updated
    }

    suspend fun rename(id: String, displayName: String) = ioLocked {
        val next = _items.value.map { item ->
            if (item.id == id) item.copy(displayName = displayName.trim().ifBlank { item.displayName }) else item
        }
        persistIndex(next)
        _items.value = next
    }

    suspend fun delete(id: String) = ioLocked {
        val item = _items.value.firstOrNull { it.id == id } ?: return@ioLocked
        val source = fileFor(item.fileName)
        val tombstone = File(root, "${item.id}.deleted")
        if (source.isFile) moveReplace(source, tombstone)
        val next = _items.value.filterNot { it.id == id }
        try {
            persistIndex(next)
            tombstone.delete()
        } catch (error: Throwable) {
            if (tombstone.isFile) moveReplace(tombstone, source)
            throw error
        }
        _items.value = next
    }

    suspend fun bytes(id: String): ByteArray = withContext(Dispatchers.IO) {
        lock.withLock {
            val item = _items.value.firstOrNull { it.id == id } ?: error("Amiibo is not in the local library")
            fileFor(item.fileName).readBytes().also {
                AmiiboFiles.validate(it)
                check(it.size == item.size && AmiiboFiles.crc32(it).equals(item.crc32, true)) {
                    "Local Amiibo copy failed integrity verification"
                }
            }
        }
    }

    suspend fun exportArchive(selectedId: String? = null): ByteArray = withContext(Dispatchers.IO) {
        lock.withLock {
            val exported = _items.value.map { item ->
                AmiiboLibraryArchive.ExportItem(
                    item = item,
                    bytes = fileFor(item.fileName).readBytes().also {
                        AmiiboFiles.validate(it)
                        check(it.size == item.size && AmiiboFiles.crc32(it).equals(item.crc32, true)) {
                            "Local Amiibo copy failed integrity verification"
                        }
                    },
                    loaded = item.id == selectedId,
                )
            }
            AmiiboLibraryArchive.write(exported)
        }
    }

    /**
     * Replace the complete private library only after the whole archive has
     * been parsed, normalized, and validated. New UUID names ensure a failed
     * index write cannot overwrite an existing private dump.
     */
    suspend fun importArchive(raw: ByteArray): AmiiboArchiveImportResult = ioLocked {
        val parsed = AmiiboLibraryArchive.read(raw)
        val imported = parsed.map { entry ->
            val bytes = entry.bytes
            val identity = AmiiboCrypto.identity(bytes)
            val id = UUID.randomUUID().toString()
            AmiiboLibraryItem(
                id = id,
                displayName = entry.displayName,
                fileName = "$id.bin",
                size = bytes.size,
                crc32 = AmiiboFiles.crc32(bytes),
                uid = identity.uid,
                figureId = identity.figureId,
                importedAtMillis = System.currentTimeMillis(),
                characterGameCode = identity.characterGameCode,
                characterVariant = identity.characterVariant,
                tagType = identity.tagType,
                typeName = identity.typeName,
                modelNumber = identity.modelNumber,
                seriesCode = identity.seriesCode,
                formatVersion = identity.formatVersion,
                extendedVariant = identity.extendedVariant,
            ) to bytes
        }
        val oldItems = _items.value
        val written = mutableListOf<File>()
        try {
            imported.forEach { (item, bytes) ->
                val destination = fileFor(item.fileName)
                writeAtomic(destination, bytes)
                written += destination
            }
            persistIndex(imported.map { it.first })
        } catch (error: Throwable) {
            written.forEach { it.delete() }
            throw error
        }
        oldItems.asSequence()
            .map { it.fileName }
            .filterNot { oldName -> imported.any { it.first.fileName == oldName } }
            .forEach { oldName -> runCatching { fileFor(oldName).delete() } }
        val next = imported.map { it.first }
        _items.value = next
        val selected = parsed.zip(next).firstOrNull { it.first.loaded }?.second?.id
        AmiiboArchiveImportResult(next, selected)
    }

    /**
     * Import many files at once: tag dumps, archives, or any mix of the two.
     *
     * THE ONE ENTRY POINT EVERY PLATFORM'S BULK IMPORT USES — files a user
     * multi-selected, or everything found under a folder. Importing one tag at a
     * time is fine for a first tag and useless for a collection, and the
     * difference between "pick a file" and "pick two hundred files" should be the
     * picker, not the app.
     *
     * A `.zip` is expanded and its tags imported, so a user never has to know
     * whether what they were given is a dump or an archive of them. That includes
     * archives from other tools: see [AmiiboLibraryArchive.read].
     *
     * NOTHING HERE IS FATAL. Anything unreadable is counted as skipped and
     * recorded in [AmiiboBulkImportResult.problems], because a folder import is
     * expected to meet files that are not tags, and one of them must never cost
     * the user the rest.
     */
    suspend fun importMany(sources: List<AmiiboImportSource>): AmiiboBulkImportResult = ioLocked {
        val imported = mutableListOf<AmiiboLibraryItem>()
        val problems = mutableListOf<String>()
        var duplicates = 0
        var skipped = 0

        fun add(displayName: String, sourceName: String, bytes: ByteArray) {
            runCatching { importUnlocked(displayName, sourceName, bytes) }
                .onSuccess { result ->
                    if (result.duplicate) duplicates++ else imported += result.item
                }
                .onFailure { error ->
                    skipped++
                    problems += "'$sourceName' was not imported: ${error.safeMessage()}"
                }
        }

        sources.forEach { source ->
            val looksLikeZip = source.bytes.size >= 2 &&
                source.bytes[0] == 'P'.code.toByte() &&
                source.bytes[1] == 'K'.code.toByte()

            if (looksLikeZip) {
                runCatching { AmiiboLibraryArchive.read(source.bytes) }
                    .onSuccess { entries ->
                        entries.forEach { add(it.displayName, it.fileName, it.bytes) }
                    }
                    .onFailure { error ->
                        skipped++
                        problems += "'${source.name}' could not be read: ${error.safeMessage()}"
                    }
            } else {
                add(source.name.substringBeforeLast('.'), source.name, source.bytes)
            }
        }

        AmiiboBulkImportResult(imported, duplicates, skipped, problems)
    }

    private suspend fun <T> ioLocked(block: suspend () -> T): T = withContext(Dispatchers.IO) { lock.withLock { block() } }

    private fun importUnlocked(displayName: String, sourceName: String, raw: ByteArray): AmiiboImportResult {
        val normalized = AmiiboFiles.normalizeImport(raw)
        val crc = AmiiboFiles.crc32(normalized)
        _items.value.firstOrNull { it.size == normalized.size && it.crc32.equals(crc, true) }?.let { candidate ->
            if (runCatching { fileFor(candidate.fileName).readBytes().contentEquals(normalized) }.getOrDefault(false)) {
                return AmiiboImportResult(candidate, true)
            }
        }
        val id = UUID.randomUUID().toString()
        val now = System.currentTimeMillis()
        val identity = AmiiboCrypto.identity(normalized)
        val fileName = "$id.bin"
        val destination = fileFor(fileName)
        writeAtomic(destination, normalized)
        val item = AmiiboLibraryItem(
            id = id, displayName = displayName, fileName = fileName, size = normalized.size, crc32 = crc,
            uid = identity.uid, figureId = identity.figureId, importedAtMillis = now,
            characterGameCode = identity.characterGameCode, characterVariant = identity.characterVariant,
            tagType = identity.tagType, typeName = identity.typeName, modelNumber = identity.modelNumber,
            seriesCode = identity.seriesCode, formatVersion = identity.formatVersion,
            extendedVariant = identity.extendedVariant,
        )
        val next = listOf(item) + _items.value
        try { persistIndex(next) } catch (error: Throwable) { destination.delete(); throw error }
        _items.value = next
        return AmiiboImportResult(item, false)
    }

    private fun loadAndRecover(): List<AmiiboLibraryItem> {
        val warnings = mutableListOf<String>()
        val indexed = runCatching { parseIndex(index.readText()) }.getOrElse { error ->
            if (index.exists()) warnings += "Library index was unreadable; valid backups were recovered (${error.safeMessage()})"
            emptyList()
        }
        val result = mutableListOf<AmiiboLibraryItem>()
        val seenFiles = mutableSetOf<String>()
        indexed.forEach { item ->
            val file = runCatching { fileFor(item.fileName) }.getOrElse {
                warnings += "Ignored an unsafe library filename"
                return@forEach
            }
            if (!file.isFile) {
                warnings += "Missing local backup for ${item.displayName}"
                return@forEach
            }
            val data = runCatching { file.readBytes().also(AmiiboFiles::validate) }.getOrElse {
                warnings += "Kept unreadable backup file for ${item.displayName}; it was not deleted"
                return@forEach
            }
            val identity = AmiiboCrypto.identity(data)
            val repaired = item.copy(size = data.size, crc32 = AmiiboFiles.crc32(data), uid = AmiiboFiles.uid(data),
                figureId = AmiiboFiles.figureId(data), updatedAtMillis = item.updatedAtMillis.takeIf { it > 0 } ?: item.importedAtMillis,
                characterGameCode = identity.characterGameCode,
                characterVariant = identity.characterVariant,
                tagType = identity.tagType,
                typeName = identity.typeName,
                modelNumber = identity.modelNumber,
                seriesCode = identity.seriesCode,
                formatVersion = identity.formatVersion,
                extendedVariant = identity.extendedVariant)
            if (item.crc32.isNotBlank() && (item.size != repaired.size || !item.crc32.equals(repaired.crc32, true) || item.uid != repaired.uid)) {
                warnings += "Integrity metadata changed for ${item.displayName}; verify before using this backup"
                return@forEach
            }
            result += repaired
            seenFiles += item.fileName
        }
        root.listFiles { file -> file.isFile && UUID_FILE.matches(file.name) }?.sortedBy { it.name }?.forEach { file ->
            if (file.name in seenFiles) return@forEach
            val data = runCatching { file.readBytes().also(AmiiboFiles::validate) }.getOrNull() ?: return@forEach
            val id = file.name.removeSuffix(".bin")
            val stamp = file.lastModified().takeIf { it > 0 } ?: System.currentTimeMillis()
            val identity = AmiiboCrypto.identity(data)
            result += AmiiboLibraryItem(id, "Recovered Amiibo ${id.take(8)}", file.name, data.size,
                AmiiboFiles.crc32(data), identity.uid, identity.figureId, stamp, stamp,
                characterGameCode = identity.characterGameCode,
                characterVariant = identity.characterVariant,
                tagType = identity.tagType,
                typeName = identity.typeName,
                modelNumber = identity.modelNumber,
                seriesCode = identity.seriesCode,
                formatVersion = identity.formatVersion,
                extendedVariant = identity.extendedVariant)
            warnings += "Recovered an unindexed Amiibo backup"
        }
        _warnings.value = warnings.distinct()
        return result.distinctBy { it.id }
    }

    private fun parseIndex(text: String): List<AmiiboLibraryItem> {
        val root = Json.parseToJsonElement(text)
        val array = when (root) {
            is JsonArray -> root // v0 migration
            is JsonObject -> {
                val version = root["version"]?.jsonPrimitive?.intOrNull ?: error("Missing library schema version")
                require(version in 1..SCHEMA_VERSION) { "Unsupported library schema $version" }
                root["items"]?.jsonArray ?: error("Missing library items")
            }
            else -> error("Invalid library index")
        }
        return array.mapNotNull { value -> runCatching { value.jsonObject.toItem() }.getOrNull() }
    }

    private fun persistIndex(items: List<AmiiboLibraryItem>) {
        val payload = buildJsonObject {
            put("version", SCHEMA_VERSION)
            put("items", buildJsonArray { items.forEach { add(it.toJson()) } })
        }.toString().encodeToByteArray()
        writeAtomic(index, payload)
    }

    private fun fileFor(fileName: String): File {
        require(UUID_FILE.matches(fileName)) { "Unsafe Amiibo library filename" }
        return File(root, fileName)
    }

    private fun writeAtomic(destination: File, bytes: ByteArray) {
        val temporary = File(root, ".${destination.name}.${UUID.randomUUID()}.tmp")
        try {
            temporary.outputStream().use { stream -> stream.write(bytes); stream.fd.sync() }
            moveReplace(temporary, destination)
        } finally {
            temporary.delete()
        }
    }

    private fun moveReplace(source: File, destination: File) {
        runCatching {
            Files.move(source.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
        }.getOrElse {
            Files.move(source.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING)
        }
    }

    private fun safeDisplayName(sourceName: String): String = sourceName.substringAfterLast('/').substringAfterLast('\\')
        .substringBeforeLast('.').trim().take(120).ifBlank { "Imported Amiibo" }

    private fun AmiiboLibraryItem.toJson() = buildJsonObject {
        put("id", id); put("displayName", displayName); put("fileName", fileName); put("size", size)
        put("crc32", crc32); put("uid", uid); put("figureId", figureId); put("importedAtMillis", importedAtMillis)
        put("updatedAtMillis", updatedAtMillis); put("dirtyFromAdapter", dirtyFromAdapter)
        put("characterGameCode", characterGameCode); put("characterVariant", characterVariant)
        put("tagType", tagType.name); put("typeName", typeName); put("modelNumber", modelNumber)
        put("seriesCode", seriesCode); put("formatVersion", formatVersion); put("extendedVariant", extendedVariant)
    }

    private fun JsonObject.toItem() = AmiiboLibraryItem(
        id = s("id"), displayName = s("displayName").take(120), fileName = s("fileName"), size = i("size"),
        crc32 = s("crc32"), uid = s("uid"), figureId = s("figureId"), importedAtMillis = l("importedAtMillis"),
        updatedAtMillis = l("updatedAtMillis"), dirtyFromAdapter = b("dirtyFromAdapter"),
        characterGameCode = s("characterGameCode"), characterVariant = i("characterVariant"),
        tagType = runCatching { enumValueOf<dev.picoswitch.companion.model.AmiiboTagType>(s("tagType")) }
            .getOrDefault(dev.picoswitch.companion.model.AmiiboTagType.Ntag215),
        typeName = s("typeName").ifBlank { "Figure" }, modelNumber = s("modelNumber"),
        seriesCode = i("seriesCode"), formatVersion = i("formatVersion"), extendedVariant = s("extendedVariant"),
    ).also { require(UUID_FILE.matches(it.fileName) && it.id.isNotBlank()) }

    private fun addWarning(message: String) { _warnings.value = (_warnings.value + message).distinct() }
    private fun Throwable.safeMessage() = message?.take(160) ?: javaClass.simpleName
    private fun JsonObject.s(key: String) = this[key]?.jsonPrimitive?.content.orEmpty()
    private fun JsonObject.i(key: String) = this[key]?.jsonPrimitive?.intOrNull ?: 0
    private fun JsonObject.l(key: String) = this[key]?.jsonPrimitive?.longOrNull ?: 0L
    private fun JsonObject.b(key: String) = this[key]?.jsonPrimitive?.booleanOrNull ?: false

    companion object {
        const val SCHEMA_VERSION = 1
        private val UUID_FILE = Regex("^[0-9a-fA-F-]{36}\\.bin$")
    }
}
