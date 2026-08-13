package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.AmiiboLibraryItem
import kotlinx.serialization.json.*
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets
import java.time.Instant
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * The private Android library exchange is deliberately the same flat archive
 * shape as the portal: a v3 `library.json` plus one `.bin` per entry. The
 * manifest is metadata only; the images are authoritative and are validated
 * before a replacement is committed.
 */
object AmiiboLibraryArchive {
    const val FORMAT = "PicoSwitch2 Amiibo Library"
    const val VERSION = 3
    const val MAX_ARCHIVE_BYTES = 8 * 1024 * 1024
    const val MAX_ENTRIES = 128
    const val MAX_MANIFEST_BYTES = 128 * 1024
    const val MAX_IMAGE_BYTES = 2048
    const val MAX_TOTAL_IMAGE_BYTES = 256 * 1024
    const val MAX_NAME_CHARS = 120

    data class ExportItem(
        val item: AmiiboLibraryItem,
        val bytes: ByteArray,
        val loaded: Boolean,
    )

    data class ImportedEntry(
        val fileName: String,
        val displayName: String,
        val bytes: ByteArray,
        val loaded: Boolean,
    )

    fun write(items: List<ExportItem>): ByteArray {
        require(items.size <= MAX_ENTRIES) { "Amiibo library has too many entries" }
        val usedNames = mutableSetOf("library.json")
        val manifestEntries = mutableListOf<JsonObject>()
        val imageFiles = mutableListOf<Pair<String, ByteArray>>()
        items.forEach { exported ->
            AmiiboFiles.validate(exported.bytes)
            require(exported.bytes.size <= MAX_IMAGE_BYTES) { "Amiibo image exceeds the archive limit" }
            val base = safeBaseName(exported.item.displayName, exported.item.figureId)
            val fileName = uniqueName(base, usedNames)
            imageFiles += fileName to exported.bytes.copyOf()
            manifestEntries += buildJsonObject {
                put("file", fileName)
                put("id", exported.item.figureId)
                put("uid", exported.item.uid)
                put("name", exported.item.displayName.take(MAX_NAME_CHARS))
                put("loaded", exported.loaded)
            }
        }
        val manifest = buildJsonObject {
            put("format", FORMAT)
            put("version", VERSION)
            put("exportedAt", Instant.now().toString())
            put("loadedKey", items.firstOrNull { it.loaded }?.let { portalKey(it.bytes) }.orEmpty())
            put("entries", buildJsonArray { manifestEntries.forEach(::add) })
        }.toString().encodeToByteArray()
        require(manifest.size <= MAX_MANIFEST_BYTES) { "Amiibo library manifest is too large" }
        val output = ByteArrayOutputStream()
        ZipOutputStream(output, StandardCharsets.UTF_8).use { zip ->
            // The portal places library.json first. Doing the same also keeps
            // the simple browser reader compatible with Android exports.
            putStored(zip, "library.json", manifest)
            imageFiles.forEach { (fileName, bytes) -> putStored(zip, fileName, bytes) }
        }
        val result = output.toByteArray()
        require(result.size <= MAX_ARCHIVE_BYTES) { "Amiibo library archive is too large" }
        return result
    }

    /** Parse and validate every archive entry without touching the local store. */
    fun read(raw: ByteArray): List<ImportedEntry> {
        require(raw.size <= MAX_ARCHIVE_BYTES) { "Amiibo library archive is too large" }
        require(raw.size >= 4 && raw[0] == 'P'.code.toByte() && raw[1] == 'K'.code.toByte()) {
            "Amiibo library backup is not a ZIP archive"
        }
        val images = mutableListOf<Pair<String, ByteArray>>()
        var manifestBytes: ByteArray? = null
        val names = mutableSetOf<String>()
        var totalImageBytes = 0
        var imageCount = 0
        var entryCount = 0
        ZipInputStream(ByteArrayInputStream(raw), StandardCharsets.UTF_8).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                entryCount++
                require(entryCount <= MAX_ENTRIES + 1) { "Amiibo library archive has too many entries" }
                val name = safeEntryName(entry)
                require(names.add(name.lowercase())) { "Amiibo library archive contains duplicate entries" }
                if (entry.isDirectory) {
                    zip.closeEntry()
                    continue
                }
                when {
                    name == "library.json" -> {
                        require(manifestBytes == null) { "Amiibo library archive has duplicate manifests" }
                        manifestBytes = readBounded(zip, MAX_MANIFEST_BYTES, "library manifest")
                    }
                    name.endsWith(".bin", ignoreCase = true) -> {
                        imageCount++
                        require(imageCount <= MAX_ENTRIES) { "Amiibo library archive has too many images" }
                        val bytes = readBounded(zip, MAX_IMAGE_BYTES, "Amiibo image")
                        totalImageBytes += bytes.size
                        require(totalImageBytes <= MAX_TOTAL_IMAGE_BYTES) { "Amiibo library archive contains too much image data" }
                        val normalized = AmiiboFiles.normalizeImport(bytes)
                        images += name to normalized
                    }
                    else -> error("Unsupported Amiibo library entry: $name")
                }
                zip.closeEntry()
            }
        }
        require(images.isNotEmpty()) { "Amiibo library archive contains no valid .bin files" }
        val manifest = manifestBytes?.let(::parseManifest)
        val manifestByFile = manifest?.entries?.associateBy { it.fileName }.orEmpty()
        val loadedFile = manifest?.entries?.firstOrNull { it.loaded }?.fileName
            ?: manifest?.loadedKey?.let { wanted ->
                images.firstOrNull { portalKey(it.second) == wanted }?.first
            }
        return images.map { (fileName, bytes) ->
            val metadata = manifestByFile[fileName]
            ImportedEntry(
                fileName = fileName,
                displayName = (metadata?.name ?: fileName.substringBeforeLast('.'))
                    .trim().take(MAX_NAME_CHARS).ifBlank { "Imported Amiibo" },
                bytes = bytes,
                loaded = fileName == loadedFile,
            )
        }
    }

    private data class ManifestEntry(
        val fileName: String,
        val name: String,
        val loaded: Boolean,
    )

    private data class Manifest(val entries: List<ManifestEntry>, val loadedKey: String?)

    private fun parseManifest(bytes: ByteArray): Manifest {
        val root = Json.parseToJsonElement(bytes.decodeToString()).jsonObject
        require(root["format"]?.jsonPrimitive?.content == FORMAT) { "Unsupported Amiibo library format" }
        require(root["version"]?.jsonPrimitive?.intOrNull == VERSION) { "Unsupported Amiibo library version" }
        val entries = root["entries"]?.jsonArray ?: JsonArray(emptyList())
        require(entries.size <= MAX_ENTRIES) { "Amiibo library manifest has too many entries" }
        val seen = mutableSetOf<String>()
        val loadedKey = root["loadedKey"]?.jsonPrimitive?.content?.takeIf { it.length <= 200 }
        return Manifest(entries.map { value ->
            val objectValue = value.jsonObject
            val file = objectValue["file"]?.jsonPrimitive?.content ?: error("Manifest entry has no file")
            require(file != "library.json" && file.endsWith(".bin", ignoreCase = true)) {
                "Manifest entry has an invalid image file"
            }
            safeEntryName(file)
            require(seen.add(file.lowercase())) { "Manifest contains duplicate image files" }
            ManifestEntry(
                fileName = file,
                name = objectValue["name"]?.jsonPrimitive?.content.orEmpty().take(MAX_NAME_CHARS),
                loaded = objectValue["loaded"]?.jsonPrimitive?.booleanOrNull == true,
            )
        }, loadedKey)
    }

    private fun putStored(zip: ZipOutputStream, name: String, bytes: ByteArray) {
        val entry = ZipEntry(name).apply {
            method = ZipEntry.STORED
            size = bytes.size.toLong()
            crc = java.util.zip.CRC32().apply { update(bytes) }.value
        }
        zip.putNextEntry(entry)
        zip.write(bytes)
        zip.closeEntry()
    }

    private fun readBounded(zip: ZipInputStream, limit: Int, what: String): ByteArray {
        val output = ByteArrayOutputStream()
        val buffer = ByteArray(1024)
        while (true) {
            val count = zip.read(buffer)
            if (count < 0) break
            if (output.size() + count > limit) error("$what exceeds the archive limit")
            output.write(buffer, 0, count)
        }
        return output.toByteArray()
    }

    private fun safeEntryName(entry: ZipEntry): String = safeEntryName(entry.name)

    private fun safeEntryName(name: String): String {
        require(name.isNotBlank() && name.length <= MAX_NAME_CHARS + 16) { "Unsafe Amiibo archive filename" }
        require(!name.contains('/') && !name.contains('\\') && name != "." && !name.startsWith(".")) {
            "Unsafe Amiibo archive filename"
        }
        require(name.none { it == '\u0000' || it.isISOControl() }) { "Unsafe Amiibo archive filename" }
        return name
    }

    private fun safeBaseName(value: String, fallback: String): String {
        val base = value.trim().ifBlank { "Amiibo ${fallback.take(16)}" }
            .replace(Regex("[^A-Za-z0-9._ -]"), "_")
            .trim().take(MAX_NAME_CHARS).ifBlank { "Amiibo" }
        return if (base.endsWith(".bin", ignoreCase = true)) base else "$base.bin"
    }

    private fun uniqueName(base: String, used: MutableSet<String>): String {
        var candidate = base
        var suffix = 2
        while (!used.add(candidate.lowercase())) {
            val stem = base.removeSuffix(".bin")
            candidate = "$stem ($suffix).bin"
            suffix++
        }
        return candidate
    }

    private fun portalKey(bytes: ByteArray): String {
        val identity = AmiiboCrypto.identity(bytes)
        return if (identity.tagType == dev.picoswitch.companion.model.AmiiboTagType.FigureV3) {
            "amiibo:${identity.figureId}:${AmiiboFiles.crc32(bytes).lowercase()}"
        } else {
            "amiibo:${identity.figureId}"
        }
    }
}
