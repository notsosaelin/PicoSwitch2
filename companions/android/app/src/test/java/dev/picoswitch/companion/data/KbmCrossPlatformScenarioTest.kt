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
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Two companions, one adapter, and what actually travels between them.
 *
 * WHAT THIS DOES AND DOES NOT CLAIM. No Windows process runs here. What is
 * verified is the CONTRACT the two companions share, which is the only thing
 * that can carry a profile between them:
 *
 *  - Local ids are NOT shared. Windows mints its own, Android mints its own, and
 *    neither ever sees the other's. A profile crosses only as content resident
 *    on the adapter.
 *  - The bridge is therefore the FINGERPRINT plus the sparse override set. Both
 *    sides compute the fingerprint with an implementation replayed against
 *    vectors emitted by the firmware's own `ns2_kbm_content_fingerprint`
 *    (KbmFingerprintParityTest here, its C# counterpart there), so agreement is
 *    established by both agreeing with the firmware rather than with each other.
 *
 * The "Windows" side of this scenario is therefore built from those same
 * firmware vectors: the residents below carry exactly the fingerprints the
 * adapter itself produces for that content, which is precisely what a Windows
 * companion would have written. If either client's implementation drifted, its
 * own parity test fails first.
 *
 * The scenario is the user-facing one: four profiles on one companion, three of
 * them assigned to the adapter, and the fourth staying put.
 */
class KbmCrossPlatformScenarioTest {

    private val layout = KbmProfile.Keyboard

    // ------------------------------------------------------------ fixtures

    private data class Vector(
        val layout: KbmProfile,
        val overrides: List<KbmBinding>,
        val mouse: KbmMouseConfig,
        val fingerprint: Long,
    )

    private val vectors: Map<String, Vector> = run {
        val corpus = Json.parseToJsonElement(
            checkNotNull(javaClass.classLoader!!.getResource("kbm-wire-corpus.json")).readText(),
        ).jsonObject
        corpus["fingerprints"]!!.jsonArray.associate { element ->
            val item = element.jsonObject
            val mouse = item["mouse"]!!.jsonObject
            item["label"]!!.jsonPrimitive.content to Vector(
                layout = checkNotNull(KbmProfile.fromWire(item["layout"]!!.jsonPrimitive.content)),
                overrides = item["overrides"]!!.jsonArray.map { row ->
                    val binding = row.jsonObject
                    KbmBinding(
                        source = checkNotNull(
                            KbmSource.parse(binding["src"]!!.jsonPrimitive.content),
                        ),
                        destination = checkNotNull(
                            KbmDestination.fromWire(binding["dst"]!!.jsonPrimitive.content),
                        ),
                        custom = true,
                    )
                },
                mouse = KbmMouseConfig(
                    sensitivityX = mouse["sensitivityX"]!!.jsonPrimitive.int,
                    sensitivityY = mouse["sensitivityY"]!!.jsonPrimitive.int,
                    velocityWindowMs = mouse["velocityWindowMs"]!!.jsonPrimitive.int,
                    invertX = mouse["invertX"]!!.jsonPrimitive.boolean,
                    invertY = mouse["invertY"]!!.jsonPrimitive.boolean,
                    antiDeadzone = mouse["antiDeadzone"]!!.jsonPrimitive.int,
                ),
                fingerprint = item["fingerprint"]!!.jsonPrimitive.long,
            )
        }
    }

    private fun vector(label: String): Vector = checkNotNull(vectors[label]) { label }

    private class MemoryStore(
        private var document: KbmProfileLibrary = KbmProfileLibrary.EMPTY,
    ) : KbmProfileLibraryStore {
        override fun load(): KbmProfileLibrary = document
        override fun save(library: KbmProfileLibrary) { document = library }
    }

    /**
     * What the adapter reports after the OTHER companion assigned these.
     *
     * Fingerprints come from the firmware vectors, so these residents are
     * byte-for-byte what the adapter would hold.
     */
    private fun adapterHolding(
        assignments: List<Triple<Int, String, String>>,
        runtime: Int = KbmPositions.DEFAULT,
        boot: Int = KbmPositions.DEFAULT,
        matchesSaved: Boolean = true,
    ): KbmProfiles {
        val residents = assignments.mapIndexed { index, (position, name, label) ->
            val v = vector(label)
            KbmProfileInfo(
                id = 2 + index,
                layout = v.layout,
                name = name,
                revision = 1,
                overrides = v.overrides.size,
                fingerprint = v.fingerprint,
                position = position,
            )
        }
        return KbmProfiles(
            profiles = residents,
            active = KbmProfile.entries.map { entry ->
                KbmActiveMapping(
                    layout = entry,
                    sourceId = KbmProfileIds.DEFAULT,
                    revision = 1,
                    fingerprint = 0,
                    matchesSaved = matchesSaved,
                    bootPosition = if (entry == layout) boot else KbmPositions.DEFAULT,
                    runtimePosition = if (entry == layout) runtime else KbmPositions.DEFAULT,
                )
            },
            max = KbmLimits.MAX_PROFILES,
        )
    }

    /** Positions 1/2/3 = Halo/Zelda/Tekken, as the first companion assigned them. */
    private fun windowsAssignedBank(
        runtime: Int = KbmPositions.DEFAULT,
        matchesSaved: Boolean = true,
    ) = adapterHolding(
        listOf(
            Triple(1, "Halo", "kb-one-rebind"),
            Triple(2, "Zelda", "kb-unsorted-rebinds"),
            Triple(3, "Tekken", "kb-cleared-binding"),
        ),
        runtime = runtime,
        matchesSaved = matchesSaved,
    )

    // ---------------------------------------------------- Windows → Android

    @Test fun `the second companion sees the residents and none of the first's local-only work`() {
        // "Coding" was never assigned, so it exists only in the first companion's
        // library and MUST NOT appear here. Nothing carries a local-only profile
        // between companions, and inventing one would be inventing content.
        val android = KbmLibraryRepository(MemoryStore())
        val bank = KbmBankView.bank(windowsAssignedBank(), emptyList(), layout)

        assertEquals(
            listOf("Default", "Halo", "Zelda", "Tekken"),
            bank.map { it.residentLabel },
        )
        assertTrue("nothing may be adopted implicitly", android.value.profiles.isEmpty())
        assertTrue(
            "a local-only profile cannot cross",
            bank.none { it.residentLabel == "Coding" },
        )
    }

    @Test fun `copying a resident into the second library recognises it as the same profile`() {
        val android = KbmLibraryRepository(MemoryStore())
        val adapter = windowsAssignedBank()
        val halo = vector("kb-one-rebind")

        val imported = android.import(layout, "Halo", halo.overrides, halo.mouse)

        // The two libraries hold different ids for the same profile, which is
        // exactly why the fingerprint is what the relationship is computed from.
        assertEquals(halo.fingerprint, imported.fingerprint)
        val row = KbmBankView.library(android.value, adapter, layout)
            .first { it.profile.id == imported.id }
        assertEquals(KbmLocalState.OnAdapter, row.state)
        assertEquals(1, row.assignedPosition)
    }

    @Test fun `copying the same resident twice does not duplicate it`() {
        val android = KbmLibraryRepository(MemoryStore())
        val halo = vector("kb-one-rebind")
        val first = android.import(layout, "Halo", halo.overrides, halo.mouse)
        val again = android.import(layout, "Halo", halo.overrides, halo.mouse)
        assertEquals(first.id, again.id)
        assertEquals(1, android.value.profiles.size)
    }

    @Test fun `a local save on the second companion leaves the adapter alone and reports divergence`() {
        // The central guarantee. Editing and saving changes the LIBRARY. The
        // resident copy keeps the fingerprint the first companion wrote, and the
        // row says so rather than pretending the adapter is up to date.
        val android = KbmLibraryRepository(MemoryStore())
        val adapter = windowsAssignedBank()
        val halo = vector("kb-one-rebind")
        val edited = vector("kb-unsorted-rebinds")

        val imported = android.import(layout, "Halo", halo.overrides, halo.mouse)
        val saved = android.save(imported.id, "Halo", edited.overrides, edited.mouse)

        assertNotEquals(halo.fingerprint, saved.fingerprint)
        assertEquals(
            "the adapter's copy must be untouched by a local save",
            halo.fingerprint,
            adapter.at(layout, 1)!!.fingerprint,
        )

        val row = KbmBankView.library(android.value, adapter, layout).single()
        assertEquals(KbmLocalState.AdapterCopyOutOfDate, row.state)
        assertTrue(row.canUpdateAdapterCopy)
    }

    @Test fun `an explicit update is what changes the resident fingerprint`() {
        // Modelled as the adapter reporting the new content afterwards, which is
        // what a re-read returns once `kbm draft begin pos:N` … commit lands.
        val android = KbmLibraryRepository(MemoryStore())
        val halo = vector("kb-one-rebind")
        val edited = vector("kb-unsorted-rebinds")

        val imported = android.import(layout, "Halo", halo.overrides, halo.mouse)
        android.save(imported.id, "Halo", edited.overrides, edited.mouse)

        val afterUpdate = adapterHolding(
            listOf(
                Triple(1, "Halo", "kb-unsorted-rebinds"),
                Triple(2, "Zelda", "kb-unsorted-rebinds"),
                Triple(3, "Tekken", "kb-cleared-binding"),
            ),
        )
        assertEquals(edited.fingerprint, afterUpdate.at(layout, 1)!!.fingerprint)

        val rows = KbmBankView.library(android.value, afterUpdate, layout)
        val halosRow = rows.first { it.profile.id == imported.id }
        assertEquals(KbmLocalState.OnAdapter, halosRow.state)
        assertFalse(halosRow.canUpdateAdapterCopy)
    }

    // ---------------------------------------------------- Android → Windows

    @Test fun `the first companion sees the change and its own copy stays independent`() {
        // The inverse leg. The first companion still holds the ORIGINAL Halo in
        // its library; the adapter now holds the second companion's version. Its
        // row must report divergence, and its local content must be unchanged —
        // a remote update is not allowed to rewrite anyone's library.
        val windows = KbmLibraryRepository(MemoryStore())
        val halo = vector("kb-one-rebind")
        val edited = vector("kb-unsorted-rebinds")

        val local = windows.create(layout, "Halo", halo.overrides, halo.mouse)
        val afterOtherCompanionUpdated = adapterHolding(
            listOf(Triple(1, "Halo", "kb-unsorted-rebinds")),
        )

        assertEquals(
            "a remote update must not touch this library",
            halo.fingerprint,
            windows.value.find(local.id)!!.fingerprint,
        )
        val row = KbmBankView.library(
            windows.value, afterOtherCompanionUpdated, layout,
        ).single()
        assertEquals(KbmLocalState.AdapterCopyOutOfDate, row.state)
        assertNotEquals(edited.fingerprint, row.profile.fingerprint)
    }

    @Test fun `the inverse leg works for a keyboard and mouse profile`() {
        // Repeated in the other bank because position is scoped to a layout: the
        // same number in the other bank is a different profile, and a projection
        // that ignored the layout would let one claim the other's position.
        val combined = KbmProfile.KeyboardMouse
        val android = KbmLibraryRepository(MemoryStore())
        val v = vector("kbm-mouse-only")

        val created = android.create(combined, "Combined", v.overrides, v.mouse)
        assertEquals(v.fingerprint, created.fingerprint)

        val adapter = adapterHolding(listOf(Triple(1, "Combined", "kbm-mouse-only")))
        val windows = KbmLibraryRepository(MemoryStore())
        val imported = windows.import(combined, "Combined", v.overrides, v.mouse)
        assertEquals(created.fingerprint, imported.fingerprint)

        val row = KbmBankView.library(windows.value, adapter, combined).single()
        assertEquals(KbmLocalState.OnAdapter, row.state)
        assertEquals(1, row.assignedPosition)

        // And the keyboard bank is untouched by any of it.
        assertTrue(KbmBankView.library(windows.value, adapter, KbmProfile.Keyboard).isEmpty())
    }

    // ------------------------------------------------- independent lifetimes

    @Test fun `deleting locally leaves the resident copy running`() {
        val android = KbmLibraryRepository(MemoryStore())
        val adapter = windowsAssignedBank(runtime = 1)
        val halo = vector("kb-one-rebind")
        val imported = android.import(layout, "Halo", halo.overrides, halo.mouse)

        android.delete(imported.id)

        assertTrue(android.value.profiles.isEmpty())
        assertEquals(halo.fingerprint, adapter.at(layout, 1)!!.fingerprint)
        assertEquals(1, adapter.activeFor(layout)!!.runtimePosition)
    }

    @Test fun `removing the resident leaves the local profile in the library`() {
        val android = KbmLibraryRepository(MemoryStore())
        val halo = vector("kb-one-rebind")
        val imported = android.import(layout, "Halo", halo.overrides, halo.mouse)

        // What the adapter reports after `kbm remove kb 1`: the position is empty
        // and the layout has fallen back to Default rather than dangling.
        val afterRemoval = adapterHolding(
            listOf(Triple(2, "Zelda", "kb-unsorted-rebinds")),
            runtime = KbmPositions.DEFAULT,
            boot = KbmPositions.DEFAULT,
        )

        assertNull(afterRemoval.at(layout, 1))
        assertEquals(KbmPositions.DEFAULT, afterRemoval.activeFor(layout)!!.runtimePosition)
        assertEquals(KbmPositions.DEFAULT, afterRemoval.activeFor(layout)!!.bootPosition)

        val row = KbmBankView.library(android.value, afterRemoval, layout).single()
        assertEquals(imported.id, row.profile.id)
        assertEquals(KbmLocalState.LocalOnly, row.state)
        assertEquals(halo.fingerprint, row.profile.fingerprint)
    }

    @Test fun `reconnecting restores every relationship from content alone`() {
        // Nothing about the relationship is persisted: it is recomputed from the
        // library and whatever the adapter reports. A cached "on adapter" flag is
        // the lie this model exists to prevent — it survives the profile being
        // removed by someone else.
        val store = MemoryStore()
        val first = KbmLibraryRepository(store)
        val halo = vector("kb-one-rebind")
        val zelda = vector("kb-unsorted-rebinds")
        first.import(layout, "Halo", halo.overrides, halo.mouse)
        first.import(layout, "Zelda", zelda.overrides, zelda.mouse)
        first.create(layout, "Coding")

        // Disconnected: the library is fully readable and nothing claims a
        // position, because there is no adapter to claim one on.
        val offline = KbmBankView.library(store.load(), KbmProfiles(), layout)
        assertEquals(3, offline.size)
        assertTrue(offline.all { it.state == KbmLocalState.LocalOnly })

        // Reconnected, in a fresh process, against the same adapter.
        val restarted = KbmLibraryRepository(store)
        val rows = KbmBankView.library(restarted.value, windowsAssignedBank(runtime = 1), layout)
        assertEquals(KbmLocalState.Active, rows.first { it.profile.name == "Halo" }.state)
        assertEquals(KbmLocalState.OnAdapter, rows.first { it.profile.name == "Zelda" }.state)
        assertEquals(KbmLocalState.LocalOnly, rows.first { it.profile.name == "Coding" }.state)
    }
}
