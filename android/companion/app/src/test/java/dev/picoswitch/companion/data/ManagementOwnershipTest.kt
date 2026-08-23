package dev.picoswitch.companion.data

import dev.picoswitch.companion.model.ConnectionPhase
import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.companion.protocol.ManagementTransport
import dev.picoswitch.companion.transport.GattCallbackAuthority
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

/**
 * One management relationship per process.
 *
 * Regression cover for the 2026-08-23 hardware finding: `dumpsys activity
 * activities` showed five stacked `MainActivity` records, each with its own
 * ViewModel, transport, GATT connection and 5-second background poller. Pressing
 * Disconnect retired the foreground instance correctly while a different
 * instance kept the real session alive, so the app reported "Not connected"
 * while the adapter still reported `cble.client: true` and kept answering
 * commands for hours.
 *
 * These cases pin the two properties that make that impossible: exactly one
 * transport is ever constructed, and a retired generation can neither send
 * commands nor become authoritative again.
 */
class ManagementOwnershipTest {

    private class RecordingTransport : ManagementTransport {
        val commands = mutableListOf<String>()
        var disconnects = 0
        var closes = 0
        var closed = false
        private val _connection = MutableStateFlow(ConnectionState(phase = ConnectionPhase.Connected))
        override val connection: StateFlow<ConnectionState> = _connection

        override suspend fun scanAndConnect() {
            closed = false
            _connection.value = ConnectionState(phase = ConnectionPhase.Connected)
        }

        override suspend fun disconnect() {
            disconnects++
            closed = true
            _connection.value = ConnectionState()
        }

        override fun close() {
            closes++
            closed = true
            _connection.value = ConnectionState()
        }

        override suspend fun transact(command: String, timeoutMillis: Long): String {
            // A retired generation must not be able to drive the adapter. The
            // live transport enforces this by requiring a current owner; the
            // fake enforces the same contract so the test can observe it.
            check(!closed) { "retired management generation issued: $command" }
            commands += command
            return "{}"
        }
    }

    @Before fun reset() = ManagementOwner.resetForTest()
    @After fun cleanup() = ManagementOwner.resetForTest()

    @Test fun `a second screen entry reuses the process transport instead of building another`() {
        var built = 0
        val first = ManagementOwner.get { built++; AdapterRepository(RecordingTransport()) }
        val second = ManagementOwner.get { built++; AdapterRepository(RecordingTransport()) }
        val third = ManagementOwner.get { built++; AdapterRepository(RecordingTransport()) }

        // The exact defect: every additional entry used to construct another
        // BleGattManagementTransport, and each one opened a real GATT link.
        assertEquals("only one management transport may ever be constructed", 1, built)
        assertSame(first, second)
        assertSame(first, third)
        assertTrue(ManagementOwner.hasRepository())
    }

    @Test fun `disconnect retires the live session rather than a detached copy`() = runTest {
        val transport = RecordingTransport()
        val repository = ManagementOwner.get { AdapterRepository(transport) }

        repository.disconnect()

        // Reaching the live transport is the whole point: the stacked-Activity
        // build called disconnect on a transport that held no connection while
        // another instance kept the real one open.
        assertEquals(1, transport.disconnects)
        assertTrue(transport.closed)
        assertFalse(repository.connection.value.connected)
    }

    @Test fun `a retired generation cannot drive the adapter afterwards`() = runTest {
        val transport = RecordingTransport()
        val repository = ManagementOwner.get { AdapterRepository(transport) }
        repository.disconnect()

        val afterRetirement = runCatching { transport.transact("input", 1_000L) }

        assertTrue(
            "a retired generation must refuse commands",
            afterRetirement.isFailure,
        )
        assertTrue(transport.commands.isEmpty())
    }

    @Test fun `a retired generation cannot become authoritative again`() {
        // Callback authority is generation-scoped and closure-scoped. Both
        // conditions matter: a stale generation loses to the current one, and a
        // closed owner loses even when it is still the current generation.
        assertTrue(GattCallbackAuthority.isAuthoritative(4, 4, callbackOwnerClosed = false))
        assertFalse(GattCallbackAuthority.isAuthoritative(5, 4, callbackOwnerClosed = false))
        assertFalse(GattCallbackAuthority.isAuthoritative(4, 4, callbackOwnerClosed = true))
        assertFalse(GattCallbackAuthority.isAuthoritative(null, 4, callbackOwnerClosed = false))
    }

    @Test fun `releasing a screen does not destroy the process transport`() = runTest {
        val transport = RecordingTransport()
        ManagementOwner.get { AdapterRepository(transport) }

        ManagementOwner.releaseSession()

        // close() cancels the transport's lifecycle scope permanently. A
        // process-scoped owner that closed on every departing screen would be
        // unusable for the rest of the process, so release must disconnect only.
        assertEquals("the process transport must not be closed by a departing screen", 0, transport.closes)
        assertTrue(ManagementOwner.hasRepository())
    }
}
