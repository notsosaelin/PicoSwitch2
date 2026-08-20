package dev.picoswitch.management

import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * Single-flight ownership for carriers whose replies have no request id.
 *
 * Cancellation while waiting for ownership is normal. Once [exchange] starts,
 * however, the operation finishes in a non-cancellable context so its reply is
 * consumed (or the backend invalidates the session) before another caller can
 * transmit. This prevents a late reply from becoming the next request's reply.
 */
class SerializedManagementSession {
    private val mutex = Mutex()

    suspend fun <T> exchange(operation: suspend () -> T): T = mutex.withLock {
        withContext(NonCancellable) { operation() }
    }

    /** Serialize lifecycle mutation, such as disconnect, against an exchange. */
    suspend fun <T> mutate(operation: suspend () -> T): T = mutex.withLock { operation() }
}
