package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.model.CapabilityState
import dev.picoswitch.companion.model.KbmDestination
import dev.picoswitch.companion.model.KbmMode
import dev.picoswitch.companion.model.KbmMouseField
import dev.picoswitch.companion.model.KbmProfileIds
import dev.picoswitch.companion.model.KbmProfile
import dev.picoswitch.companion.model.KbmSource
import dev.picoswitch.companion.model.KbmSourceKind
import dev.picoswitch.companion.protocol.ManagementException
import dev.picoswitch.companion.protocol.ManagementTransport
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Keyboard/Mouse repository behaviour, driven through a scripted transport.
 *
 * The interesting properties are not "the command was sent" but the ones a
 * screen cannot check for itself: that a paginated mapping is assembled
 * completely and refuses to loop, that an unsaved marker means what the UI
 * claims it means, and that a disconnect cannot carry one session's state into
 * the next.
 */
class KbmRepositoryTest {

    @Test
    fun `refresh reads status and mouse limits together`() = runTest {
        val transport = KbmTransport()
        val repository = AdapterRepository(transport)
        repository.refreshKbm()

        // Status, mouse limits, and the profile library are one read: a
        // half-loaded page would show a mapping without being able to say
        // whether it is the one in use.
        assertEquals(
            listOf("kbm status", "kbm mouse", "kbm profiles", "kbm active"),
            transport.commands,
        )
        val state = repository.kbm.value
        assertEquals(CapabilityState.Available, state.available)
        assertEquals(KbmMode.KeyboardMouse, state.status.mode)
        assertEquals(512, state.mouse.sensitivityX)
        assertEquals(8192, state.mouse.sensitivityMax)
        assertTrue(state.profiles.supported)
        assertEquals("Work", state.profiles.forLayout(KbmProfile.Keyboard)[1].name)
        // The Keyboard layout is running its built-in Default, not Work.
        assertEquals(
            KbmProfileIds.DEFAULT,
            state.profiles.activeFor(KbmProfile.Keyboard)?.sourceId,
        )
    }

    @Test
    fun `firmware without the kbm surface is reported unsupported rather than empty`() = runTest {
        // An empty configuration would look exactly like a connected keyboard
        // with nothing bound, which is the wrong thing to show a user.
        val repository = AdapterRepository(KbmTransport(supported = false))
        repository.refreshKbm()
        assertEquals(CapabilityState.Unsupported, repository.kbm.value.available)
    }

    @Test
    fun `a paginated mapping is assembled from every page`() = runTest {
        val transport = KbmTransport(mapPages = 3)
        val repository = AdapterRepository(transport)
        val mapping = repository.loadKbmMapping(KbmProfile.Keyboard)

        assertEquals(
            listOf("kbm map kb 0", "kbm map kb 1", "kbm map kb 2"),
            transport.commands,
        )
        assertEquals(3, mapping.bindings.size)
        assertTrue(mapping.loaded)
        assertEquals(mapping, repository.kbm.value.mapping(KbmProfile.Keyboard))
    }

    @Test
    fun `a mapping page for the wrong profile is refused`() = runTest {
        val repository = AdapterRepository(KbmTransport(mapProfileOverride = "kbm"))
        assertThrows(ManagementException::class.java) {
            kotlinx.coroutines.runBlocking { repository.loadKbmMapping(KbmProfile.Keyboard) }
        }
    }

    @Test
    fun `a mapping whose total changes mid-pagination is refused`() = runTest {
        val repository = AdapterRepository(KbmTransport(mapPages = 2, shiftTotalOnPage = 1))
        assertThrows(ManagementException::class.java) {
            kotlinx.coroutines.runBlocking { repository.loadKbmMapping(KbmProfile.Keyboard) }
        }
    }

    @Test
    fun `an adapter that never stops paginating is bounded`() = runTest {
        val repository = AdapterRepository(KbmTransport(mapPages = Int.MAX_VALUE))
        val error = runCatching {
            kotlinx.coroutines.runBlocking { repository.loadKbmMapping(KbmProfile.Keyboard) }
        }.exceptionOrNull()
        assertTrue(error is ManagementException)
    }

    @Test
    fun `binding sends the exact wire form and re-reads the profile`() = runTest {
        val transport = KbmTransport()
        val repository = AdapterRepository(transport)
        repository.bindKbm(
            KbmProfile.KeyboardMouse,
            KbmSource(KbmSourceKind.MouseButton, 4),
            KbmDestination.Capture,
        )
        assertEquals("kbm bind kbm mouse:4 capture", transport.commands.first())
        assertTrue(transport.commands.contains("kbm map kbm 0"))
        assertTrue(repository.kbm.value.dirty)
    }

    @Test
    fun `a null destination restores the canonical binding rather than unassigning it`() = runTest {
        // `none` and `default` are different commands: one says "do nothing",
        // the other removes the override. Collapsing them would make Restore
        // Default silently unbind the key.
        val transport = KbmTransport()
        AdapterRepository(transport).bindKbm(
            KbmProfile.Keyboard,
            KbmSource(KbmSourceKind.Key, 0x2C),
            null,
        )
        assertEquals("kbm bind kb key:2C default", transport.commands.first())
    }

    @Test
    fun `unassigning sends none`() = runTest {
        val transport = KbmTransport()
        AdapterRepository(transport).bindKbm(
            KbmProfile.Keyboard,
            KbmSource(KbmSourceKind.Key, 0x2C),
            KbmDestination.None,
        )
        assertEquals("kbm bind kb key:2C none", transport.commands.first())
    }

    @Test
    fun `a mouse change adopts the adapter's own reply as the new truth`() = runTest {
        val transport = KbmTransport()
        val repository = AdapterRepository(transport)
        repository.setKbmMouse(KbmMouseField.AntiDeadzone, 12)
        assertEquals("kbm mouse antideadzone 12", transport.commands.single())
        // The scripted adapter echoes the applied value; the client must take
        // it rather than optimistically storing what it asked for.
        assertEquals(12, repository.kbm.value.mouse.antiDeadzone)
        assertTrue(repository.kbm.value.dirty)
    }

    @Test
    fun `mode changes re-read the effective mode instead of assuming the override`() = runTest {
        // Under Automatic the live mode is inferred from the admitted roles, so
        // writing the override tells you nothing about what is now in force.
        val transport = KbmTransport()
        val repository = AdapterRepository(transport)
        repository.setKbmMode(KbmMode.Automatic)
        assertEquals("kbm mode auto", transport.commands.first())
        assertTrue(transport.commands.contains("kbm status"))
    }

    @Test
    fun `saving clears the unsaved marker only on acceptance`() = runTest {
        val transport = KbmTransport()
        val repository = AdapterRepository(transport)
        repository.setKbmMouse(KbmMouseField.Sensitivity, 640)
        assertTrue(repository.kbm.value.dirty)
        repository.saveConfiguration()
        assertFalse(repository.kbm.value.dirty)
        assertFalse(repository.kbm.value.saving)
        assertTrue(transport.commands.contains("save"))
    }

    @Test
    fun `a failed save leaves the changes marked unsaved`() = runTest {
        val transport = KbmTransport(failSave = true)
        val repository = AdapterRepository(transport)
        repository.setKbmMouse(KbmMouseField.Sensitivity, 640)
        runCatching { repository.saveConfiguration() }
        assertTrue("a rejected save must not claim the settings are stored", repository.kbm.value.dirty)
        assertFalse(repository.kbm.value.saving)
    }

    @Test
    fun `disconnecting drops one session's keyboard state entirely`() = runTest {
        // Both halves matter: a stale mapping would belong to another adapter,
        // and a stale unsaved marker would offer to save changes that may never
        // have landed.
        val transport = KbmTransport()
        val repository = AdapterRepository(transport)
        repository.refreshKbm()
        repository.loadKbmMapping(KbmProfile.Keyboard)
        repository.setKbmMouse(KbmMouseField.Sensitivity, 640)

        repository.clearDisconnectedSnapshot()

        val state = repository.kbm.value
        assertFalse(state.dirty)
        assertTrue(state.mappings.isEmpty())
        assertEquals(CapabilityState.Unknown, state.available)
    }
}

/**
 * A scripted management transport that answers the KB/M command surface the way
 * `src/config.c` does, including its pagination shape.
 */
private class KbmTransport(
    private val supported: Boolean = true,
    private val mapPages: Int = 1,
    private val mapProfileOverride: String? = null,
    private val shiftTotalOnPage: Int? = null,
    private val failSave: Boolean = false,
) : ManagementTransport {
    val commands = mutableListOf<String>()
    private var antiDeadzone = 0
    private var sensitivity = 512

    override val connection = MutableStateFlow(ConnectionState())
    override suspend fun scanAndConnect() = Unit
    override suspend fun disconnect() = Unit

    override suspend fun transact(command: String, timeoutMillis: Long): String {
        commands += command
        if (!supported && command.startsWith("kbm")) {
            return """{"error":"unknown command"}"""
        }
        return when {
            command == "kbm status" -> status()
            command == "kbm mouse" -> mouse()
            command.startsWith("kbm mouse ") -> {
                val (field, value) = command.removePrefix("kbm mouse ").split(" ")
                when (field) {
                    "antideadzone" -> antiDeadzone = value.toInt()
                    "sensitivity", "sensitivityx" -> sensitivity = value.toInt()
                }
                mouse()
            }
            command.startsWith("kbm map ") -> mapPage(command)
            // Only CUSTOM profiles are stored; Default is a template the client
            // synthesises, which is what keeps all six adapter slots for the user.
            command == "kbm profiles" -> """
                {"profiles":[
                  {"id":2,"layout":"kb","name":"Work","revision":3,"overrides":3,"fingerprint":111}
                ],"max":6,"more":false}
            """.trimIndent()
            // What each layout is REALLY running.
            command == "kbm active" -> """
                {"active":[
                  {"layout":"kb","sourceId":1,"revision":0,"fingerprint":900,"matchesSaved":true},
                  {"layout":"kbm","sourceId":1,"revision":0,"fingerprint":901,"matchesSaved":true}
                ]}
            """.trimIndent()
            command.startsWith("kbm bind ") -> """{"ok":true}"""
            command.startsWith("kbm mode ") -> """{"ok":true,"mode":"auto"}"""
            command.startsWith("kbm reset") -> """{"ok":true,"reset":"all"}"""
            command == "save" -> if (failSave) """{"error":"save timeout"}""" else """{"ok":true,"queued":true}"""
            else -> """{"ok":true}"""
        }
    }

    private fun status() = """
        {"mode":"kbmouse","override":"auto","profile":"kb","keyboard":true,"mouse":true,
         "nativeMouse":false,"keyboardConn":1,"mouseConn":2,"group":1,"source":1,
         "keyboardReports":0,"mouseReports":0,"rejectedMode":0,"rejectedDuplicate":0,
         "rejectedNotOwner":0,"rollover":0,"roleLosses":0,"mapGeneration":1,
         "neutralizations":0,"publishes":0,"recenters":0}
    """.trimIndent().replace("\n", "")

    private fun mouse() = """
        {"sensitivityX":$sensitivity,"sensitivityY":$sensitivity,"recenterMs":120,
         "invertX":false,"invertY":false,"antiDeadzone":$antiDeadzone,
         "sensitivityMin":16,"sensitivityMax":8192,"recenterMinMs":10,
         "recenterMaxMs":2000,"antiDeadzoneMax":50}
    """.trimIndent().replace("\n", "")

    private fun mapPage(command: String): String {
        val parts = command.removePrefix("kbm map ").split(" ")
        val profile = mapProfileOverride ?: parts[0]
        val page = parts.getOrNull(1)?.toIntOrNull() ?: 0
        val total = if (shiftTotalOnPage == page) mapPages + 5 else mapPages
        val more = page < mapPages - 1
        return """{"profile":"$profile","page":$page,"pageSize":1,"total":$total,""" +
            """"bindings":[{"src":"key:%02X","dst":"a","custom":false}],"more":$more}"""
                .format(0x04 + page)
    }
}
