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
