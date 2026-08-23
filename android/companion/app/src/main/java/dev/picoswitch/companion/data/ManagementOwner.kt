package dev.picoswitch.companion.data

import android.content.Context
import dev.picoswitch.companion.diagnostics.DiagnosticLog
import dev.picoswitch.companion.transport.BleGattManagementTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * The one management relationship this process owns.
 *
 * WHY THIS IS APPLICATION-SCOPED AND NOT VIEWMODEL-SCOPED
 *
 * The adapter relationship is a device-level resource: one adapter, one LE
 * management link, one owner. It was previously constructed inside
 * [dev.picoswitch.companion.ui.CompanionViewModel], which scopes it to an
 * Activity's ViewModelStore, so every additional Activity instance created a
 * second [BleGattManagementTransport] with its own GATT connection and its own
 * 5-second background poller.
 *
 * Confirmed on hardware 2026-08-23. `dumpsys activity activities` showed **five**
 * live `MainActivity` records stacked in one task, and logcat showed two of their
 * transports polling the adapter concurrently -- `gatt=5 seq=5401` (ready for
 * 7.5 hours) alongside `gatt=1 seq=18` (ready for 42 seconds). Generation and
 * sequence counters are per-transport, so a "1" can only be a fresh instance.
 *
 * The user-visible symptom was that pressing Disconnect appeared to do nothing:
 * the UI correctly reported ITS OWN instance as disconnected while a different
 * instance kept the real session alive, so the adapter still reported
 * `cble.client: true` and kept answering commands. Both clients multiplex over a
 * single LE ACL, so the adapter could not distinguish them either -- at teardown
 * `gatt=1` and `gatt=5` both died with `status=19` within 3 ms of each other.
 *
 * Two things now prevent that. `MainActivity` is `launchMode="singleTask"`, so
 * entering the app cannot stack instances; and this holder makes single
 * ownership structural rather than a consequence of navigation, so no future
 * entry path can reintroduce a second live management session.
 *
 * Deliberately NOT a second ownership mechanism layered over the transport's
 * own generation handling: the transport still owns retirement of its GATT
 * generations exactly as before. This only ensures there is one transport to
 * own them.
 */
object ManagementOwner {
    @Volatile private var repository: AdapterRepository? = null
    @Volatile private var diagnosticLog: DiagnosticLog? = null

    /**
     * Coroutine scope for session teardown requested by a departing screen.
     * Application-lived on purpose: the request outlives the ViewModel that
     * makes it, and the work must not be cancelled halfway through closing a
     * real BLE connection.
     */
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /**
     * The process-wide management repository, created on first use.
     *
     * [diagnostics] is adopted from the first caller so diagnostic output keeps
     * flowing to the log the UI is observing. Later callers get the existing
     * repository; passing a different log does not build a second transport.
     */
    fun get(context: Context, diagnostics: DiagnosticLog? = null): AdapterRepository =
        get(diagnostics) {
            AdapterRepository(
                BleGattManagementTransport(context.applicationContext, diagnostics),
            )
        }

    /**
     * Single-ownership core, with construction injected.
     *
     * [create] runs at most once for the life of the process. That "at most
     * once" is the entire invariant, so it is expressed here where a test can
     * observe it without an Android BLE stack.
     */
    @Synchronized
    fun get(diagnostics: DiagnosticLog? = null, create: () -> AdapterRepository): AdapterRepository {
        repository?.let { return it }
        diagnosticLog = diagnostics
        val created = create()
        repository = created
        return created
    }

    /** The diagnostics sink the live transport reports through, if one exists. */
    fun diagnostics(): DiagnosticLog? = diagnosticLog

    /** True once a transport exists. Used by tests to assert single creation. */
    fun hasRepository(): Boolean = repository != null

    /**
     * Retire the live management session because the screen that was using it
     * went away.
     *
     * This disconnects; it deliberately does NOT close the transport. Closing
     * cancels the transport's internal lifecycle scope permanently, which would
     * make the singleton unusable for the rest of the process -- the exact
     * failure mode a per-ViewModel transport never had to consider because it
     * was thrown away with its owner.
     */
    fun releaseSession() {
        val active = repository ?: return
        scope.launch { runCatching { active.disconnect() } }
    }

    /** Tests only: drop the singleton so each case starts from a clean process. */
    @Synchronized
    fun resetForTest() {
        repository = null
        diagnosticLog = null
    }
}
