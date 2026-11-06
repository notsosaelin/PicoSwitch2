package dev.picoswitch.companion.data

import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

class AmiiboLibraryArchiveTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `portal compatible zip round trips all private images and selected marker`() = runTest {
        val root = temporary.newFolder("export")
        val store = AmiiboLibraryStore(root)
        val first = store.import("Kirby / Warp", "one.bin", validImage(1)).item
        val second = store.import("Mario", "two.bin", validImage(2, uidByte = 2)).item

        val archive = store.exportArchive(second.id)
        val zip = ZipInputStream(archive.inputStream()).use { input ->
            buildList {
                while (true) {
                    val entry = input.nextEntry ?: break
                    add(entry.name)
                    input.closeEntry()
                }
            }
        }
        assertEquals(listOf("library.json", "Mario.bin", "Kirby _ Warp.bin"), zip)
        val manifest = ZipInputStream(archive.inputStream()).use { input ->
            assertEquals("library.json", input.nextEntry.name)
            Json.parseToJsonElement(input.readBytes().decodeToString()).jsonObject
        }
        assertEquals(AmiiboLibraryArchive.FORMAT, manifest["format"]!!.jsonPrimitive.content)
        assertEquals(AmiiboLibraryArchive.VERSION, manifest["version"]!!.jsonPrimitive.content.toInt())
        assertEquals("amiibo:${second.figureId}", manifest["loadedKey"]!!.jsonPrimitive.content)

        val restored = AmiiboLibraryStore(temporary.newFolder("import"))
        val result = restored.importArchive(archive)
        assertEquals(2, result.items.size)
        assertEquals("Mario", restored.items.value.first().displayName)
        assertEquals("Kirby / Warp", restored.items.value.last().displayName)
        assertEquals(result.selectedId, restored.items.value.first().id)
        assertArrayEquals(validImage(2, uidByte = 2), restored.bytes(result.selectedId!!))
        assertArrayEquals(validImage(1), restored.bytes(restored.items.value.last().id))
        assertTrue(first.id != second.id)
    }

    @Test fun `a traversal path cannot escape because it is never used as a path`() = runTest {
        // The rule CHANGED and the guarantee got stronger. Archive entry names
        // used to be refused outright, which also refused every ordinary
        // collection zip that keeps dumps in folders. Now the name is display
        // text only: the library writes every image under a generated name, so
        // there is no path to traverse with.
        val root = temporary.newFolder("preserve")
        val store = AmiiboLibraryStore(root)
        val existing = store.import("Keep", "keep.bin", validImage()).item
        val hostile = zipOf("../escape.bin" to validImage(9))

        val result = store.importMany(listOf(AmiiboImportSource("hostile.zip", hostile)))

        // The tag is imported -- it is a perfectly good dump -- and lands inside
        // the library root under a generated name.
        val imported = result.imported.single()
        assertEquals("${imported.id}.bin", imported.fileName)
        assertTrue(root.resolve(imported.fileName).isFile)
        assertFalse(root.resolve("escape.bin").exists())
        assertFalse(root.parentFile.resolve("escape.bin").exists())

        // And the existing entry is untouched.
        assertArrayEquals(validImage(), store.bytes(existing.id))
    }

    @Test fun `any zip holding dumps is readable, in folders and beside other files`() = runTest {
        // NOT limited to our own export format. People keep amiibo dumps in
        // ordinary zips, in folders, next to readmes and cover art.
        val store = AmiiboLibraryStore(temporary.newFolder("collection"))
        val collection = zipOf(
            "Animal Crossing/Tom Nook.bin" to validImage(1),
            "Kirby/Air Riders/Kirby.bin" to validImage(2, uidByte = 2),
            "readme.txt" to "my dumps".encodeToByteArray(),
            "covers/nook.png" to byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47),
        )

        val result = store.importMany(listOf(AmiiboImportSource("collection.zip", collection)))

        assertEquals(2, result.imported.size)
        assertEquals(0, result.skipped)
        // Named from the file, not the folder path.
        assertTrue(result.imported.any { it.displayName == "Tom Nook" })
    }

    @Test fun `one bad dump does not cost the rest`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("mixed"))
        val mixed = zipOf(
            "good.bin" to validImage(1),
            "bogus.bin" to ByteArray(540),
            "also-good.bin" to validImage(2, uidByte = 2),
        )

        val result = store.importMany(listOf(AmiiboImportSource("mixed.zip", mixed)))

        assertEquals(2, result.imported.size)
    }

    @Test fun `bulk import mixes dumps and archives and counts what it skipped`() = runTest {
        // The user should never have to know, or tell the app, which kind of
        // file they are holding. Non-tag files are skipped rather than fatal,
        // because pointing this at a folder is EXPECTED to meet them.
        val store = AmiiboLibraryStore(temporary.newFolder("bulk"))
        val result = store.importMany(
            listOf(
                AmiiboImportSource("Tom Nook.bin", validImage(1)),
                AmiiboImportSource("archive.zip", zipOf("Kirby.bin" to validImage(2, uidByte = 2))),
                AmiiboImportSource("readme.txt", "hello".encodeToByteArray()),
            ),
        )

        assertEquals(2, result.imported.size)
        assertEquals(1, result.skipped)
        assertEquals(0, result.duplicates)
        assertTrue(result.summary.contains("2 added"))
        assertTrue(result.summary.contains("1 skipped"))
    }

    @Test fun `bulk import reports what it already had rather than storing it twice`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("dupes"))
        store.import("Tom Nook", "a.bin", validImage(1))

        val result = store.importMany(
            listOf(
                AmiiboImportSource("Tom Nook.bin", validImage(1)),
                AmiiboImportSource("Kirby.bin", validImage(2, uidByte = 2)),
            ),
        )

        assertEquals(1, result.imported.size)
        assertEquals(1, result.duplicates)
        assertEquals(2, store.items.value.size)
        assertTrue(result.summary.contains("already in your library"))
    }

    @Test fun `importing nothing says so rather than claiming success`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("empty"))
        val result = store.importMany(emptyList())
        assertEquals(0, result.considered)
        assertEquals("Nothing to import.", result.summary)
    }

    @Test fun `oversized image and unsupported entries fail closed`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("limits"))
        val oversized = zipOf("too-big.bin" to ByteArray(AmiiboLibraryArchive.MAX_IMAGE_BYTES + 1))
        assertTrue(runCatching { store.importArchive(oversized) }.isFailure)
        assertTrue(store.items.value.isEmpty())

        // A zip with nothing usable in it is still refused: unsupported entries
        // are now SKIPPED rather than fatal, so the failure comes from there
        // being no tag images at all.
        val unsupported = zipOf("notes.txt" to "private notes".encodeToByteArray())
        assertTrue(runCatching { store.importArchive(unsupported) }.isFailure)
        assertTrue(store.items.value.isEmpty())

        val tooMany = zipOf(*Array(AmiiboLibraryArchive.MAX_ENTRIES + 1) { index ->
            "item-$index.bin" to validImage(index)
        })
        assertTrue(runCatching { store.importArchive(tooMany) }.isFailure)
        assertTrue(store.items.value.isEmpty())
    }

    private fun zipOf(vararg entries: Pair<String, ByteArray>): ByteArray =
        ByteArrayOutputStream().also { output ->
            ZipOutputStream(output).use { zip ->
                entries.forEach { (name, bytes) ->
                    zip.putNextEntry(ZipEntry(name))
                    zip.write(bytes)
                    zip.closeEntry()
                }
            }
        }.toByteArray()

    private fun validImage(change: Int = 0, uidByte: Int = 0): ByteArray = ByteArray(540).apply {
        this[0] = 4
        this[1] = uidByte.toByte()
        this[3] = (0x88 xor 4 xor uidByte).toByte()
        this[8] = 0
        this[0x54] = 1
        this[100] = change.toByte()
    }
}
