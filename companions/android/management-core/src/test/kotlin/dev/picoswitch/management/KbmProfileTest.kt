package dev.picoswitch.management

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The profile library, the local draft, and the Save-vs-Apply distinction.
 *
 * Mirrors the Windows tests deliberately: both companions must agree about what
 * dirty, conflicted and applied MEAN, or one of them will show a user something
 * the other contradicts.
 */
class KbmProfileTest {
    private val work = KbmProfileInfo(
        id = 2,
        layout = KbmProfile.Keyboard,
        name = "Work",
        revision = 3,
        overrides = 3,
        fingerprint = 111,
    )

    private fun library(
        activeSourceId: Int = 2,
        activeMatchesSaved: Boolean = true,
        profiles: List<KbmProfileInfo> = listOf(work),
    ) = KbmProfiles(
        profiles = profiles,
        active = listOf(
            KbmActiveMapping(KbmProfile.Keyboard, activeSourceId, 3, 111, activeMatchesSaved),
            KbmActiveMapping(KbmProfile.KeyboardMouse, KbmProfileIds.DEFAULT, 0, 0, true),
        ),
        max = 6,
    )

    private fun draft(profile: KbmProfileInfo = work) =
        KbmDraft.from(profile, emptyList(), KbmMouseConfig())

    private fun key(usage: Int) = KbmSource(KbmSourceKind.Key, usage)

    @Test
    fun `default is synthesised and listed first`() {
        // Default is a TEMPLATE the adapter never stores, which is exactly what
        // keeps all six slots available to the user.
        val rows = library().forLayout(KbmProfile.Keyboard)
        assertEquals(2, rows.size)
        assertTrue(rows[0].builtin)
        assertEquals("Default", rows[0].name)
        assertEquals("Work", rows[1].name)
        // A profile belongs to one layout and is never offered under the other.
        assertEquals(1, library().forLayout(KbmProfile.KeyboardMouse).size)
    }

    @Test
    fun `an adapter without a profile library is not supported, not broken`() {
        assertFalse(KbmProfiles().supported)
        assertTrue(library().supported)
        assertFalse(library().full)
        assertTrue(library(profiles = (2..7).map { work.copy(id = it) }).full)
    }

    @Test
    fun `editing a draft sends nothing and is reversible`() {
        // THE central requirement. Every edit here is a pure transformation:
        // there is no client in scope to write to.
        var edited = draft()
        assertFalse(edited.dirty)

        edited = edited.with(key(0x04), KbmDestination.Zr).with(key(0x05), KbmDestination.Zl)
        assertTrue(edited.dirty)
        assertEquals(2, edited.bindings.size)

        // Dirty is computed from CONTENT, so Discard genuinely restores clean.
        assertFalse(edited.discard().dirty)
    }

    @Test
    fun `rebinding one input replaces it rather than accumulating`() {
        val edited = draft()
            .with(key(0x04), KbmDestination.Zr)
            .with(key(0x04), KbmDestination.Zl)
        assertEquals(1, edited.bindings.size)
        assertEquals(KbmDestination.Zl, edited.bindings.single().destination)
    }

    @Test
    fun `saved but not applied is its own state`() {
        // The state the whole feature exists to express: the profile that
        // produced the realized mapping has been edited and saved since, so the
        // console is still running the old behaviour.
        val state = draft().stateAgainst(library(activeMatchesSaved = false), connected = true)
        assertEquals(KbmDraftState.SavedNotApplied, state)
    }

    @Test
    fun `active requires both the id and the content to match`() {
        // An id match alone is what would let the UI claim "Active" for a
        // profile that was saved and never applied.
        assertEquals(
            KbmDraftState.Active,
            draft().stateAgainst(library(), connected = true),
        )
        assertEquals(
            KbmDraftState.SavedNotApplied,
            draft().stateAgainst(library(activeSourceId = 9), connected = true),
        )
    }

    @Test
    fun `an edit outranks the applied state`() {
        // A dirty draft is not "Active" even when the profile it came from is:
        // what is on screen is not what the adapter has.
        val edited = draft().with(key(0x04), KbmDestination.Zr)
        assertEquals(KbmDraftState.Dirty, edited.stateAgainst(library(), connected = true))
    }

    @Test
    fun `another companions save shows as a conflict`() {
        // The draft was based on revision 3; the adapter now reports 5. Saving
        // over that would discard whatever the other companion stored.
        val moved = library(profiles = listOf(work.copy(revision = 5)))
        assertEquals(KbmDraftState.Conflict, draft().stateAgainst(moved, connected = true))
    }

    @Test
    fun `disconnected never claims active`() {
        // A cached "Active" is exactly the lie this model exists to prevent.
        assertEquals(
            KbmDraftState.Disconnected,
            draft().stateAgainst(library(), connected = false),
        )
    }

    @Test
    fun `a saved draft is clean against the revision the adapter reported`() {
        val edited = draft().with(key(0x04), KbmDestination.Zr)
        assertTrue(edited.dirty)

        val rebased = edited.rebased(profileId = 2, revision = 4, name = "Work")
        assertFalse(rebased.dirty)
        assertEquals(4, rebased.baseRevision)
        // ...and is now stale against the revision it was built from, which is
        // what makes a second save with the old base a conflict.
        assertEquals(
            KbmDraftState.Conflict,
            rebased.stateAgainst(library(), connected = true),
        )
    }

    @Test
    fun `the built-in default is editable but not a stored profile`() {
        val template = library().forLayout(KbmProfile.Keyboard).first { it.builtin }
        val fromDefault = draft(template)
        assertTrue(fromDefault.isBuiltin)
        // No stored revision to conflict against, so editing it is never a
        // conflict -- saving it creates a new profile instead.
        assertEquals(
            KbmDraftState.Dirty,
            fromDefault.with(key(0x04), KbmDestination.Zr)
                .stateAgainst(library(), connected = true),
        )
        assertNull(library().find(KbmProfileIds.DEFAULT))
    }

    @Test
    fun `profile-owned mouse settings are all written`() {
        // X and Y are sent separately: a profile may legitimately carry
        // different axis sensitivities, and the combined `sensitivity` form
        // would silently flatten them.
        val fields = KbmMouseField.profileOwned(
            KbmMouseConfig(sensitivityX = 500, sensitivityY = 700, antiDeadzone = 12),
        )
        assertEquals(6, fields.size)
        assertEquals(500, fields.first { it.first == KbmMouseField.SensitivityX }.second)
        assertEquals(700, fields.first { it.first == KbmMouseField.SensitivityY }.second)
        assertEquals(12, fields.first { it.first == KbmMouseField.AntiDeadzone }.second)
        // The combined form is deliberately not among them.
        assertFalse(fields.any { it.first == KbmMouseField.Sensitivity })
    }
}
