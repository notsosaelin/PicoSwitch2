package dev.picoswitch.companion.bridge

import android.content.Context
import dev.picoswitch.bridge.core.BridgeCounters
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.protocol.ControllerLinkDataPlane
import dev.picoswitch.bridge.protocol.ControllerReportEncoder
import dev.picoswitch.bridge.session.BridgeHost
import dev.picoswitch.bridge.session.BridgeLinkPhase
import dev.picoswitch.bridge.session.BridgeTransport
import dev.picoswitch.companion.model.ConnectionState
import dev.picoswitch.management.ControllerLinkState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

/** The saved adapter as the bridge sees it: an address and a name. */
class AdapterBridgeHost(override val address: String, override val name: String?) : BridgeHost

/**
 * The management link's Controller Link data plane, as the bridge needs it.
 *
 * Supplied by the process-wide management owner: Controller Link opens
 * nothing of its own. There is one Bluetooth connection to the adapter and the
 * management owner holds it; gameplay, companion audio and management all ride
 * it (docs/experiments/android-two-link-coexistence-2026-11-22.md).
 */
interface ControllerLinkPlane {
    /** The live management link exposes the data plane right now. */
    val available: Boolean
    val connection: StateFlow<ConnectionState>
    suspend fun start(): ControllerLinkState
    suspend fun stop(): ControllerLinkState
    suspend fun open(onOutput: (ByteArray) -> Unit): Boolean
    suspend fun close()
    suspend fun write(frame: ByteArray): Boolean
}

/**
 * Controller Link over Path C: the canonical v2 report, framed, written to the
 * Controller Link characteristic of the management link.
 *
 * The only carrier. It replaced the Classic HID connection Android used to
 * open beside management: that second link had to be paged into existence
 * through the adapter's inquiry cycle (the slow, often failing first connect)
 * and then shared one radio with the LE link, which starved and timed out
 * under it on a Galaxy S25 Ultra while the Classic link stayed up (2026-11-22).
 *
 * Writing is single-flight and the newest frame wins: a frame that could not
 * go out is superseded, never queued. With no new state the newest frame is
 * re-sent every [KEEPALIVE_MS], because the adapter neutralizes a source that
 * has been quiet for 300 ms (NS2_COMPANION_LINK_STALE_MS) and the session only
 * sends on change.
 */
class GattControllerLinkTransport(
    context: Context,
    private val plane: () -> ControllerLinkPlane?,
    private val diagnostics: BridgeDiagnostics,
    private val counters: BridgeCounters,
) : BridgeTransport {
    private val appContext = context.applicationContext
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var listener: BridgeTransport.Listener? = null
    @Volatile private var up = false
    @Volatile private var hostName: String? = null
    private var session: Job? = null
    private var mailbox = Channel<ByteArray>(Channel.CONFLATED)
    private var sequence = 0

    /** Whether this carrier can serve a Controller Link right now. */
    fun available(): Boolean = plane()?.available == true

    override fun attach(listener: BridgeTransport.Listener) {
        this.listener = listener
    }

    override fun attachedListener(): BridgeTransport.Listener? = listener

    /** This carrier has exactly one host, the management peer, which the caller names. */
    override fun knownHosts(): List<BridgeHost> = emptyList()

    override fun start(preferredHost: BridgeHost?) = open(preferredHost)

    override fun connect(host: BridgeHost) = open(host)

    @Synchronized
    private fun open(host: BridgeHost?) {
        val target = plane() ?: return fail("No adapter connection to carry Controller Link")
        if (session?.isActive == true) return
        hostName = host?.name
        mailbox = Channel(Channel.CONFLATED)
        session = scope.launch { run(target) }
    }

    private suspend fun run(target: ControllerLinkPlane) {
        val listener = listener ?: return
        listener.onPhase(BridgeLinkPhase.Preparing, hostName, "Starting Controller Link on the adapter connection")
        if (!target.available) return fail("The adapter connection does not offer Controller Link")
        listener.onPhase(BridgeLinkPhase.Connecting, hostName, "Starting Controller Link")
        val state = runCatching { target.start() }.getOrElse {
            return fail("The adapter refused Controller Link: ${it.message}")
        }
        diagnostics.event(
            "transport", "clink.start",
            "active=${state.active} mtu=${state.attMtu} min=${state.minimumAttMtu} mtuOk=${state.mtuOk} " +
                "version=${state.version} generation=${state.generation}",
        )
        if (!state.active) return fail("The adapter did not start Controller Link")
        if (!state.mtuOk || !ControllerLinkDataPlane.mtuSufficient(state.attMtu)) {
            return fail("The adapter connection cannot carry Controller Link (ATT MTU ${state.attMtu})")
        }
        if (state.version != ControllerLinkDataPlane.VERSION) {
            return fail("Controller Link version mismatch (adapter ${state.version})")
        }
        val opened = target.open { frame -> onOutput(frame) }
        if (!opened) {
            runCatching { target.stop() }
            return fail("Could not subscribe to Controller Link feedback")
        }
        sequence = 0
        up = true
        BridgeForegroundService.start(appContext, diagnostics)
        diagnostics.event("transport", "clink.up", "carrier=management-link")
        listener.onLinkUp(hostName)

        // Management loss is carrier loss: the adapter releases the source with it.
        val watcher = scope.launch {
            target.connection.first { !it.connected }
            if (up) linkDown(target, "The adapter connection was lost", sendStop = false)
        }
        try {
            writeLoop(target)
        } finally {
            watcher.cancel()
        }
    }

    private suspend fun writeLoop(target: ControllerLinkPlane) {
        var latest: ByteArray? = null
        var consecutiveFailures = 0
        while (up) {
            val next = withTimeoutOrNull(KEEPALIVE_MS) { mailbox.receive() }
            val payload = next ?: latest ?: continue
            latest = payload
            val frame = ControllerLinkDataPlane.encodeInput(payload, sequence)
            sequence = (sequence + 1) and 0xFFFF
            if (target.write(frame)) {
                consecutiveFailures = 0
            } else if (++consecutiveFailures >= FAILURES_BEFORE_CHECK && !target.available) {
                linkDown(target, "The adapter connection was lost", sendStop = false)
            }
        }
    }

    private fun onOutput(frame: ByteArray) {
        counters.transportOutputCallbacks.incrementAndGet()
        val decoded = ControllerLinkDataPlane.decodeOutput(frame)
        if (decoded == null) {
            listener?.onOutputReport(null, null)
            return
        }
        listener?.onOutputReport(decoded.payload, decoded.reportId)
    }

    override fun send(reportId: Int, payload: ByteArray): Boolean {
        if (!up) return false
        if (reportId != ControllerReportEncoder.REPORT_ID) return false
        if (payload.size != ControllerLinkDataPlane.PAYLOAD_BYTES) return false
        return mailbox.trySend(payload.copyOf()).isSuccess
    }

    override fun stop() {
        val target = plane()
        val wasRunning = session?.isActive == true || up
        session?.cancel()
        session = null
        if (!wasRunning) return
        scope.launch {
            withContext(NonCancellable) {
                if (target != null) linkDown(target, null, sendStop = true) else markDown(null)
            }
        }
    }

    private suspend fun linkDown(target: ControllerLinkPlane, message: String?, sendStop: Boolean) {
        if (!up && !sendStop) return
        up = false
        runCatching { target.close() }
        // The adapter publishes neutral before dropping the source and keeps
        // management up: stopping Controller Link is not carrier loss.
        if (sendStop && target.available) runCatching { target.stop() }
        markDown(message)
    }

    private fun markDown(message: String?) {
        up = false
        BridgeForegroundService.stop(appContext, diagnostics)
        diagnostics.event("transport", "clink.down", message ?: "stopped")
        listener?.onLinkDown(message)
        listener?.onPhase(BridgeLinkPhase.Idle, hostName, message)
    }

    private fun fail(message: String) {
        diagnostics.event("transport", "clink.failed", message)
        listener?.onPhase(BridgeLinkPhase.Failed, hostName, message)
    }

    override fun close() {
        stop()
        scope.cancel()
    }

    private companion object {
        /** Well inside the adapter's 300 ms stale-input timeout. */
        const val KEEPALIVE_MS = 100L
        const val FAILURES_BEFORE_CHECK = 5
    }
}
