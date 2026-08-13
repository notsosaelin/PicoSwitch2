package dev.picoswitch.companion.data

import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class AmiiboLibraryStoreTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun `exact duplicate returns stable item without another file`() = runTest {
        val root = temporary.newFolder("library")
        val store = AmiiboLibraryStore(root)
        val first = store.import("First", "one.bin", validImage())
        val duplicate = store.import("Different name", "two.bin", validImage())
        assertFalse(first.duplicate)
        assertTrue(duplicate.duplicate)
        assertEquals(first.item.id, duplicate.item.id)
        assertEquals(1, store.items.value.size)
        assertEquals(1, root.listFiles { file -> file.extension == "bin" }!!.size)
    }

    @Test fun `same name with different content remains two backups`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("collisions"))
        store.import("Kirby", "same.bin", validImage(1))
        store.import("Kirby", "same.bin", validImage(2))
        assertEquals(2, store.items.value.size)
        assertEquals(2, store.items.value.map { it.id }.distinct().size)
    }

    @Test fun `versioned index survives restart and rename delete`() = runTest {
        val root = temporary.newFolder("restart")
        val store = AmiiboLibraryStore(root)
        val item = store.import("Original", "one.bin", validImage()).item
        store.rename(item.id, "Renamed")
        val json = Json.parseToJsonElement(root.resolve("library.json").readText()).jsonObject
        assertEquals(AmiiboLibraryStore.SCHEMA_VERSION, json["version"]!!.jsonPrimitive.content.toInt())
        val reopened = AmiiboLibraryStore(root)
        assertEquals("Renamed", reopened.items.value.single().displayName)
        assertArrayEquals(validImage(), reopened.bytes(item.id))
        reopened.delete(item.id)
        assertTrue(AmiiboLibraryStore(root).items.value.isEmpty())
    }

    @Test fun `valid orphan is recovered after malformed index`() = runTest {
        val root = temporary.newFolder("recovery")
        val original = AmiiboLibraryStore(root).import("Backup", "one.bin", validImage()).item
        root.resolve("library.json").writeText("{broken")
        val recovered = AmiiboLibraryStore(root)
        assertEquals(original.id, recovered.items.value.single().id)
        assertTrue(recovered.items.value.single().displayName.startsWith("Recovered Amiibo"))
        assertTrue(recovered.warnings.value.any { it.contains("recovered", true) })
    }

    @Test fun `corrupt private file is retained but never returned as valid`() = runTest {
        val root = temporary.newFolder("corrupt")
        val item = AmiiboLibraryStore(root).import("Backup", "one.bin", validImage()).item
        val file = root.resolve(item.fileName)
        file.writeBytes(ByteArray(540))
        val reopened = AmiiboLibraryStore(root)
        assertTrue(reopened.items.value.isEmpty())
        assertTrue(file.exists())
        assertTrue(reopened.warnings.value.isNotEmpty())
    }

    @Test fun `invalid import leaves no dump or index entry`() = runTest {
        val root = temporary.newFolder("failed")
        val store = AmiiboLibraryStore(root)
        val error = runCatching { store.import("Bad", "bad.bin", ByteArray(2049)) }.exceptionOrNull()
        assertTrue(error is IllegalArgumentException)
        assertTrue(store.items.value.isEmpty())
        assertEquals(0, root.listFiles { file -> file.extension == "bin" }!!.size)
    }

    @Test fun `sync with a different uid creates a new entry instead of overwriting selection`() = runTest {
        val store = AmiiboLibraryStore(temporary.newFolder("identity"))
        val original = store.import("Original", "one.bin", validImage(uidByte = 1)).item
        val synced = store.updateFromAdapter(original.id, validImage(uidByte = 2))
        assertNotEquals(original.id, synced.id)
        assertEquals(2, store.items.value.size)
        assertArrayEquals(validImage(uidByte = 1), store.bytes(original.id))
    }

    private fun validImage(change: Int = 0, uidByte: Int = 0): ByteArray = ByteArray(540).apply {
        this[0] = 4
        this[1] = uidByte.toByte()
        this[3] = (0x88 xor 4 xor uidByte).toByte()
        this[8] = 0
        this[0x54] = 1
        this[100] = change.toByte()
    }
}
