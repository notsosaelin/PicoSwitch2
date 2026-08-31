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
 * The library exchange archive: a v3 `library.json` plus one `.bin` per entry.
 *
 * A CROSS-PLATFORM FORMAT, not this app's private save file. The same shape is
 * written and read by the Windows companion and by the web portal, so an archive
 * exported from any of the three opens in the other two.
 *
 * The manifest is metadata only; the images are authoritative and are validated
 * before a replacement is committed.
 */
object AmiiboLibraryArchive {
    const val FORMAT = "PicoSwitch2 Amiibo Library"
    const val VERSION = 3

    // Sized for a real collection rather than a handful of favourites. Someone
    // with a thousand tags is an ordinary user of this feature, and the previous
    // 128-entry / 256 KB ceilings turned that into an unexplained refusal. 4096
    // tags at the 2048-byte maximum is 8 MB of images, comfortably inside these
    // bounds, and the bounds still exist because a ZIP is attacker-shaped input.
    const val MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
    const val MAX_ENTRIES = 4096
    const val MAX_MANIFEST_BYTES = 1024 * 1024
    const val MAX_IMAGE_BYTES = 2048
    const val MAX_TOTAL_IMAGE_BYTES = 16 * 1024 * 1024
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

    /**
     * Read every tag image out of a ZIP, ours or anyone else's.
     *
     * DELIBERATELY NOT LIMITED TO OUR OWN EXPORT FORMAT. People keep amiibo
     * dumps in ordinary zips, usually in folders, usually alongside readme and
     * image files. Refusing those and demanding a re-export from another tool
     * would make this the most annoying way to move a collection, so the rule is
     * simply: take every `.bin` that is a valid tag, ignore everything else, and
     * fail only when there was nothing usable at all.
     *
     * Our `library.json` is still read when present, and still supplies names
     * and the loaded marker. It is metadata, never a gate.
     *
     * A pure read: the caller decides what to keep.
     */
    fun read(raw: ByteArray): List<ImportedEntry> {
        require(raw.size <= MAX_ARCHIVE_BYTES) { "This archive is too large to import" }
        require(raw.size >= 4 && raw[0] == 'P'.code.toByte() && raw[1] == 'K'.code.toByte()) {
            "That file is not a ZIP archive"
        }
        val images = mutableListOf<Pair<String, ByteArray>>()
        var manifestBytes: ByteArray? = null
        val names = mutableSetOf<String>()
        var totalImageBytes = 0
        var entryCount = 0
        ZipInputStream(ByteArrayInputStream(raw), StandardCharsets.UTF_8).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                entryCount++
                require(entryCount <= MAX_ENTRIES + 1) { "This archive has too many entries to import" }
                if (entry.isDirectory) {
                    zip.closeEntry()
                    continue
                }

                // OUR manifest sits at the root. One found anywhere else belongs
                // to some other tool and is not ours to interpret.
                if (entry.name.equals("library.json", ignoreCase = true)) {
                    require(manifestBytes == null) { "Amiibo library archive has duplicate manifests" }
                    manifestBytes = readBounded(zip, MAX_MANIFEST_BYTES, "library manifest")
                    zip.closeEntry()
                    continue
                }

                val leaf = entry.name.substringAfterLast('/').substringAfterLast('\\')
                if (!leaf.endsWith(".bin", ignoreCase = true) &&
                    !leaf.endsWith(".nfc", ignoreCase = true)
                ) {
                    // Readmes, cover images, other tools' metadata. Not an error:
                    // a collection zip is full of them.
                    zip.closeEntry()
                    continue
                }

                require(images.size < MAX_ENTRIES) { "This archive has too many Amiibo images to import" }

                // The LAST path segment, so a dump inside "Animal Crossing/" is
                // taken on its merits. Display only -- the library stores every
                // image under a generated name, so an archive path never reaches
                // the filesystem and cannot traverse out of anywhere.
                var name = displayEntryName(leaf)
                if (!names.add(name.lowercase())) {
                    // Two folders holding a same-named dump. Both are kept: they
                    // may well be different tags, and content de-duplication
                    // happens on import where it can compare bytes.
                    name = uniqueName(name, names)
                }

                val bytes = readBounded(zip, MAX_IMAGE_BYTES, "Amiibo image")
                totalImageBytes += bytes.size
                require(totalImageBytes <= MAX_TOTAL_IMAGE_BYTES) {
                    "This archive contains too much image data to import"
                }

                // A file that is not a tag is SKIPPED, not fatal. One stray .bin
                // in a folder of five hundred good ones must not cost the user
                // the other four hundred and ninety-nine.
                runCatching { AmiiboFiles.normalizeImport(bytes) }
                    .onSuccess { images += name to it }

                zip.closeEntry()
            }
        }
        require(images.isNotEmpty()) { "This archive contains no Amiibo dumps this app can read" }

        // A manifest that cannot be read costs the display names and nothing
        // else. A foreign archive may well carry a library.json of its own that
        // means something entirely different, and refusing the images over it
        // would throw away files that are perfectly valid.
        val manifest = manifestBytes?.let { runCatching { parseManifest(it) }.getOrNull() }
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

    /**
     * A safe display name for an entry in someone else's archive.
     *
     * TOTAL, unlike [safeEntryName]. This runs on names from arbitrary
     * collection zips, where an over-long or oddly-punctuated filename is a
     * normal thing to meet — throwing would cost the user the whole import over
     * one badly named file. The name is display-only: the library stores every
     * image under a generated name, so nothing here reaches the filesystem.
     *
     * `isISOControl` covers NUL, so no separate check for it is needed.
     */
    private fun displayEntryName(name: String): String {
        val cleaned = name
            .filterNot { it.isISOControl() || it == '/' || it == '\\' }
            .take(MAX_NAME_CHARS)
            .trim()
            .trimStart('.')
        return cleaned.ifBlank { "amiibo.bin" }
    }

    private fun safeEntryName(entry: ZipEntry): String = safeEntryName(entry.name)

    private fun safeEntryName(name: String): String {
        require(name.isNotBlank() && name.length <= MAX_NAME_CHARS + 16) { "Unsafe Amiibo archive filename" }
        require(!name.contains('/') && !name.contains('\\') && name != "." && !name.startsWith(".")) {
            "Unsafe Amiibo archive filename"
        }
        require(name.none { it.isISOControl() }) { "Unsafe Amiibo archive filename" }
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
