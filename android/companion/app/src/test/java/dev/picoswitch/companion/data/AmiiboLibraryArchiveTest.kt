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

    @Test fun `malicious archive is rejected without changing existing library`() = runTest {
        val root = temporary.newFolder("preserve")
        val store = AmiiboLibraryStore(root)
        val existing = store.import("Keep", "keep.bin", validImage()).item
        val malicious = zipOf("../escape.bin" to validImage(9))

        assertTrue(runCatching { store.importArchive(malicious) }.isFailure)
        assertEquals(listOf(existing.id), store.items.value.map { it.id })
        assertArrayEquals(validImage(), store.bytes(existing.id))
        assertFalse(root.resolve("escape.bin").exists())
    }

    @Test fun `oversized image and unsupported entries fail closed`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("limits"))
        val oversized = zipOf("too-big.bin" to ByteArray(AmiiboLibraryArchive.MAX_IMAGE_BYTES + 1))
        assertTrue(runCatching { store.importArchive(oversized) }.isFailure)
        assertTrue(store.items.value.isEmpty())

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
