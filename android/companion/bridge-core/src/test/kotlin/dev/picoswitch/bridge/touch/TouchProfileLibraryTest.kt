package dev.picoswitch.bridge.touch

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

private const val T0 = 1_700_000_000_000L

private fun applied(edit: TouchProfileEdit): TouchProfileEdit.Applied {
    assertTrue("expected an applied edit, got $edit", edit is TouchProfileEdit.Applied)
    return edit as TouchProfileEdit.Applied
}

private fun rejected(edit: TouchProfileEdit): String {
    assertTrue("expected a rejected edit, got $edit", edit is TouchProfileEdit.Rejected)
    return (edit as TouchProfileEdit.Rejected).reason
}

/** A small, valid customization: one control nudged and enlarged. */
private fun sampleOverride(
    personality: TouchProfileId = TouchProfileId.GameCube,
    scale: Float = 1.2f,
): TouchLayoutOverride {
    val profile = TouchProfileCatalog.require(personality)
    val target = profile.defaultTemplate.controls.first { it.output == TouchOutputControl.Dpad }
    return TouchLayoutEditor.scale(
        profile,
        TouchLayoutEditor.empty(profile),
        target.id,
        scale,
        editGroup = false,
    )
}

class TouchProfileLibraryTest {

    @Test fun `the factory profile exists without storage and is never a stored profile`() {
        TouchProfileId.entries.forEach { personality ->
            val library = TouchProfileLibrary.empty(personality)
            assertEquals(1, library.profiles.size)
            assertTrue(library.profiles.single().isFactory)
            assertTrue(library.userProfiles.isEmpty())
            assertTrue(library.selected.isFactory)
            assertTrue(library.selected.isPristine)
            // The runtime composes the shipped template, not a stored document.
            assertNull(library.activeOverride)
            // ... and the encoded document stores no copy of it that could rot.
            // The selection may name it; the profile list may never contain it.
            assertTrue(
                TouchProfileLibraryJsonCodec.encode(library).contains("\"profiles\":[]"),
            )
        }
    }

    @Test fun `saving onto the factory profile creates a profile instead of overwriting it`() {
        val library = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val edit = applied(
            TouchProfileLibraryEditor.save(
                library,
                TouchProfileLibrary.FACTORY_PROFILE_ID,
                sampleOverride(),
                T0,
                newProfileName = "Smash",
            ),
        )
        assertEquals(1, edit.library.userProfiles.size)
        assertEquals("Smash", edit.library.selected.name)
        assertNotEquals(TouchProfileLibrary.FACTORY_PROFILE_ID, edit.library.selectedProfileId)
        // The protected entry is untouched and still reachable.
        assertTrue(edit.library.factoryProfile.isPristine)
        assertEquals(
            TouchProfileLibrary.FACTORY_PROFILE_NAME,
            edit.library.factoryProfile.name,
        )
    }

    @Test fun `the factory profile cannot be renamed or deleted`() {
        val library = TouchProfileLibrary.empty(TouchProfileId.Pro2)
        rejected(TouchProfileLibraryEditor.rename(library, TouchProfileLibrary.FACTORY_PROFILE_ID, "Mine"))
        rejected(TouchProfileLibraryEditor.delete(library, TouchProfileLibrary.FACTORY_PROFILE_ID))
        // Resetting it is a no-op rather than an error: it is already the default.
        val reset = applied(
            TouchProfileLibraryEditor.resetToDefault(
                library, TouchProfileLibrary.FACTORY_PROFILE_ID, T0,
            ),
        )
        assertEquals(library, reset.library)
    }

    @Test fun `duplicating the factory profile starts a user profile from the shipped layout`() {
        val library = TouchProfileLibrary.empty(TouchProfileId.JoyConLeft)
        val edit = applied(
            TouchProfileLibraryEditor.duplicate(
                library, TouchProfileLibrary.FACTORY_PROFILE_ID, "FPS", T0,
            ),
        )
        val copy = edit.library.selected
        assertFalse(copy.isFactory)
        assertEquals("FPS", copy.name)
        assertTrue(copy.isPristine)
        assertEquals(TouchProfileId.JoyConLeft, copy.personality)
    }

    @Test fun `deleting the active profile falls back to the one that always exists`() {
        var library = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val first = applied(TouchProfileLibraryEditor.create(library, "A", T0))
        library = first.library
        val second = applied(TouchProfileLibraryEditor.create(library, "B", T0 + 1))
        library = second.library
        assertEquals(second.profileId, library.selectedProfileId)

        library = applied(TouchProfileLibraryEditor.delete(library, second.profileId)).library
        assertEquals(TouchProfileLibrary.FACTORY_PROFILE_ID, library.selectedProfileId)
        // Deleting a profile that is NOT active leaves the selection alone.
        library = applied(TouchProfileLibraryEditor.select(library, first.profileId)).library
        val third = applied(TouchProfileLibraryEditor.create(library, "C", T0 + 2))
        library = applied(TouchProfileLibraryEditor.select(third.library, first.profileId)).library
        library = applied(TouchProfileLibraryEditor.delete(library, third.profileId)).library
        assertEquals(first.profileId, library.selectedProfileId)
    }

    @Test fun `names are sanitized, bounded, unique, and never collide with the default`() {
        var library = TouchProfileLibrary.empty(TouchProfileId.Pro2)
        library = applied(TouchProfileLibraryEditor.create(library, "  Race\tMode  ", T0)).library
        assertEquals("Race Mode", library.selected.name)

        library = applied(TouchProfileLibraryEditor.create(library, "Race Mode", T0 + 1)).library
        assertEquals("Race Mode 2", library.selected.name)

        // The reserved factory name cannot be taken by a user profile, or the
        // protected entry becomes unidentifiable in the picker.
        library = applied(
            TouchProfileLibraryEditor.create(library, TouchProfileLibrary.FACTORY_PROFILE_NAME, T0 + 2),
        ).library
        assertNotEquals(TouchProfileLibrary.FACTORY_PROFILE_NAME, library.selected.name)

        library = applied(TouchProfileLibraryEditor.create(library, "x".repeat(200), T0 + 3)).library
        assertTrue(library.selected.name.length <= TouchProfileLibrary.MAX_NAME_LENGTH)

        library = applied(TouchProfileLibraryEditor.create(library, "   ", T0 + 4)).library
        assertTrue(library.selected.name.isNotBlank())
    }

    @Test fun `the profile count is bounded and the refusal explains itself`() {
        var library = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        repeat(TouchProfileLibrary.MAX_USER_PROFILES) { index ->
            library = applied(TouchProfileLibraryEditor.create(library, "P$index", T0 + index)).library
        }
        assertEquals(TouchProfileLibrary.MAX_USER_PROFILES, library.userProfiles.size)
        val reason = rejected(TouchProfileLibraryEditor.create(library, "One more", T0 + 99))
        assertTrue(reason.contains(TouchProfileLibrary.MAX_USER_PROFILES.toString()))
    }

    @Test fun `ids stay distinct even when the clock does not move`() {
        var library = TouchProfileLibrary.empty(TouchProfileId.Pro2)
        val ids = mutableSetOf<String>()
        repeat(5) {
            val edit = applied(TouchProfileLibraryEditor.create(library, "P$it", T0))
            library = edit.library
            assertTrue("duplicate id ${edit.profileId}", ids.add(edit.profileId))
        }
        assertFalse(TouchProfileLibrary.FACTORY_PROFILE_ID in ids)
    }

    @Test fun `a layout from another controller is refused rather than adopted`() {
        val library = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val created = applied(TouchProfileLibraryEditor.create(library, "Mine", T0))
        rejected(
            TouchProfileLibraryEditor.save(
                created.library,
                created.profileId,
                sampleOverride(TouchProfileId.Pro2),
                T0,
            ),
        )
    }

    @Test fun `personalities keep independent libraries`() {
        val gc = applied(
            TouchProfileLibraryEditor.save(
                TouchProfileLibrary.empty(TouchProfileId.GameCube),
                TouchProfileLibrary.FACTORY_PROFILE_ID,
                sampleOverride(TouchProfileId.GameCube),
                T0,
                newProfileName = "Smash",
            ),
        ).library
        val pro = TouchProfileLibrary.empty(TouchProfileId.Pro2)
        assertTrue(pro.userProfiles.isEmpty())
        assertTrue(pro.selected.isFactory)
        // Encoding one and decoding it as the other must not be accepted.
        val decoded = TouchProfileLibraryJsonCodec.decode(
            TouchProfileLibraryJsonCodec.encode(gc),
            TouchProfileId.Pro2,
        )
        assertTrue(decoded is TouchProfileLibraryDecodeResult.Invalid)
    }

    @Test fun `reset clears a profile's customizations without deleting the profile`() {
        var library = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val created = applied(TouchProfileLibraryEditor.create(library, "Mine", T0))
        library = applied(
            TouchProfileLibraryEditor.save(
                created.library, created.profileId, sampleOverride(), T0 + 1,
            ),
        ).library
        assertFalse(library.selected.isPristine)

        library = applied(
            TouchProfileLibraryEditor.resetToDefault(library, created.profileId, T0 + 2),
        ).library
        assertTrue(library.selected.isPristine)
        assertEquals("Mine", library.selected.name)
        assertEquals(1, library.userProfiles.size)
    }

    @Test fun `the pre-profile override document survives the upgrade as a named profile`() {
        val override = sampleOverride(TouchProfileId.GameCube)
        val library = TouchProfileLibraryEditor.adoptLegacyOverride(
            TouchProfileId.GameCube, override, T0,
        )
        assertEquals(1, library.userProfiles.size)
        assertEquals(override, library.selected.override)
        assertFalse(library.selected.isFactory)

        // Nothing to adopt: an absent or empty legacy document leaves the shipped
        // controller in place rather than creating an empty profile.
        val empty = TouchProfileLibraryEditor.adoptLegacyOverride(
            TouchProfileId.GameCube,
            TouchLayoutEditor.empty(TouchProfileCatalog.require(TouchProfileId.GameCube)),
            T0,
        )
        assertTrue(empty.userProfiles.isEmpty())
    }
}

class TouchProfileLibraryCodecTest {

    private fun library(): TouchProfileLibrary {
        var value = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val a = applied(TouchProfileLibraryEditor.create(value, "Smash", T0))
        value = applied(
            TouchProfileLibraryEditor.save(a.library, a.profileId, sampleOverride(), T0 + 1),
        ).library
        value = applied(TouchProfileLibraryEditor.create(value, "Kart", T0 + 2)).library
        return value
    }

    @Test fun `a library round trips deterministically`() {
        val original = library()
        val encoded = TouchProfileLibraryJsonCodec.encode(original)
        assertEquals(encoded, TouchProfileLibraryJsonCodec.encode(original))
        val decoded = TouchProfileLibraryJsonCodec.decode(encoded, TouchProfileId.GameCube)
        assertTrue(decoded is TouchProfileLibraryDecodeResult.Valid)
        val value = (decoded as TouchProfileLibraryDecodeResult.Valid).value
        assertEquals(original, value)
        assertEquals(encoded, TouchProfileLibraryJsonCodec.encode(value))
    }

    @Test fun `unusable documents are reported and never invent a layout`() {
        val bad = listOf(
            "" to "not JSON",
            "[]" to "not an object",
            """{"personality":"gc","profiles":[]}""" to "no schema version",
            """{"schemaVersion":2,"personality":"gc","profiles":[]}""" to "future schema",
            """{"schemaVersion":1,"personality":"nope","profiles":[]}""" to "unknown controller",
            """{"schemaVersion":1,"personality":"gc"}""" to "no profile list",
            """{"schemaVersion":1,"personality":"gc","profiles":[{}]}""" to "nameless profile",
        )
        bad.forEach { (raw, why) ->
            val decoded = TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
            assertTrue(why, decoded is TouchProfileLibraryDecodeResult.Invalid)
        }
    }

    @Test fun `a stored profile may not claim the reserved default identity`() {
        val raw = """
            {"schemaVersion":1,"personality":"gc","selectedProfileId":"x","profiles":[
              {"id":"${TouchProfileLibrary.FACTORY_PROFILE_ID}","name":"Fake",
               "templateId":"picoswitch.touch.gc.v1","templateRevision":2,"controls":{}}]}
        """.trimIndent()
        val decoded = TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
        assertTrue(decoded is TouchProfileLibraryDecodeResult.Invalid)
    }

    @Test fun `duplicate stored ids are refused because selection would be ambiguous`() {
        val entry = """{"id":"p1","name":"A","templateId":"picoswitch.touch.gc.v1",
            "templateRevision":2,"controls":{}}""".trimIndent().replace("\n", "")
        val raw = """{"schemaVersion":1,"personality":"gc","profiles":[$entry,$entry]}"""
        assertTrue(
            TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
                is TouchProfileLibraryDecodeResult.Invalid,
        )
    }

    @Test fun `a stored control override still has to pass every range check`() {
        val raw = """
            {"schemaVersion":1,"personality":"gc","profiles":[
              {"id":"p1","name":"A","templateId":"picoswitch.touch.gc.v1","templateRevision":2,
               "controls":{"dpad":{"anchorX":4.0}}}]}
        """.trimIndent()
        assertTrue(
            TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
                is TouchProfileLibraryDecodeResult.Invalid,
        )
    }

    @Test fun `a selection naming a missing profile resolves to the default, not an error`() {
        val raw = """
            {"schemaVersion":1,"personality":"gc","selectedProfileId":"gone","profiles":[
              {"id":"p1","name":"A","templateId":"picoswitch.touch.gc.v1","templateRevision":2,
               "controls":{}}]}
        """.trimIndent()
        val decoded = TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
        assertTrue(decoded is TouchProfileLibraryDecodeResult.Valid)
        val value = (decoded as TouchProfileLibraryDecodeResult.Valid).value
        assertEquals(TouchProfileLibrary.FACTORY_PROFILE_ID, value.selectedProfileId)
        assertEquals(1, value.userProfiles.size)
    }

    @Test fun `an exported profile round trips and imports as a new profile`() {
        val source = library()
        val exported = TouchProfileLibraryJsonCodec.encodeExport(source.userProfiles.first())
        val decoded = TouchProfileLibraryJsonCodec.decodeExport(exported)
        assertTrue(decoded is TouchProfileDecodeResult.Valid)
        val profile = (decoded as TouchProfileDecodeResult.Valid).value
        assertEquals(source.userProfiles.first().override, profile.override)

        val target = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val edit = applied(TouchProfileLibraryEditor.import(target, profile, T0 + 10))
        assertEquals(1, edit.library.userProfiles.size)
        // The imported document's own id is never reused; the receiving library
        // allocates one, so an import can never collide with what is already there.
        assertNotEquals("imported", edit.library.selectedProfileId)
        assertEquals(profile.override, edit.library.selected.override)
    }

    @Test fun `an export for another controller is refused on import`() {
        val other = applied(
            TouchProfileLibraryEditor.create(TouchProfileLibrary.empty(TouchProfileId.Pro2), "P", T0),
        ).library.userProfiles.first()
        rejected(
            TouchProfileLibraryEditor.import(
                TouchProfileLibrary.empty(TouchProfileId.GameCube), other, T0,
            ),
        )
    }

    @Test fun `an unrelated JSON file is not mistaken for a layout export`() {
        assertTrue(
            TouchProfileLibraryJsonCodec.decodeExport("""{"schemaVersion":1,"hello":"world"}""")
                is TouchProfileDecodeResult.Invalid,
        )
    }

    @Test fun `every stored profile composes and resolves into a usable controller`() {
        val library = library()
        val profile = TouchProfileCatalog.require(library.personality)
        library.profiles.forEach { entry ->
            val composition = TouchLayoutComposer.compose(profile, entry.override)
            assertNull(entry.name, composition.warning)
            val resolved = TouchLayoutResolver.resolve(
                composition.layout,
                TouchLayoutRegion(0f, 0f, 915f, 412f, 1f),
                TouchLayoutAuditMode.Runtime,
            )
            assertTrue("${entry.name}: ${resolved.problem}", resolved.fits)
        }
    }
}
