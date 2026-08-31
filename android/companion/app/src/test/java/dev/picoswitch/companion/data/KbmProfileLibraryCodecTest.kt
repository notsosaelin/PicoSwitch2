package dev.picoswitch.companion.data

import dev.picoswitch.management.KbmBinding
import dev.picoswitch.management.KbmDestination
import dev.picoswitch.management.KbmMouseConfig
import dev.picoswitch.management.KbmProfile
import dev.picoswitch.management.KbmSource
import dev.picoswitch.management.KbmSourceKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The library's on-disk form.
 *
 * The rules worth encoding are about what happens to a document this build
 * cannot fully read. A profile is the user's own work and is offered back to
 * them as theirs, so a row that cannot be read exactly is DROPPED rather than
 * repaired into something plausible: a mapping missing one key is not the
 * mapping they saved, and it would then behave in a way they never configured.
 */
class KbmProfileLibraryCodecTest {

    private fun key(usage: Int) = KbmSource(KbmSourceKind.Key, usage)

    private fun profile(
        id: String = "a1",
        layout: KbmProfile = KbmProfile.Keyboard,
        name: String = "Halo",
    ) = KbmLocalProfile(
        id = id,
        layout = layout,
        name = name,
        bindings = listOf(KbmBinding(key(0x04), KbmDestination.Zr, custom = true)),
        mouse = KbmMouseConfig(sensitivityX = 640, invertY = true),
        fingerprint = 12345L,
        modifiedMillis = 1_700_000_000_000L,
    )

    @Test fun `a library round-trips exactly`() {
        val library = KbmProfileLibrary(listOf(profile(), profile(id = "b2", name = "Zelda")))
        assertEquals(library, KbmProfileLibraryCodec.decode(KbmProfileLibraryCodec.encode(library)))
    }

    @Test fun `an empty document decodes to an empty library`() {
        assertEquals(KbmProfileLibrary.EMPTY, KbmProfileLibraryCodec.decode(null))
        assertEquals(KbmProfileLibrary.EMPTY, KbmProfileLibraryCodec.decode(""))
        assertEquals(KbmProfileLibrary.EMPTY, KbmProfileLibraryCodec.decode("   "))
    }

    @Test fun `garbage never throws`() {
        // A damaged preferences file must not be able to stop the app starting.
        listOf("{", "[]", "not json at all", "{\"profiles\":7}").forEach {
            assertEquals(KbmProfileLibrary.EMPTY, KbmProfileLibraryCodec.decode(it))
        }
    }

    @Test fun `a future schema is refused rather than misread`() {
        // Reading it with this build's rules would silently rewrite it on the
        // next save, losing whatever the newer build had stored.
        val text = KbmProfileLibraryCodec.encode(KbmProfileLibrary(listOf(profile())))
            .replace("\"schema\":1", "\"schema\":99")
        assertEquals(KbmProfileLibrary.EMPTY, KbmProfileLibraryCodec.decode(text))
    }

    @Test fun `a row with an unreadable binding is dropped, not repaired`() {
        val text = KbmProfileLibraryCodec.encode(KbmProfileLibrary(listOf(profile())))
            .replace("\"dst\":\"zr\"", "\"dst\":\"teleport\"")
        assertTrue(KbmProfileLibraryCodec.decode(text).profiles.isEmpty())
    }

    @Test fun `a row with an unreadable layout is dropped`() {
        val text = KbmProfileLibraryCodec.encode(KbmProfileLibrary(listOf(profile())))
            .replace("\"layout\":\"kb\"", "\"layout\":\"gamepad\"")
        assertTrue(KbmProfileLibraryCodec.decode(text).profiles.isEmpty())
    }

    @Test fun `one bad row does not lose the good ones`() {
        val good = profile(id = "good", name = "Zelda")
        val bad = profile(id = "bad", name = "Halo")
        val text = KbmProfileLibraryCodec.encode(KbmProfileLibrary(listOf(bad, good)))
            // Only the first row's destination is corrupted.
            .replaceFirst("\"dst\":\"zr\"", "\"dst\":\"teleport\"")
        val decoded = KbmProfileLibraryCodec.decode(text)
        assertEquals(listOf("good"), decoded.profiles.map { it.id })
    }

    @Test fun `duplicate ids collapse to the first`() {
        // find() and without() are id-addressed; two rows sharing an id would
        // make both ambiguous.
        val text = KbmProfileLibraryCodec.encode(
            KbmProfileLibrary(listOf(profile(name = "First"), profile(name = "Second"))),
        )
        val decoded = KbmProfileLibraryCodec.decode(text)
        assertEquals(1, decoded.profiles.size)
        assertEquals("First", decoded.profiles.single().name)
    }

    @Test fun `a large library survives the round trip`() {
        // The library is deliberately unbounded: six is what the ADAPTER holds
        // resident, and the two must never be conflated again.
        val many = (1..40).map { profile(id = "id$it", name = "Profile $it") }
        val decoded = KbmProfileLibraryCodec.decode(
            KbmProfileLibraryCodec.encode(KbmProfileLibrary(many)),
        )
        assertEquals(40, decoded.profiles.size)
    }

    @Test fun `a profile with no overrides round-trips`() {
        // An untouched copy of Default is a legitimate library profile.
        val bare = KbmLocalProfile(
            id = "bare", layout = KbmProfile.KeyboardMouse, name = "Plain",
        )
        val decoded = KbmProfileLibraryCodec.decode(
            KbmProfileLibraryCodec.encode(KbmProfileLibrary(listOf(bare))),
        )
        assertEquals(bare, decoded.profiles.single())
    }

    @Test fun `overrides are stored as user overrides on the way back in`() {
        // Everything a profile stores is by definition an override; a row that
        // came back marked canonical would be excluded from the fingerprint.
        val decoded = KbmProfileLibraryCodec.decode(
            KbmProfileLibraryCodec.encode(KbmProfileLibrary(listOf(profile()))),
        )
        assertTrue(decoded.profiles.single().bindings.all { it.custom })
    }

    @Test fun `layout scoping and lookup`() {
        val library = KbmProfileLibrary(
            listOf(
                profile(id = "kb1", layout = KbmProfile.Keyboard, name = "Halo"),
                profile(id = "kbm1", layout = KbmProfile.KeyboardMouse, name = "Halo"),
            ),
        )
        assertEquals(listOf("kb1"), library.forLayout(KbmProfile.Keyboard).map { it.id })
        assertEquals(listOf("kbm1"), library.forLayout(KbmProfile.KeyboardMouse).map { it.id })
        assertNotNull(library.find("kb1"))
        assertNull(library.find("nope"))
    }

    @Test fun `suggestName avoids collisions within a layout only`() {
        val library = KbmProfileLibrary(
            listOf(
                profile(id = "kb1", layout = KbmProfile.Keyboard, name = "Halo"),
                profile(id = "kb2", layout = KbmProfile.Keyboard, name = "Halo 2"),
            ),
        )
        assertEquals("Halo 3", library.suggestName(KbmProfile.Keyboard, "Halo"))
        // The other bank is a separate namespace; the same name there is fine.
        assertEquals("Halo", library.suggestName(KbmProfile.KeyboardMouse, "Halo"))
    }
}
