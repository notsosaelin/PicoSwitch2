package dev.picoswitch.management

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.ExperimentalCoroutinesApi
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class SerializedManagementSessionTest {
    @Test fun `concurrent exchanges are single flight`() = runTest {
        val session = SerializedManagementSession()
        val release = CompletableDeferred<Unit>()
        val events = mutableListOf<String>()
        val first = async { session.exchange { events += "first-start"; release.await(); events += "first-end" } }
        runCurrent()
        val second = async { session.exchange { events += "second" } }
        runCurrent()
        assertEquals(listOf("first-start"), events)
        release.complete(Unit)
        first.await()
        second.await()
        assertEquals(listOf("first-start", "first-end", "second"), events)
    }

    @Test fun `cancellation after ownership does not abandon a reply`() = runTest {
        val session = SerializedManagementSession()
        val transmitted = CompletableDeferred<Unit>()
        val reply = CompletableDeferred<Unit>()
        var consumed = false
        val first = launch {
            session.exchange {
                transmitted.complete(Unit)
                reply.await()
                consumed = true
            }
        }
        transmitted.await()
        first.cancel()
        val secondStarted = CompletableDeferred<Unit>()
        val second = launch { session.exchange { secondStarted.complete(Unit) } }
        runCurrent()
        assertFalse(secondStarted.isCompleted)
        reply.complete(Unit)
        first.join()
        second.join()
        assertTrue(consumed)
        assertTrue(secondStarted.isCompleted)
    }

    @Test fun `caller cancelled while queued never transmits`() = runTest {
        val session = SerializedManagementSession()
        val release = CompletableDeferred<Unit>()
        val first = launch { session.exchange { release.await() } }
        runCurrent()
        var transmitted = false
        val queued = launch { session.exchange { transmitted = true } }
        runCurrent()
        queued.cancelAndJoin()
        release.complete(Unit)
        first.join()
        assertFalse(transmitted)
    }

    @Test fun `session mutation waits for in-flight exchange`() = runTest {
        val session = SerializedManagementSession()
        val release = CompletableDeferred<Unit>()
        val order = mutableListOf<String>()
        val exchange = launch { session.exchange { order += "exchange"; release.await() } }
        runCurrent()
        val mutation = launch { session.mutate { order += "disconnect" } }
        runCurrent()
        assertEquals(listOf("exchange"), order)
        release.complete(Unit)
        exchange.join()
        mutation.join()
        assertEquals(listOf("exchange", "disconnect"), order)
    }

    @Test fun `cancellation after mutation ownership cannot expose partial cleanup`() = runTest {
        val session = SerializedManagementSession()
        val started = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        var cleanupCompleted = false
        val mutation = launch {
            session.mutate {
                started.complete(Unit)
                release.await()
                cleanupCompleted = true
            }
        }
        started.await()
        mutation.cancel()

        val exchangeStarted = CompletableDeferred<Unit>()
        val exchange = launch { session.exchange { exchangeStarted.complete(Unit) } }
        runCurrent()
        assertFalse(exchangeStarted.isCompleted)

        release.complete(Unit)
        mutation.join()
        exchange.join()
        assertTrue(cleanupCompleted)
        assertTrue(exchangeStarted.isCompleted)
    }
}
