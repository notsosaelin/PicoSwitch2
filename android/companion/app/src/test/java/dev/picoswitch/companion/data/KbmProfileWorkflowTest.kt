package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.protocol.ManagementTransport
import dev.picoswitch.management.KbmBinding
import dev.picoswitch.management.KbmDestination
import dev.picoswitch.management.KbmMouseConfig
import dev.picoswitch.management.KbmPositions
import dev.picoswitch.management.KbmProfile
import dev.picoswitch.management.KbmSource
import dev.picoswitch.management.KbmSourceKind
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The seam between the two stores, measured in WIRE TRAFFIC.
 *
 * THE DEFECT THIS PINS. The companion's New called the staged adapter
 * transaction and its Duplicate called `kbm profile dup`, because the only
 * notion of "profile" it had was an adapter resident id. Creating a profile
 * therefore required a connection and erased flash, and Save changed what the
 * console might run.
 *
 * Asserting the domain split in the abstract is not enough — a future change
 * could reintroduce the coupling one call at a time and still satisfy a test
 * that only checked returned values. So these tests drive the REAL adapter
 * repository over a scripted transport, run local library operations against a
 * LIVE connection, and assert that the recorded command log is empty. The
 * evidence is the absence of traffic, which is the property that was lost.
 *
 * They then assert the exact command sequences the explicit adapter operations
 * do produce, so "explicit" is pinned in both directions.
 */
class KbmProfileWorkflowTest {

    private val layout = KbmProfile.Keyboard

    private class MemoryStore(
        private var document: KbmProfileLibrary = KbmProfileLibrary.EMPTY,
    ) : KbmProfileLibraryStore {
        override fun load(): KbmProfileLibrary = document
        override fun save(library: KbmProfileLibrary) { document = library }
    }

    private fun key(usage: Int) = KbmSource(KbmSourceKind.Key, usage)

    private fun override(usage: Int, destination: KbmDestination) =
        KbmBinding(key(usage), destination, custom = true)

    /** A connected adapter holding Halo at Profile 1 and nothing else. */
    private class Scripted : ManagementTransport {
        val commands = mutableListOf<String>()
        var assignedName = "Halo"
        var assignedFingerprint = 111L
        var runtimePosition = KbmPositions.DEFAULT
        var bootPosition = KbmPositions.DEFAULT
        var occupied = true

        override val connection =
            MutableStateFlow(ConnectionState(phase = ConnectionPhase.Connected))
        override suspend fun scanAndConnect() = Unit
        override suspend fun disconnect() = Unit

        /** Everything sent since the last call. */
        fun drain(): List<String> = commands.toList().also { commands.clear() }

        override suspend fun transact(command: String, timeoutMillis: Long): String {
            commands += command
            return when {
                command == "kbm status" -> """
                    {"mode":"keyboard","override":"auto","profile":"kb","keyboard":true,
                     "mouse":false,"nativeMouse":false,"keyboardConn":1,"mouseConn":0,
                     "group":1,"source":1}
                """.trimIndent()
                command == "kbm counters" -> """{"keyboardReports":0,"mouseReports":0}"""
                command == "kbm mouse" -> """
                    {"sensitivityX":512,"sensitivityY":512,"velocityWindowMs":120,
                     "invertX":false,"invertY":false,"antiDeadzone":0,
                     "sensitivityMin":16,"sensitivityMax":8192,
                     "velocityWindowMinMs":4,"velocityWindowMaxMs":250,
                     "antiDeadzoneMax":16000}
                """.trimIndent()
                command == "kbm profiles 0" -> if (occupied) {
                    """
                    {"cursor":0,"total":1,"max":6,"profiles":[
                      {"id":2,"layout":"kb","name":"$assignedName","revision":3,
                       "overrides":1,"fingerprint":$assignedFingerprint,"position":1}
                    ],"next":null}
                    """.trimIndent()
                } else {
                    """{"cursor":0,"total":0,"max":6,"profiles":[],"next":null}"""
                }
                command == "kbm active" -> """
                    {"active":[
                      {"layout":"kb","sourceId":1,"revision":0,"fingerprint":900,
                       "matchesSaved":true,"bootPosition":$bootPosition,
                       "runtimePosition":$runtimePosition},
                      {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,
                       "matchesSaved":true,"bootPosition":0,"runtimePosition":0}
                    ]}
                """.trimIndent()
                command == "kbm switches" -> """{"positions":3,"switches":[]}"""
                command.startsWith("kbm map ") ->
                    """{"profile":"kb","cursor":0,"total":0,"bindings":[],"next":null}"""
                command == "kbm draft commit" -> """{"ok":true,"id":2,"revision":4}"""
                else -> """{"ok":true}"""
            }
        }
    }

    private suspend fun connected(): Pair<AdapterRepository, Scripted> {
        val transport = Scripted()
        val adapter = AdapterRepository(transport)
        adapter.refreshKbm()
        transport.drain()
        return adapter to transport
    }

    // ------------------------------------------- local operations are silent

    @Test fun `creating a profile while connected sends nothing`() = runTest {
        // The headline regression. A live session is present and deliberately
        // unused: New must not assign, must not save to flash, and must not
        // consume one of the adapter's three positions.
        val (_, transport) = connected()
        val library = KbmLibraryRepository(MemoryStore())

        library.create(layout, "Halo")

        assertEquals(emptyList<String>(), transport.drain())
    }

    @Test fun `a whole editing session while connected sends nothing`() = runTest {
        val (_, transport) = connected()
        val library = KbmLibraryRepository(MemoryStore())

        val created = library.create(layout, "Halo")
        library.save(created.id, "Halo", listOf(override(0x04, KbmDestination.Zr)), KbmMouseConfig())
        val copy = library.duplicate(created.id, "Halo copy")!!
        library.rename(copy.id, "Zelda")
        library.delete(copy.id)

        assertEquals(
            "no local library operation may reach the adapter",
            emptyList<String>(),
            transport.drain(),
        )
    }

    @Test fun `saving does not touch the resident copy`() = runTest {
        // Save changes the LIBRARY. An older copy may be resident and may be
        // running; the screen reports the divergence and only an explicit Update
        // resolves it. Conflating the two made every keystroke a flash erase.
        val (adapter, transport) = connected()
        val library = KbmLibraryRepository(MemoryStore())

        val created = library.create(layout, "Halo")
        val before = adapter.kbm.value.profiles.at(layout, 1)!!.fingerprint
        library.save(created.id, "Halo", listOf(override(0x04, KbmDestination.Zr)), KbmMouseConfig())

        assertEquals(emptyList<String>(), transport.drain())
        assertEquals(before, adapter.kbm.value.profiles.at(layout, 1)!!.fingerprint)
    }

    // ------------------------------------ adapter operations are explicit

    @Test fun `assign is one staged transaction carrying only the overrides`() = runTest {
        val (adapter, transport) = connected()
        val library = KbmLibraryRepository(MemoryStore())
        val profile = library.create(
            layout, "Halo", listOf(override(0x04, KbmDestination.Zr)),
        )

        adapter.assignKbmPosition(layout, 2, profile)

        val sent = transport.drain()
        // `pos:N` addresses a POSITION, not a storage slot: the user picks
        // "Profile 2" and the adapter decides which record backs it.
        assertEquals("kbm draft begin kb pos:2 0 Halo", sent.first())
        assertTrue(sent.contains("kbm draft bind key:04 zr"))
        assertTrue(sent.contains("kbm draft commit"))
        // Only what the user chose. Sending the effective mapping would upload
        // the canonical table as if every default had been picked deliberately.
        assertEquals(1, sent.count { it.startsWith("kbm draft bind ") })
        // Re-read rather than trusting the acknowledgement.
        assertTrue(sent.contains("kbm profiles 0"))
    }

    @Test fun `assign carries the occupant's revision so a concurrent change is refused`() =
        runTest {
            // A stale base revision is how the adapter tells two companions apart;
            // sending 0 into an occupied position would silently overwrite work
            // the other one had just stored.
            val (adapter, transport) = connected()
            val library = KbmLibraryRepository(MemoryStore())
            val profile = library.create(layout, "Halo")

            adapter.assignKbmPosition(layout, 1, profile)

            assertEquals("kbm draft begin kb pos:1 3 Halo", transport.drain().first())
        }

    @Test fun `assign does not activate`() = runTest {
        // Updating a stored copy must not change gameplay mid-session, so the
        // realized snapshot is left alone until the user activates.
        val (adapter, transport) = connected()
        val library = KbmLibraryRepository(MemoryStore())
        adapter.assignKbmPosition(layout, 1, library.create(layout, "Halo"))

        assertTrue(transport.drain().none { it.startsWith("kbm apply") })
    }

    @Test fun `activate is runtime only and costs no flash write`() = runTest {
        val (adapter, transport) = connected()
        adapter.activateKbmPosition(layout, 1)

        val sent = transport.drain()
        assertTrue(sent.any { it.startsWith("kbm apply ") })
        assertTrue("activation must not write flash", sent.none { it == "save" })
        assertTrue(sent.none { it.startsWith("kbm boot") })
    }

    @Test fun `activating an empty position asks the adapter nothing`() = runTest {
        // There is nothing to activate and the adapter would only answer with a
        // refusal the user cannot act on.
        val (adapter, transport) = connected()
        adapter.activateKbmPosition(layout, 3)
        assertEquals(emptyList<String>(), transport.drain())
    }

    @Test fun `the startup choice is a separate command from activation`() = runTest {
        // "Use this now" and "use this after a reboot" are different intentions,
        // and only the second is worth a flash write.
        val (adapter, transport) = connected()
        adapter.setKbmBootPosition(layout, 1)

        val sent = transport.drain()
        assertTrue(sent.contains("kbm boot kb 1"))
        assertTrue(sent.none { it.startsWith("kbm apply") })
    }

    @Test fun `removing a resident re-reads the realized mapping`() = runTest {
        // The layout may have fallen back to Default, so what the console is
        // running is read rather than assumed: a screen still showing the removed
        // mapping would be describing something that is no longer in force.
        val (adapter, transport) = connected()
        adapter.removeKbmPosition(layout, 1)

        val sent = transport.drain()
        assertTrue(sent.contains("kbm remove kb 1"))
        assertTrue(sent.any { it.startsWith("kbm map kb") })
        assertTrue(sent.contains("kbm status"))
    }

    @Test fun `removing a resident does not touch the local library`() = runTest {
        val (adapter, transport) = connected()
        val store = MemoryStore()
        val library = KbmLibraryRepository(store)
        val profile = library.create(layout, "Halo")
        transport.drain()

        adapter.removeKbmPosition(layout, 1)

        // The repository holding the library is not reachable from the adapter
        // repository at all; this asserts the user-visible consequence.
        assertEquals(1, KbmLibraryRepository(store).value.profiles.size)
        assertEquals(profile.id, store.load().profiles.single().id)
    }

    @Test fun `binding a switch key re-reads the table`() = runTest {
        // Assigning a key to an action that already has one MOVES it, so a
        // caller's model of the table is wrong the moment it guesses.
        val (adapter, transport) = connected()
        adapter.bindKbmSwitch(key(0x3A), 2)

        val sent = transport.drain()
        assertTrue(sent.contains("kbm switch key:3A 2"))
        assertTrue(sent.contains("kbm switches"))
    }

    @Test fun `clearing a switch key is distinct from binding Default to it`() = runTest {
        // "No key selects this" and "this key selects the built-in Default" are
        // different configurations, and Default IS a bindable action.
        //
        // Default goes on the wire as the WORD, not as 0: `kbm_position_arg` in
        // src/config.c accepts "default" and rejects any number below 1, so a
        // client that sent the position's numeric value would have its command
        // refused for the one action a user is most likely to want a key for.
        val (adapter, transport) = connected()
        adapter.bindKbmSwitch(key(0x3A), null)
        val cleared = transport.drain()

        adapter.bindKbmSwitch(key(0x3A), KbmPositions.DEFAULT)
        val toDefault = transport.drain()

        assertTrue(cleared.contains("kbm switch key:3A none"))
        assertTrue(toDefault.contains("kbm switch key:3A default"))
        assertTrue("0 is refused by the firmware", toDefault.none { it.endsWith(" 0") })
    }
}
