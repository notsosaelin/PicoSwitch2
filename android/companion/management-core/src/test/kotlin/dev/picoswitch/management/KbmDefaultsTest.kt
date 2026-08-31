package dev.picoswitch.management

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The canonical default mapping shipped in the app must be the firmware's.
 *
 * WHY THE APP CARRIES IT AT ALL. A local profile stores only sparse overrides —
 * the same representation the adapter stores, which is what makes an assignment
 * a copy rather than a translation. Drawing the grid, or answering "what does
 * this key do", means applying those overrides to the canonical table. Fetching
 * that table from the adapter would make creating and editing a profile require
 * a connection, which is the defect this whole model removes.
 *
 * Carrying a copy is only safe while it is verifiably the same copy, which is
 * what this test is for. The fixture is generated from src/ns2_kbm.c by
 * tools/test_ns2_kbm_commands.c; if the firmware's defaults change and the
 * fixture is not regenerated, this fails rather than letting the app draw a
 * mapping the adapter does not have.
 */
class KbmDefaultsTest {

    private val fixture = Json.parseToJsonElement(
        checkNotNull(javaClass.getResource("/kbm-default-mappings.json")).readText(),
    ).jsonObject["layouts"]!!.jsonObject

    @Test fun `the shipped table is present and non-empty for both layouts`() {
        // A missing resource degrades to an empty table at runtime rather than
        // crashing; this is what stops that happening silently in a shipped build.
        KbmProfile.entries.forEach { layout ->
            val mapping = KbmDefaults.forLayout(layout)
            assertTrue(
                "${layout.wire} has no default bindings",
                mapping.bindings.isNotEmpty(),
            )
        }
    }

    @Test fun `every binding matches the firmware fixture exactly`() {
        KbmProfile.entries.forEach { layout ->
            val expected = fixture[layout.wire]!!.jsonObject["bindings"]!!.jsonArray
                .map { row ->
                    val item = row.jsonObject
                    "${item["src"]!!.jsonPrimitive.content}=" +
                        item["dst"]!!.jsonPrimitive.content
                }
            val actual = KbmDefaults.forLayout(layout).bindings
                .map { "${it.source.wire}=${it.destination.wire}" }
            assertEquals(layout.wire, expected.sorted(), actual.sorted())
        }
    }

    @Test fun `default bindings are not marked as user overrides`() {
        // The grid marks changed rows, and a canonical binding flagged custom
        // would report every key as changed in a brand-new profile.
        KbmProfile.entries.forEach { layout ->
            assertTrue(
                "${layout.wire} marks defaults as custom",
                KbmDefaults.forLayout(layout).bindings.none { it.custom },
            )
        }
    }

    @Test fun `the two layouts differ`() {
        // Keyboard-only drives both sticks from keys; keyboard+mouse gives the
        // right stick to the mouse. If these were equal the layout distinction
        // would be decorative.
        assertTrue(
            KbmDefaults.forLayout(KbmProfile.Keyboard).bindings !=
                KbmDefaults.forLayout(KbmProfile.KeyboardMouse).bindings,
        )
    }

    @Test fun `effective applies an override over the canonical table`() {
        val layout = KbmProfile.Keyboard
        val target = KbmDefaults.forLayout(layout).bindings.first()
        val replacement = if (target.destination == KbmDestination.A) {
            KbmDestination.B
        } else {
            KbmDestination.A
        }

        val effective = KbmDefaults.effective(
            layout,
            listOf(KbmBinding(target.source, replacement, custom = true)),
        )

        val row = effective.firstOrNull { it.source == target.source }
        assertNotNull(row)
        assertEquals(replacement, row!!.destination)
        assertTrue("an override must be marked changed", row.custom)
        // An override REPLACES a row; it never adds one.
        assertEquals(KbmDefaults.forLayout(layout).bindings.size, effective.size)
    }

    @Test fun `an explicit not-mapped override survives composition`() {
        // Dropping it would silently restore the adapter's canonical binding,
        // which is the opposite of what the user asked for.
        val layout = KbmProfile.Keyboard
        val target = KbmDefaults.forLayout(layout).bindings.first()
        val effective = KbmDefaults.effective(
            layout,
            listOf(KbmBinding(target.source, KbmDestination.None, custom = true)),
        )
        assertEquals(
            KbmDestination.None,
            effective.first { it.source == target.source }.destination,
        )
    }

    @Test fun `effective is ordered the way the fingerprint orders overrides`() {
        // One ordering rule, used by the grid and by the digest, so a mapping
        // cannot be drawn in one order and hashed in another.
        val effective = KbmDefaults.effective(KbmProfile.KeyboardMouse, emptyList())
        val sorted = effective.sortedWith(
            compareBy(
                { KbmFingerprint.firmwareCode(it.source.kind) },
                { it.source.code },
            ),
        )
        assertEquals(sorted, effective)
    }
}
