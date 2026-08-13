package dev.picoswitch.companion.data

import android.content.Context
import android.util.AtomicFile
import dev.picoswitch.companion.model.AmiiboLibraryItem
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.*
import java.io.File
import java.util.UUID

class AmiiboLibrary(context: Context) {
    private val root = File(context.filesDir, "amiibo-library").apply { mkdirs() }
    private val index = AtomicFile(File(root, "library.json"))
    private val _items = MutableStateFlow(loadIndex())
    val items: StateFlow<List<AmiiboLibraryItem>> = _items.asStateFlow()

    suspend fun import(displayName: String, sourceName: String, data: ByteArray): AmiiboLibraryItem = withContext(Dispatchers.IO) {
        val normalized = AmiiboFiles.normalizeImport(data)
        val id = UUID.randomUUID().toString()
        val now = System.currentTimeMillis()
        val fileName = "$id.bin"
        File(root, fileName).writeBytes(normalized)
        val item = AmiiboLibraryItem(
            id = id,
            displayName = displayName.trim().ifBlank { sourceName.substringBeforeLast('.').ifBlank { "Imported Amiibo" } },
            fileName = fileName,
            size = normalized.size,
            crc32 = AmiiboFiles.crc32(normalized),
            uid = AmiiboFiles.uid(normalized),
            figureId = AmiiboFiles.figureId(normalized),
            importedAtMillis = now,
        )
        persist(listOf(item) + _items.value)
        item
    }

    suspend fun updateFromAdapter(id: String?, data: ByteArray): AmiiboLibraryItem = withContext(Dispatchers.IO) {
        AmiiboFiles.validate(data)
        val existing = id?.let { wanted -> _items.value.firstOrNull { it.id == wanted } }
            ?: _items.value.firstOrNull { it.uid == AmiiboFiles.uid(data) }
        if (existing == null) return@withContext import("Synced Amiibo", "adapter.bin", data)
        File(root, existing.fileName).writeBytes(data)
        val updated = existing.copy(
            size = data.size,
            crc32 = AmiiboFiles.crc32(data),
            uid = AmiiboFiles.uid(data),
            figureId = AmiiboFiles.figureId(data),
            updatedAtMillis = System.currentTimeMillis(),
            dirtyFromAdapter = false,
        )
        persist(_items.value.map { if (it.id == updated.id) updated else it })
        updated
    }

    suspend fun rename(id: String, displayName: String) = withContext(Dispatchers.IO) {
        persist(_items.value.map { if (it.id == id) it.copy(displayName = displayName.trim().ifBlank { it.displayName }) else it })
    }

    suspend fun delete(id: String) = withContext(Dispatchers.IO) {
        val item = _items.value.firstOrNull { it.id == id } ?: return@withContext
        File(root, item.fileName).delete()
        persist(_items.value.filterNot { it.id == id })
    }

    suspend fun bytes(id: String): ByteArray = withContext(Dispatchers.IO) {
        val item = _items.value.firstOrNull { it.id == id } ?: error("Amiibo is not in the local library")
        File(root, item.fileName).readBytes().also {
            AmiiboFiles.validate(it)
            check(AmiiboFiles.crc32(it) == item.crc32) { "Local Amiibo copy failed CRC verification" }
        }
    }

    private fun loadIndex(): List<AmiiboLibraryItem> = runCatching {
        val text = index.openRead().bufferedReader().use { it.readText() }
        Json.parseToJsonElement(text).jsonArray.map { value ->
            val o = value.jsonObject
            AmiiboLibraryItem(
                id = o.s("id"), displayName = o.s("displayName"), fileName = o.s("fileName"),
                size = o.i("size"), crc32 = o.s("crc32"), uid = o.s("uid"), figureId = o.s("figureId"),
                importedAtMillis = o.l("importedAtMillis"), updatedAtMillis = o.l("updatedAtMillis"),
                dirtyFromAdapter = o.b("dirtyFromAdapter"),
            )
        }.filter { File(root, it.fileName).isFile }
    }.getOrDefault(emptyList())

    private fun persist(items: List<AmiiboLibraryItem>) {
        val json = buildJsonArray {
            items.forEach { item ->
                add(buildJsonObject {
                    put("id", item.id); put("displayName", item.displayName); put("fileName", item.fileName)
                    put("size", item.size); put("crc32", item.crc32); put("uid", item.uid); put("figureId", item.figureId)
                    put("importedAtMillis", item.importedAtMillis); put("updatedAtMillis", item.updatedAtMillis)
                    put("dirtyFromAdapter", item.dirtyFromAdapter)
                })
            }
        }.toString().encodeToByteArray()
        val output = index.startWrite()
        try {
            output.write(json)
            index.finishWrite(output)
            _items.value = items
        } catch (error: Throwable) {
            index.failWrite(output)
            throw error
        }
    }

    private fun JsonObject.s(key: String) = this[key]?.jsonPrimitive?.content.orEmpty()
    private fun JsonObject.i(key: String) = this[key]?.jsonPrimitive?.intOrNull ?: 0
    private fun JsonObject.l(key: String) = this[key]?.jsonPrimitive?.longOrNull ?: 0L
    private fun JsonObject.b(key: String) = this[key]?.jsonPrimitive?.booleanOrNull ?: false
}
