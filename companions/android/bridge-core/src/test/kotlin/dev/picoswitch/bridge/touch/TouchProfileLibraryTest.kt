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

/** A small, valid customization: one control enlarged. */
private fun sampleDocument(
    personality: TouchProfileId = TouchProfileId.GameCube,
    scale: Float = 1.2f,
): TouchLayoutDocument {
    val profile = TouchProfileCatalog.require(personality)
    val target = profile.catalog.first { it.output == TouchOutputControl.Dpad }
    return TouchLayoutEditor.setScale(
        authored(profile),
        setOf(target.id),
        scale,
        editGroup = false,
    )
}

/** The same customization expressed in the retired schema-1 override model. */
private fun sampleOverride(
    personality: TouchProfileId = TouchProfileId.GameCube,
    scale: Float = 1.2f,
): TouchLayoutOverride {
    val profile = TouchProfileCatalog.require(personality)
    val target = profile.catalog.first { it.output == TouchOutputControl.Dpad }
    return TouchLayoutOverride(
        profileId = personality,
        templateId = profile.defaultTemplate.id,
        basedOnRevision = profile.defaultTemplate.templateRevision,
        controls = mapOf(target.id to TouchControlOverride(scale = scale)),
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
            // The runtime composes the SHIPPED arrangement, synthesized from the
            // template rather than read back from anything stored.
            assertEquals(
                TouchLayoutDocument.authoredDefault(TouchProfileCatalog.require(personality)),
                library.activeDocument,
            )
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
                sampleDocument(),
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
                sampleDocument(TouchProfileId.Pro2),
                T0,
            ),
        )
    }

    @Test fun `personalities keep independent libraries`() {
        val gc = applied(
            TouchProfileLibraryEditor.save(
                TouchProfileLibrary.empty(TouchProfileId.GameCube),
                TouchProfileLibrary.FACTORY_PROFILE_ID,
                sampleDocument(TouchProfileId.GameCube),
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
                created.library, created.profileId, sampleDocument(), T0 + 1,
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
        val library = TouchProfileLibraryEditor.adoptLegacyOverride(
            TouchProfileId.GameCube, sampleOverride(TouchProfileId.GameCube), T0,
        )
        assertEquals(1, library.userProfiles.size)
        // Migrated, not stored as it was: the customization is now an instance
        // transform, and the rest of the shipped layout came with it.
        assertEquals(sampleDocument(TouchProfileId.GameCube), library.selected.document)
        assertFalse(library.selected.isFactory)

        // Nothing to adopt: an absent or empty legacy document leaves the shipped
        // controller in place rather than creating an empty profile.
        val empty = TouchProfileLibraryEditor.adoptLegacyOverride(
            TouchProfileId.GameCube,
            TouchLayoutOverride(
                profileId = TouchProfileId.GameCube,
                templateId = TouchPersonalityTemplates.gameCube.id,
                basedOnRevision = TouchPersonalityTemplates.gameCube.templateRevision,
            ),
            T0,
        )
        assertTrue(empty.userProfiles.isEmpty())
    }
}

/**
 * Golden migration coverage: a schema-1 layout must become the instance layout a
 * user would recognize, for every shipped personality.
 *
 * The property that matters is not "it decoded" but "it decoded into the same
 * arrangement". A migration that silently dropped a moved control, or that
 * resurrected one the user had hidden, would pass a decode-only test.
 */
class TouchLayoutMigrationTest {

    private fun legacyLibraryJson(
        personality: TouchProfileId,
        controls: String,
    ): String {
        val template = TouchProfileCatalog.require(personality).defaultTemplate
        return """
            {"schemaVersion":1,"personality":"${personality.key}","selectedProfileId":"p1",
             "profiles":[{"id":"p1","name":"Mine","templateId":"${template.id}",
              "templateRevision":${template.templateRevision},"controls":$controls}]}
        """.trimIndent().replace("\n", "")
    }

    @Test fun `an unmodified legacy layout migrates to exactly the shipped arrangement`() {
        TouchProfileId.entries.forEach { personality ->
            val profile = TouchProfileCatalog.require(personality)
            val decoded = TouchProfileLibraryJsonCodec.decode(
                legacyLibraryJson(personality, "{}"),
                personality,
            )
            val value = (decoded as TouchProfileLibraryDecodeResult.Valid)
            assertTrue("$personality was not reported as migrated", value.migrated)
            assertEquals(
                "$personality",
                TouchLayoutDocument.authoredDefault(profile).controls,
                value.value.activeDocument.controls,
            )
        }
    }

    @Test fun `a moved, scaled and latched control keeps all three through migration`() {
        val personality = TouchProfileId.Pro2
        val decoded = TouchProfileLibraryJsonCodec.decode(
            legacyLibraryJson(
                personality,
                """{"${TouchLayoutV1.DPAD}":{"anchorX":0.4,"anchorY":0.6,"scale":1.3,"latch":false}}""",
            ),
            personality,
        )
        val document = (decoded as TouchProfileLibraryDecodeResult.Valid).value.activeDocument
        val dpad = requireNotNull(document.instance(TouchLayoutV1.DPAD))
        assertEquals(0.4f, dpad.anchorX, 1e-6f)
        assertEquals(0.6f, dpad.anchorY, 1e-6f)
        assertEquals(1.3f, dpad.scale, 1e-6f)
        assertEquals(false, dpad.latch)
        assertEquals(TouchLayoutV1.DPAD, dpad.catalogId)
    }

    @Test fun `a hidden control becomes an absent instance, not a ghost`() {
        val personality = TouchProfileId.Pro2
        val decoded = TouchProfileLibraryJsonCodec.decode(
            legacyLibraryJson(personality, """{"${TouchLayoutV1.CHAT}":{"visible":false}}"""),
            personality,
        )
        val document = (decoded as TouchProfileLibraryDecodeResult.Valid).value.activeDocument
        assertNull(document.instance(TouchLayoutV1.CHAT))
        // Absent from the layout, still present in the catalog -- which is what
        // makes Add Control able to bring it back.
        assertTrue(
            TouchProfileCatalog.require(personality).catalogEntry(TouchLayoutV1.CHAT) != null,
        )
    }

    @Test fun `a revealed optional control migrates as a real instance`() {
        val personality = TouchProfileId.Pro2
        val decoded = TouchProfileLibraryJsonCodec.decode(
            legacyLibraryJson(personality, """{"${TouchLayoutV1.GRIP_LEFT}":{"visible":true}}"""),
            personality,
        )
        val document = (decoded as TouchProfileLibraryDecodeResult.Valid).value.activeDocument
        val grip = requireNotNull(document.instance(TouchLayoutV1.GRIP_LEFT))
        assertEquals(TouchLayoutV1.GRIP_LEFT, grip.catalogId)
        // The other grip stayed out, because the legacy document never asked
        // for it. Optional means optional in both directions.
        assertNull(document.instance(TouchLayoutV1.GRIP_RIGHT))
    }

    @Test fun `a legacy group scale is baked into the migrated offsets`() {
        val personality = TouchProfileId.Pro2
        val faces = listOf(
            TouchLayoutV1.FACE_NORTH, TouchLayoutV1.FACE_SOUTH,
            TouchLayoutV1.FACE_EAST, TouchLayoutV1.FACE_WEST,
        )
        val overrides = faces.joinToString(",") {
            """"$it":{"scale":1.5,"groupOffsetScale":1.5}"""
        }
        val decoded = TouchProfileLibraryJsonCodec.decode(
            legacyLibraryJson(personality, "{$overrides}"),
            personality,
        )
        val document = (decoded as TouchProfileLibraryDecodeResult.Valid).value.activeDocument
        val authoredDoc = TouchLayoutDocument.authoredDefault(
            TouchProfileCatalog.require(personality),
        )
        faces.forEach { id ->
            val before = requireNotNull(authoredDoc.instance(id))
            val after = requireNotNull(document.instance(id))
            assertEquals(1.5f, after.scale, 1e-6f)
            assertEquals(before.offsetXUnits * 1.5f, after.offsetXUnits, 1e-4f)
            assertEquals(before.offsetYUnits * 1.5f, after.offsetYUnits, 1e-4f)
            // The authored cluster survives as a real group.
            assertEquals(before.groupId, after.groupId)
        }
    }

    @Test fun `migration is deterministic`() {
        val personality = TouchProfileId.GameCube
        val raw = legacyLibraryJson(personality, """{"dpad":{"anchorX":0.33}}""")
        val first = TouchProfileLibraryJsonCodec.decode(raw, personality)
        val second = TouchProfileLibraryJsonCodec.decode(raw, personality)
        assertEquals(first, second)
        assertEquals(
            TouchProfileLibraryJsonCodec.encode(
                (first as TouchProfileLibraryDecodeResult.Valid).value,
            ),
            TouchProfileLibraryJsonCodec.encode(
                (second as TouchProfileLibraryDecodeResult.Valid).value,
            ),
        )
    }

    @Test fun `a migrated library is written back in the current schema`() {
        val personality = TouchProfileId.GameCube
        val decoded = TouchProfileLibraryJsonCodec.decode(
            legacyLibraryJson(personality, """{"dpad":{"scale":1.2}}"""),
            personality,
        ) as TouchProfileLibraryDecodeResult.Valid
        val encoded = TouchProfileLibraryJsonCodec.encode(decoded.value)
        assertTrue(""""schemaVersion":2""" in encoded)
        // Round trips at the new version without migrating again.
        val again = TouchProfileLibraryJsonCodec.decode(encoded, personality)
        assertEquals(decoded.value, (again as TouchProfileLibraryDecodeResult.Valid).value)
        assertFalse(again.migrated)
    }
}

class TouchProfileLibraryCodecTest {

    private fun library(): TouchProfileLibrary {
        var value = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val a = applied(TouchProfileLibraryEditor.create(value, "Smash", T0))
        value = applied(
            TouchProfileLibraryEditor.save(a.library, a.profileId, sampleDocument(), T0 + 1),
        ).library
        value = applied(TouchProfileLibraryEditor.create(value, "Kart", T0 + 2)).library
        return value
    }

    /** A minimal, valid schema-2 profile body for the negative fixtures below. */
    private fun profileJson(controls: String, id: String = "p1") =
        """{"id":"$id","name":"A","templateId":"picoswitch.touch.gc.v1",""" +
            """"templateRevision":2,"controls":$controls}"""

    private val oneControl =
        """[{"instanceId":"dpad","catalogId":"dpad","anchorX":0.27,"anchorY":0.78,"zIndex":0}]"""

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
            """{"schemaVersion":3,"personality":"gc","profiles":[]}""" to "future schema",
            """{"schemaVersion":0,"personality":"gc","profiles":[]}""" to "pre-migration schema",
            """{"schemaVersion":2,"personality":"nope","profiles":[]}""" to "unknown controller",
            """{"schemaVersion":2,"personality":"gc"}""" to "no profile list",
            """{"schemaVersion":2,"personality":"gc","profiles":[{}]}""" to "nameless profile",
            """{"schemaVersion":2,"personality":"gc","profiles":[${profileJson("{}")}]}""" to
                "controls object where a list belongs",
        )
        bad.forEach { (raw, why) ->
            val decoded = TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
            assertTrue(why, decoded is TouchProfileLibraryDecodeResult.Invalid)
        }
    }

    @Test fun `a stored profile may not claim the reserved default identity`() {
        val raw = """{"schemaVersion":2,"personality":"gc","selectedProfileId":"x","profiles":[
            ${profileJson(oneControl, id = TouchProfileLibrary.FACTORY_PROFILE_ID)}]}"""
        val decoded = TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
        assertTrue(decoded is TouchProfileLibraryDecodeResult.Invalid)
    }

    @Test fun `duplicate stored ids are refused because selection would be ambiguous`() {
        val entry = profileJson(oneControl)
        val raw = """{"schemaVersion":2,"personality":"gc","profiles":[$entry,$entry]}"""
        assertTrue(
            TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
                is TouchProfileLibraryDecodeResult.Invalid,
        )
    }

    @Test fun `a stored control instance still has to pass every range check`() {
        val outOfRange = listOf(
            """[{"instanceId":"dpad","catalogId":"dpad","anchorX":4.0,"anchorY":0.5}]""",
            """[{"instanceId":"dpad","catalogId":"dpad","anchorX":0.5,"anchorY":0.5,"scale":9.0}]""",
            """[{"instanceId":"dpad","catalogId":"dpad","anchorX":0.5,"anchorY":0.5,
                 "offsetXUnits":1.0e9}]""".replace("\n", ""),
            """[{"catalogId":"dpad","anchorX":0.5,"anchorY":0.5}]""",
            """[{"instanceId":"dpad","anchorX":0.5,"anchorY":0.5}]""",
            """[{"instanceId":"dpad","catalogId":"dpad","anchorX":0.5,"anchorY":0.5,
                 "latch":"yes"}]""".replace("\n", ""),
        )
        outOfRange.forEach { controls ->
            val raw =
                """{"schemaVersion":2,"personality":"gc","profiles":[${profileJson(controls)}]}"""
            assertTrue(
                controls,
                TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
                    is TouchProfileLibraryDecodeResult.Invalid,
            )
        }
    }

    /**
     * Structural damage is REPAIRED at composition rather than refused at
     * decode: one bad instance must not cost the user a whole layout. The
     * distinction from the range checks above is deliberate — an unreadable
     * number means the document is corrupt, while an instance naming a control
     * this build no longer has is simply a control that is gone.
     */
    @Test fun `a dangling catalog reference costs one control, not the layout`() {
        val controls = """[
            {"instanceId":"dpad","catalogId":"dpad","anchorX":0.27,"anchorY":0.78,"zIndex":0},
            {"instanceId":"ghost","catalogId":"no-such-control","anchorX":0.5,"anchorY":0.5,
             "zIndex":1}]""".replace("\n", "")
        val raw = """{"schemaVersion":2,"personality":"gc","selectedProfileId":"p1",
            "profiles":[${profileJson(controls)}]}""".replace("\n", "")
        val decoded = TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
        val library = (decoded as TouchProfileLibraryDecodeResult.Valid).value
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val composition = TouchLayoutComposer.compose(profile, library.activeDocument)
        assertTrue(composition.degraded)
        assertTrue(composition.layout.controls.any { it.id == "dpad" })
        assertTrue(composition.layout.controls.none { it.id == "ghost" })
    }

    @Test fun `duplicate instance ids are repaired rather than fatal`() {
        val controls = """[
            {"instanceId":"dpad","catalogId":"dpad","anchorX":0.27,"anchorY":0.78,"zIndex":0},
            {"instanceId":"dpad","catalogId":"dpad","anchorX":0.6,"anchorY":0.5,"zIndex":1}]"""
            .replace("\n", "")
        val raw = """{"schemaVersion":2,"personality":"gc","selectedProfileId":"p1",
            "profiles":[${profileJson(controls)}]}""".replace("\n", "")
        val library = (
            TouchProfileLibraryJsonCodec.decode(raw, TouchProfileId.GameCube)
                as TouchProfileLibraryDecodeResult.Valid
            ).value
        val profile = TouchProfileCatalog.require(TouchProfileId.GameCube)
        val validated = TouchLayoutDocumentValidator.validate(library.activeDocument, profile)
        assertTrue(validated.degraded)
        assertEquals(1, validated.document.controls.count { it.instanceId == "dpad" })
        // The FIRST one survives, so a repair cannot silently relocate a control.
        assertEquals(0.27f, validated.document.controls.single().anchorX, 1e-6f)
    }

    @Test fun `a selection naming a missing profile resolves to the default, not an error`() {
        val raw = """{"schemaVersion":2,"personality":"gc","selectedProfileId":"gone",
            "profiles":[${profileJson(oneControl)}]}""".replace("\n", "")
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
        assertEquals(source.userProfiles.first().document, profile.document)

        val target = TouchProfileLibrary.empty(TouchProfileId.GameCube)
        val edit = applied(TouchProfileLibraryEditor.import(target, profile, T0 + 10))
        assertEquals(1, edit.library.userProfiles.size)
        // The imported document's own id is never reused; the receiving library
        // allocates one, so an import can never collide with what is already there.
        assertNotEquals("imported", edit.library.selectedProfileId)
        assertEquals(profile.document, edit.library.selected.document)
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
            TouchProfileLibraryJsonCodec.decodeExport("""{"schemaVersion":2,"hello":"world"}""")
                is TouchProfileDecodeResult.Invalid,
        )
    }

    @Test fun `every stored profile composes and resolves into a usable controller`() {
        val library = library()
        val profile = TouchProfileCatalog.require(library.personality)
        library.profiles.forEach { entry ->
            val composition = TouchLayoutComposer.compose(profile, entry.document)
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
