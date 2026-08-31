package dev.picoswitch.companion.data

import dev.picoswitch.management.KbmActiveMapping
import dev.picoswitch.management.KbmBinding
import dev.picoswitch.management.KbmDestination
import dev.picoswitch.management.KbmFingerprint
import dev.picoswitch.management.KbmLimits
import dev.picoswitch.management.KbmMouseConfig
import dev.picoswitch.management.KbmPositions
import dev.picoswitch.management.KbmProfile
import dev.picoswitch.management.KbmProfileIds
import dev.picoswitch.management.KbmProfileInfo
import dev.picoswitch.management.KbmProfiles
import dev.picoswitch.management.KbmSource
import dev.picoswitch.management.KbmSourceKind
import dev.picoswitch.management.KbmSwitchBinding
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Where a local profile stands relative to the adapter.
 *
 * THREE INDEPENDENT COMPARISONS, and the reason this projection exists at all:
 * local vs resident, resident vs runtime, and the local-vs-runtime conclusion
 * drawn from them. A single "matches saved" flag cannot express "I edited this
 * locally, the adapter still holds the old copy, and it is running that old
 * copy" — which is the ORDINARY state right after a local save, and the exact
 * thing the user has to be told.
 */
class KbmBankViewTest {

    private val layout = KbmProfile.Keyboard

    private fun key(usage: Int) = KbmSource(KbmSourceKind.Key, usage)

    private fun overrides(usage: Int, destination: KbmDestination) =
        listOf(KbmBinding(key(usage), destination, custom = true))

    private fun fingerprint(bindings: List<KbmBinding>, mouse: KbmMouseConfig = KbmMouseConfig()) =
        KbmFingerprint.compute(layout, KbmFingerprint.canonical(bindings), mouse)

    private fun local(id: String, name: String, bindings: List<KbmBinding> = emptyList()) =
        KbmLocalProfile(
            id = id,
            layout = layout,
            name = name,
            bindings = bindings,
            fingerprint = fingerprint(bindings),
        )

    private fun resident(
        id: Int,
        name: String,
        position: Int,
        bindings: List<KbmBinding> = emptyList(),
    ) = KbmProfileInfo(
        id = id,
        layout = layout,
        name = name,
        revision = 1,
        overrides = bindings.size,
        fingerprint = fingerprint(bindings),
        position = position,
    )

    private fun adapter(
        residents: List<KbmProfileInfo> = emptyList(),
        runtime: Int = KbmPositions.DEFAULT,
        boot: Int = KbmPositions.DEFAULT,
        matchesSaved: Boolean = true,
    ) = KbmProfiles(
        profiles = residents,
        active = listOf(
            KbmActiveMapping(
                layout = layout,
                sourceId = KbmProfileIds.DEFAULT,
                revision = 1,
                fingerprint = 0,
                matchesSaved = matchesSaved,
                bootPosition = boot,
                runtimePosition = runtime,
            ),
        ),
        max = KbmLimits.MAX_PROFILES,
    )

    // ---------------------------------------------------------------- bank

    @Test fun `every position is a row, including the empty ones`() {
        // "Profile 3 — Empty" is what tells a user they have somewhere to assign
        // to. A list of only the occupied positions would hide the capacity.
        val rows = KbmBankView.bank(adapter(), emptyList(), layout)
        assertEquals(KbmLimits.POSITIONS_PER_LAYOUT + 1, rows.size)
        assertEquals(
            listOf("Default", "Profile 1", "Profile 2", "Profile 3"),
            rows.map { it.positionLabel },
        )
        assertTrue(rows.drop(1).all { it.empty })
    }

    @Test fun `runtime and boot are separate facts`() {
        // A switch key moves the runtime choice and NOT the persisted one, so
        // after one press they differ for the rest of the session. Merging them
        // would report the wrong profile as active.
        val rows = KbmBankView.bank(
            adapter(listOf(resident(2, "Halo", 1), resident(3, "Zelda", 2)), runtime = 2, boot = 1),
            emptyList(),
            layout,
        )
        assertTrue(rows.first { it.position == 2 }.isRuntime)
        assertFalse(rows.first { it.position == 2 }.isBoot)
        assertTrue(rows.first { it.position == 1 }.isBoot)
        assertFalse(rows.first { it.position == 1 }.isRuntime)
    }

    @Test fun `a switch key is shown against the position it selects`() {
        val rows = KbmBankView.bank(
            adapter(listOf(resident(2, "Halo", 1))),
            listOf(KbmSwitchBinding(key(0x3A), position = 1)),
            layout,
        )
        assertEquals(key(0x3A), rows.first { it.position == 1 }.switchKey)
        assertNull(rows.first { it.position == 2 }.switchKey)
    }

    @Test fun `Default is a row but consumes no position`() {
        val rows = KbmBankView.bank(adapter(), emptyList(), layout)
        val default = rows.first()
        assertTrue(default.isDefault)
        // Synthesised rather than stored: it is what keeps all three custom
        // positions available to the user.
        assertEquals("Default", default.residentLabel)
    }

    @Test fun `the other layout's bank is separate`() {
        val residents = listOf(
            resident(2, "Halo", 1),
            KbmProfileInfo(
                id = 3, layout = KbmProfile.KeyboardMouse, name = "Combined",
                revision = 1, overrides = 0, fingerprint = 0, position = 1,
            ),
        )
        val kb = KbmBankView.bank(adapter(residents), emptyList(), KbmProfile.Keyboard)
        assertEquals("Halo", kb.first { it.position == 1 }.residentLabel)
    }

    // ------------------------------------------------------------- library

    @Test fun `a profile never sent to this adapter is local only`() {
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Halo"))), adapter(), layout,
        )
        assertEquals(KbmLocalState.LocalOnly, rows.single().state)
        assertNull(rows.single().assignedPosition)
        assertEquals("Local only", rows.single().stateLabel)
    }

    @Test fun `an assigned profile in agreement reads as on adapter`() {
        val content = overrides(0x04, KbmDestination.Zr)
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Halo", content))),
            adapter(listOf(resident(2, "Halo", 1, content))),
            layout,
        )
        assertEquals(KbmLocalState.OnAdapter, rows.single().state)
        assertEquals(1, rows.single().assignedPosition)
    }

    @Test fun `a local save leaves the resident copy behind and says so`() {
        // The state the whole model exists to express. The local profile has
        // moved on; the adapter still holds — and may still be running — the
        // older content, and only an explicit Update resolves it.
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Halo", overrides(0x04, KbmDestination.Zr)))),
            adapter(listOf(resident(2, "Halo", 1, overrides(0x04, KbmDestination.Zl)))),
            layout,
        )
        assertEquals(KbmLocalState.AdapterCopyOutOfDate, rows.single().state)
        assertTrue(rows.single().canUpdateAdapterCopy)
        assertTrue(rows.single().stateLabel.contains("out of date"))
    }

    @Test fun `the running profile reads as active`() {
        val content = overrides(0x04, KbmDestination.Zr)
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Halo", content))),
            adapter(listOf(resident(2, "Halo", 1, content)), runtime = 1),
            layout,
        )
        assertEquals(KbmLocalState.Active, rows.single().state)
        assertFalse(rows.single().canUpdateAdapterCopy)
    }

    @Test fun `a resident updated under a running position asks to be activated`() {
        // Assigning into the active position deliberately does NOT mutate
        // gameplay: the realized snapshot is kept until the user activates. This
        // state is how the screen says so.
        val content = overrides(0x04, KbmDestination.Zr)
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Halo", content))),
            adapter(
                listOf(resident(2, "Halo", 1, content)), runtime = 1, matchesSaved = false,
            ),
            layout,
        )
        assertEquals(KbmLocalState.ResidentUpdatedNotActivated, rows.single().state)
        assertTrue(rows.single().stateLabel.contains("activate"))
    }

    @Test fun `identical local profiles do not both claim one resident`() {
        // Two untouched copies of Default have identical content. Without unique
        // claiming both would be reported as "on adapter", and the user would be
        // told the adapter holds two profiles it does not have.
        val library = KbmProfileLibrary(listOf(local("a", "First"), local("b", "Second")))
        val rows = KbmBankView.library(
            library, adapter(listOf(resident(2, "First", 1))), layout,
        )
        assertEquals(1, rows.count { it.state != KbmLocalState.LocalOnly })
        // The strongest evidence wins: name AND content beats content alone.
        assertEquals(
            KbmLocalState.OnAdapter,
            rows.first { it.profile.name == "First" }.state,
        )
        assertEquals(
            KbmLocalState.LocalOnly,
            rows.first { it.profile.name == "Second" }.state,
        )
    }

    @Test fun `an edited profile keeps its own resident rather than one it now equals`() {
        // THE ORDERING THIS PINS: name outranks content alone.
        //
        // Editing locally is the common case and changes content, so right after
        // a save a profile no longer matches its own resident copy — and may
        // coincidentally equal a different one. Matching on content first made
        // "Halo" claim the unrelated Profile 2 and report itself safely on the
        // adapter, hiding the one fact the user needed: Halo's own copy is stale.
        val edited = overrides(0x09, KbmDestination.Gr)
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Halo", edited))),
            adapter(
                listOf(
                    resident(2, "Halo", 1, overrides(0x04, KbmDestination.Zr)),
                    // Same content as the edited local profile, different name.
                    resident(3, "Zelda", 2, edited),
                ),
            ),
            layout,
        )
        assertEquals(KbmLocalState.AdapterCopyOutOfDate, rows.single().state)
        assertEquals(1, rows.single().assignedPosition)
    }

    @Test fun `content still decides when no name matches`() {
        // What keeps a locally renamed profile attached to the resident the other
        // companion wrote: ids are not shared, so content is all that is left.
        val content = overrides(0x04, KbmDestination.Zr)
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "My Halo", content))),
            adapter(listOf(resident(2, "Halo", 1, content))),
            layout,
        )
        assertEquals(KbmLocalState.OnAdapter, rows.single().state)
        assertEquals(1, rows.single().assignedPosition)
    }

    @Test fun `a renamed local profile still recognises its resident copy`() {
        // Renaming changes no behaviour, so the content match must still hold and
        // the row must report divergence rather than "local only".
        val content = overrides(0x04, KbmDestination.Zr)
        val rows = KbmBankView.library(
            KbmProfileLibrary(listOf(local("a", "Zelda", content))),
            adapter(listOf(resident(2, "Halo", 1, content))),
            layout,
        )
        assertEquals(KbmLocalState.OnAdapter, rows.single().state)
    }

    @Test fun `a resident the library does not have produces no row`() {
        // A profile assigned from the other companion is not silently adopted:
        // importing it is an explicit Copy to library.
        val rows = KbmBankView.library(
            KbmProfileLibrary.EMPTY, adapter(listOf(resident(2, "Halo", 1))), layout,
        )
        assertTrue(rows.isEmpty())
    }

    @Test fun `the library projection is scoped to one layout`() {
        val library = KbmProfileLibrary(
            listOf(
                local("kb", "Halo"),
                KbmLocalProfile(
                    id = "kbm", layout = KbmProfile.KeyboardMouse, name = "Halo",
                ),
            ),
        )
        assertEquals(listOf("kb"), KbmBankView.library(library, adapter(), layout).map { it.profile.id })
    }

    // -------------------------------------------------------- switch keys

    @Test fun `every switch action is a row, bound or not`() {
        // Built from the ACTIONS, not the bindings: a list built from the
        // bindings would hide the three actions the user came here to set up.
        val actions = KbmBankView.switchActions(listOf(KbmSwitchBinding(key(0x3A), 1)))
        assertEquals(KbmLimits.POSITIONS_PER_LAYOUT + 1, actions.size)
        assertEquals(key(0x3A), actions.first { it.first == 1 }.second)
        assertTrue(actions.filter { it.first != 1 }.all { it.second == null })
    }

    @Test fun `switch actions include Default`() {
        // "Go back to the built-in mapping" is a real thing to want a key for.
        assertTrue(KbmBankView.switchActions(emptyList()).any { it.first == KbmPositions.DEFAULT })
    }
}
