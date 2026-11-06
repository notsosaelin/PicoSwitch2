package dev.picoswitch.companion.data

import dev.picoswitch.management.KbmBinding
import dev.picoswitch.management.KbmDefaults
import dev.picoswitch.management.KbmDestination
import dev.picoswitch.management.KbmFingerprint
import dev.picoswitch.management.KbmMouseConfig
import dev.picoswitch.management.KbmProfile
import dev.picoswitch.management.KbmSource
import dev.picoswitch.management.KbmSourceKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The local library, and the rule that makes the whole model work.
 *
 * THE PROPERTY UNDER TEST IS STRUCTURAL, not behavioural: nothing in this file
 * constructs a transport, a client or a connection, and the repository cannot
 * acquire one. Every operation below therefore works with nothing paired, which
 * is what "the library belongs to the user, not to a device" has to mean in
 * practice. Creating a profile used to erase flash and assign itself to the
 * adapter's working set in one step.
 */
class KbmLibraryRepositoryTest {

    /** The whole store, with no Android and no disk. */
    private class MemoryStore(
        private var document: KbmProfileLibrary = KbmProfileLibrary.EMPTY,
    ) : KbmProfileLibraryStore {
        var writes = 0
            private set

        override fun load(): KbmProfileLibrary = document

        override fun save(library: KbmProfileLibrary) {
            document = library
            writes++
        }
    }

    private fun key(usage: Int) = KbmSource(KbmSourceKind.Key, usage)

    private fun override(usage: Int, destination: KbmDestination) =
        KbmBinding(key(usage), destination, custom = true)

    // ------------------------------------------------------------ creating

    @Test fun `create stores a profile with a fresh identity`() {
        val repository = KbmLibraryRepository(MemoryStore())
        val a = repository.create(KbmProfile.Keyboard, "Halo")
        val b = repository.create(KbmProfile.Keyboard, "Halo")
        assertNotEquals("identity must not be derived from the name", a.id, b.id)
        assertEquals(2, repository.value.profiles.size)
    }

    @Test fun `create works with nothing connected and writes no adapter`() {
        // The structural claim, restated as a test: the repository has no
        // transport to write to, so this passing IS the guarantee.
        val store = MemoryStore()
        val repository = KbmLibraryRepository(store)
        repository.create(KbmProfile.Keyboard, "Halo")
        assertEquals(1, store.writes)
        assertEquals(1, repository.value.profiles.size)
    }

    @Test fun `the library is not capped at the adapter's six positions`() {
        // Six is what the ADAPTER holds resident. Conflating that with the user's
        // collection is the defect this replaces.
        val repository = KbmLibraryRepository(MemoryStore())
        repeat(25) { repository.create(KbmProfile.Keyboard, "Profile $it") }
        assertEquals(25, repository.value.profiles.size)
    }

    @Test fun `create computes the firmware fingerprint`() {
        val repository = KbmLibraryRepository(MemoryStore())
        val overrides = listOf(override(0x04, KbmDestination.Zr))
        val created = repository.create(KbmProfile.Keyboard, "Halo", overrides)
        assertEquals(
            KbmFingerprint.compute(
                KbmProfile.Keyboard, KbmFingerprint.canonical(overrides), KbmMouseConfig(),
            ),
            created.fingerprint,
        )
    }

    @Test fun `create canonicalises what it is given`() {
        // Two profiles built in a different order are the same profile, and must
        // fingerprint the same or neither can recognise its resident copy.
        val repository = KbmLibraryRepository(MemoryStore())
        val a = repository.create(
            KbmProfile.Keyboard, "A",
            listOf(override(0x05, KbmDestination.A), override(0x04, KbmDestination.B)),
        )
        val b = repository.create(
            KbmProfile.Keyboard, "B",
            listOf(override(0x04, KbmDestination.B), override(0x05, KbmDestination.A)),
        )
        assertEquals(a.fingerprint, b.fingerprint)
    }

    // -------------------------------------------------------------- saving

    @Test fun `save persists locally and changes the fingerprint`() {
        val repository = KbmLibraryRepository(MemoryStore())
        val created = repository.create(KbmProfile.Keyboard, "Halo")
        val saved = repository.save(
            created.id, "Halo", listOf(override(0x04, KbmDestination.Zr)), KbmMouseConfig(),
        )
        assertEquals(created.id, saved.id)
        assertNotEquals(created.fingerprint, saved.fingerprint)
        assertEquals(1, repository.value.profiles.size)
    }

    @Test fun `save keeps the profile in its own layout`() {
        // The layout is part of a profile's identity and is never re-derived from
        // whatever the screen happens to be showing.
        val repository = KbmLibraryRepository(MemoryStore())
        val created = repository.create(KbmProfile.KeyboardMouse, "Halo")
        val saved = repository.save(created.id, "Halo", emptyList(), KbmMouseConfig())
        assertEquals(KbmProfile.KeyboardMouse, saved.layout)
    }

    // ---------------------------------------------------------- duplicating

    @Test fun `duplicate copies content under a new identity`() {
        val repository = KbmLibraryRepository(MemoryStore())
        val source = repository.create(
            KbmProfile.Keyboard, "Halo", listOf(override(0x04, KbmDestination.Zr)),
        )
        val copy = repository.duplicate(source.id, "Halo copy")!!
        assertNotEquals(source.id, copy.id)
        assertEquals(source.fingerprint, copy.fingerprint)
        // Editing the copy must not be able to reach the original.
        repository.save(copy.id, "Halo copy", emptyList(), KbmMouseConfig())
        assertEquals(source.fingerprint, repository.value.find(source.id)!!.fingerprint)
    }

    @Test fun `duplicating a missing profile is a no-op`() {
        val repository = KbmLibraryRepository(MemoryStore())
        assertNull(repository.duplicate("nope", "x"))
        assertTrue(repository.value.profiles.isEmpty())
    }

    // ------------------------------------------------------------ renaming

    @Test fun `rename leaves identity and content alone`() {
        // A rename changes no behaviour, so a resident copy that agreed before
        // must still agree afterwards.
        val repository = KbmLibraryRepository(MemoryStore())
        val created = repository.create(
            KbmProfile.Keyboard, "Halo", listOf(override(0x04, KbmDestination.Zr)),
        )
        val renamed = repository.rename(created.id, "Zelda")!!
        assertEquals(created.id, renamed.id)
        assertEquals(created.fingerprint, renamed.fingerprint)
        assertEquals(created.bindings, renamed.bindings)
    }

    @Test fun `renaming a missing profile is a no-op`() {
        val repository = KbmLibraryRepository(MemoryStore())
        assertNull(repository.rename("nope", "x"))
    }

    // ------------------------------------------------------------ deleting

    @Test fun `delete removes only from the library`() {
        val repository = KbmLibraryRepository(MemoryStore())
        val created = repository.create(KbmProfile.Keyboard, "Halo")
        assertTrue(repository.delete(created.id))
        assertTrue(repository.value.profiles.isEmpty())
        // There is nothing here that could have removed an adapter's copy: the
        // repository owns no session. The bank projection test covers the
        // user-visible half of this.
        assertFalse(repository.delete(created.id))
    }

    // ------------------------------------------------------------ importing

    @Test fun `import deduplicates by content`() {
        // The cross-platform bridge. Local ids are NOT shared between companions,
        // so a resident profile is matched by what it contains; without this,
        // every reconnect would add another copy.
        val repository = KbmLibraryRepository(MemoryStore())
        val overrides = listOf(override(0x04, KbmDestination.Zr))
        val first = repository.import(KbmProfile.Keyboard, "Halo", overrides, KbmMouseConfig())
        val second = repository.import(KbmProfile.Keyboard, "Halo", overrides, KbmMouseConfig())
        assertEquals(first.id, second.id)
        assertEquals(1, repository.value.profiles.size)
    }

    @Test fun `import matches on content even under a different name`() {
        val repository = KbmLibraryRepository(MemoryStore())
        val overrides = listOf(override(0x04, KbmDestination.Zr))
        val first = repository.import(KbmProfile.Keyboard, "Halo", overrides, KbmMouseConfig())
        val second = repository.import(
            KbmProfile.Keyboard, "Renamed on the other device", overrides, KbmMouseConfig(),
        )
        assertEquals(first.id, second.id)
    }

    @Test fun `import scopes deduplication to the layout`() {
        // The same overrides mean different things in the two banks, so these are
        // genuinely two profiles.
        val repository = KbmLibraryRepository(MemoryStore())
        val overrides = listOf(override(0x04, KbmDestination.Zr))
        val kb = repository.import(KbmProfile.Keyboard, "Halo", overrides, KbmMouseConfig())
        val kbm = repository.import(KbmProfile.KeyboardMouse, "Halo", overrides, KbmMouseConfig())
        assertNotEquals(kb.id, kbm.id)
        assertEquals(2, repository.value.profiles.size)
    }

    @Test fun `import avoids a name collision when the content differs`() {
        val repository = KbmLibraryRepository(MemoryStore())
        repository.create(KbmProfile.Keyboard, "Halo")
        val imported = repository.import(
            KbmProfile.Keyboard, "Halo", listOf(override(0x04, KbmDestination.Zr)),
            KbmMouseConfig(),
        )
        assertNotEquals("Halo", imported.name)
        assertEquals(2, repository.value.profiles.size)
    }

    // --------------------------------------------------------- persistence

    @Test fun `a library survives a restart`() {
        val store = MemoryStore()
        val first = KbmLibraryRepository(store)
        repeat(25) { first.create(KbmProfile.Keyboard, "Profile $it") }
        first.create(KbmProfile.KeyboardMouse, "Combined")

        // A second repository over the same store is what a process restart looks
        // like from here.
        val second = KbmLibraryRepository(store)
        assertEquals(26, second.value.profiles.size)
        assertEquals(25, second.value.forLayout(KbmProfile.Keyboard).size)
    }

    @Test fun `every mutation is persisted immediately`() {
        // No explicit "save the library" step exists, so a crash after an edit
        // must not lose it.
        val store = MemoryStore()
        val repository = KbmLibraryRepository(store)
        val created = repository.create(KbmProfile.Keyboard, "Halo")
        repository.rename(created.id, "Zelda")
        repository.save(created.id, "Zelda", emptyList(), KbmMouseConfig())
        repository.delete(created.id)
        assertEquals(4, store.writes)
    }

    // ------------------------------------------------------------- offline

    @Test fun `a whole editing session needs no adapter`() {
        // Create, edit, save, duplicate, rename and delete, in order, with no
        // connection anywhere in the test.
        val repository = KbmLibraryRepository(MemoryStore())
        val layout = KbmProfile.Keyboard
        val first = KbmDefaults.forLayout(layout).bindings.first()

        val created = repository.create(layout, "Halo")
        val edited = repository.save(
            created.id, "Halo",
            listOf(KbmBinding(first.source, KbmDestination.Zr, custom = true)),
            KbmMouseConfig(),
        )
        assertNotEquals(created.fingerprint, edited.fingerprint)

        val copy = repository.duplicate(edited.id, "Halo copy")!!
        assertEquals(edited.fingerprint, copy.fingerprint)

        repository.rename(copy.id, "Zelda")
        assertEquals("Zelda", repository.value.find(copy.id)!!.name)

        repository.delete(edited.id)
        assertEquals(listOf("Zelda"), repository.value.profiles.map { it.name })
    }
}
