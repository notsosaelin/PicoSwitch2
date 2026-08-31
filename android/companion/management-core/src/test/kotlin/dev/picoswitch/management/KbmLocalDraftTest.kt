package dev.picoswitch.management

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The editor's working copy: local, offline, and only ever clean or dirty.
 *
 * The state model is the thing under test as much as the edits are. The draft
 * this replaces was keyed on an ADAPTER profile id and carried adapter-derived
 * states — active, saved-not-applied, conflicted — which is how "the profile I
 * have open" and "the profile resident on the adapter" became the same object.
 * Where a profile stands relative to a device is a RELATIONSHIP and belongs to
 * the bank projection, not here.
 */
class KbmLocalDraftTest {

    private val layout = KbmProfile.Keyboard

    private fun defaultBinding(index: Int = 0): KbmBinding =
        KbmDefaults.forLayout(layout).bindings[index]

    private fun other(destination: KbmDestination): KbmDestination =
        if (destination == KbmDestination.A) KbmDestination.B else KbmDestination.A

    @Test fun `a fresh draft on Default is clean and built-in`() {
        val draft = KbmLocalDraft.fromDefault(layout)
        assertTrue(draft.isBuiltin)
        assertFalse(draft.dirty)
        // Nothing of the user's is in it yet, so there is nothing to assign.
        assertTrue(draft.overrides.isEmpty())
    }

    @Test fun `a rebind makes it dirty and sends nothing`() {
        val binding = defaultBinding()
        val draft = KbmLocalDraft.fromDefault(layout)
            .with(binding.source, other(binding.destination))
        assertTrue(draft.dirty)
        assertEquals(1, draft.overrides.size)
    }

    @Test fun `putting a key back makes the draft clean again`() {
        // Dirty is a CONTENT comparison, not a latched flag. A user who changes
        // a key and changes it back has nothing to save, and offering Save would
        // write a profile identical to the one already stored.
        val binding = defaultBinding()
        val draft = KbmLocalDraft.fromDefault(layout)
            .with(binding.source, other(binding.destination))
            .with(binding.source, binding.destination)
        assertFalse(draft.dirty)
        // The redundant override is DROPPED rather than stored, so the profile's
        // content is genuinely equal to the default and fingerprints as such.
        assertTrue(draft.overrides.isEmpty())
    }

    @Test fun `binding not-mapped is an override and is kept`() {
        // "This key does nothing" differs from the default the adapter would
        // otherwise apply, so it must be stored rather than dropped as redundant.
        val binding = defaultBinding()
        val draft = KbmLocalDraft.fromDefault(layout)
            .with(binding.source, KbmDestination.None)
        assertTrue(draft.dirty)
        assertEquals(1, draft.overrides.size)
        assertEquals(
            KbmDestination.None,
            draft.effective.first { it.source == binding.source }.destination,
        )
    }

    @Test fun `restore removes the override rather than binding none`() {
        val binding = defaultBinding()
        val draft = KbmLocalDraft.fromDefault(layout)
            .with(binding.source, KbmDestination.None)
            .restore(binding.source)
        assertFalse(draft.dirty)
        assertTrue(draft.overrides.isEmpty())
        assertEquals(
            binding.destination,
            draft.effective.first { it.source == binding.source }.destination,
        )
    }

    @Test fun `renaming makes it dirty`() {
        val draft = KbmLocalDraft(
            profileId = "abc", layout = layout, name = "Halo", baseName = "Halo",
        )
        assertFalse(draft.dirty)
        assertTrue(draft.withName("Halo 2").dirty)
    }

    @Test fun `mouse tuning is part of the draft's content`() {
        val draft = KbmLocalDraft(
            profileId = "abc", layout = KbmProfile.KeyboardMouse,
            name = "Halo", baseName = "Halo",
            mouse = KbmMouseConfig(sensitivityX = 512),
            baseMouse = KbmMouseConfig(sensitivityX = 512),
        )
        assertFalse(draft.dirty)
        val tuned = draft.withMouse(KbmMouseConfig(sensitivityX = 1024))
        assertTrue(tuned.dirty)
        assertNotEquals(draft.fingerprint, tuned.fingerprint)
    }

    @Test fun `discard restores the last saved content`() {
        val binding = defaultBinding()
        val draft = KbmLocalDraft.fromDefault(layout)
            .with(binding.source, other(binding.destination))
            .withName("Something else")
        val discarded = draft.discard()
        assertFalse(discarded.dirty)
        assertEquals("Default", discarded.name)
        assertTrue(discarded.overrides.isEmpty())
    }

    @Test fun `effective composes the whole mapping without an adapter`() {
        // The grid draws from this. Nothing in this test constructs a transport,
        // a client or a connection, which is the property that matters: editing
        // must work with nothing paired.
        val draft = KbmLocalDraft.fromDefault(layout)
        assertEquals(KbmDefaults.forLayout(layout).bindings.size, draft.effective.size)
    }

    @Test fun `the fingerprint ignores the name`() {
        // Renaming changes no behaviour, so a resident copy that matched before a
        // rename must still match after it.
        val a = KbmLocalDraft(profileId = "1", layout = layout, name = "Halo", baseName = "Halo")
        val b = a.withName("Zelda")
        assertEquals(a.fingerprint, b.fingerprint)
    }

    @Test fun `two drafts with the same content fingerprint the same`() {
        // What lets a local profile recognise its own resident copy across two
        // companions that share no ids.
        val binding = defaultBinding()
        val a = KbmLocalDraft.fromDefault(layout).with(binding.source, KbmDestination.Zr)
        val b = KbmLocalDraft(
            profileId = "other", layout = layout, name = "Whatever", baseName = "Whatever",
            overrides = listOf(KbmBinding(binding.source, KbmDestination.Zr, custom = true)),
            mouse = KbmDefaults.forLayout(layout).mouse,
        )
        assertEquals(a.fingerprint, b.fingerprint)
    }
}
