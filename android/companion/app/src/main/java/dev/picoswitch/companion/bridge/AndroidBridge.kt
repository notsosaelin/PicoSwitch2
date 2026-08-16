package dev.picoswitch.companion.bridge

import android.content.Context
import dev.picoswitch.bridge.core.BridgeCounters
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.session.BridgeSession

/**
 * The Android platform backend, assembled.
 *
 * One place where every Android-specific piece is plugged into the shared
 * [BridgeSession], so the wiring is visible as a unit rather than scattered
 * through a ViewModel — and so a future Windows or Linux implementation has an
 * obvious thing to mirror: same session, same core, four different backends.
 *
 * ```text
 * AndroidInputBackend   ->  ControllerInputState  \
 * AndroidMotionBackend                             \
 * AndroidBatteryBackend                             >  BridgeSession  ->  PicoSwitch2
 * AndroidOutputBackend                             /
 * AndroidHidTransport                             /
 * ```
 *
 * The application talks to [input] for source selection and to [session] for
 * everything else. Neither exposes an Android type, which is what keeps the UI
 * and the ViewModel free of platform HID details.
 */
class AndroidBridge(
    context: Context,
    private val diagnostics: BridgeDiagnostics = BridgeDiagnostics.None,
) {
    val input = AndroidInputBackend()

    /**
     * ONE counter set for the whole path. The transport counts raw HID callbacks
     * into it and the session counts everything downstream, so reading the
     * snapshot in order localizes a break to a single boundary.
     */
    private val counters = BridgeCounters()

    private val output = AndroidOutputBackend(context, diagnostics)
    private val motion = AndroidMotionBackend(context)
    private val battery = AndroidBatteryBackend(context)
    private val transport = AndroidHidTransport(context, diagnostics, counters)
    private val selfTest = OutputSelfTest(context, output, diagnostics)

    val session = BridgeSession(
        transport = transport,
        input = input.controller,
        motion = motion,
        battery = battery,
        output = output,
        diagnostics = diagnostics,
        counters = counters,
    )

    /** Per-source layout preference, persisted by Android. */
    val layoutStore = AndroidControllerLayoutStore(context)

    init {
        selfTest.register()
        // Object identity, once, at assembly. A refactor can compile perfectly
        // while wiring half the application to a second instance; this is the
        // cheapest proof that the transport, the session and all four backends
        // are the objects the running app actually uses.
        // Only the FAILURE is logged at startup. The identity dump is available
        // on demand through wiringReport() and the diagnostics export, which is
        // where it was actually read from -- two unconditional lines per launch
        // were pure log volume.
        auditWiring()?.let { diagnostics.event("bridge", "ASSEMBLY MISMATCH", it) }
    }

    /** Both views of the graph, for the diagnostics export. */
    fun wiringReport(): String =
        assemblerWiring() + " || session view: " + session.wiring() +
            (auditWiring()?.let { "  <<< MISMATCH: $it" } ?: "  (audit clean)")

    private fun assemblerWiring() =
        "bridge=${id(this)} session=${id(session)} transport=${id(transport)} " +
            "input=${id(input)} state=${id(input.controller)} motion=${id(motion)} " +
            "battery=${id(battery)} output=${id(output)} counters=${id(counters)}"

    /**
     * Prove every backend is attached to the session this app actually runs.
     *
     * Returns null when the graph is correct, or a description of the first
     * disagreement. This is a real check, not a log: the counters bug earlier in
     * this investigation was exactly this shape — two instances, both alive, both
     * plausible, neither obviously wrong — and it was invisible until something
     * compared them.
     */
    fun auditWiring(): String? {
        val problems = mutableListOf<String>()
        if (session.counters !== counters) problems += "session counters are a different instance"
        if (transport.attachedListener() !== session) {
            problems += "transport delivers to ${id(transport.attachedListener() ?: "nothing")}, not session ${id(session)}"
        }
        // The session's own view must name the same collaborators the assembler holds.
        val view = session.wiring()
        mapOf(
            "transport" to transport, "input" to input.controller,
            "motion" to motion, "battery" to battery, "output" to output,
        ).forEach { (name, instance) ->
            if (!view.contains("$name=${id(instance)}")) {
                problems += "$name in session is not ${id(instance)}"
            }
        }
        return problems.takeIf { it.isNotEmpty() }?.joinToString("; ")
    }

    /**
     * Snapshot of every boundary counter, plus the first stage that produced
     * nothing while its upstream neighbour did. Read this FIRST when a feature is
     * missing; it names the broken boundary instead of requiring a guess.
     */
    fun countersLine(): String {
        val divergence = counters.firstDivergence()
        return session.counters.snapshot() +
            if (divergence != null) "  <<< FIRST DIVERGENCE: $divergence" else "  (no divergence)"
    }

    private fun id(value: Any) = Integer.toHexString(System.identityHashCode(value))

    /**
     * Point both halves of the bridge at a newly selected source.
     *
     * Input selection and output binding are one operation on purpose: a session
     * whose actuators still belong to the previous device is exactly the state
     * where rumble silently goes nowhere.
     */
    fun selectSource(descriptor: String?) {
        val device = descriptor?.let { wanted ->
            input.eligibleDevices().firstOrNull { it.descriptor == wanted }
        }
        input.select(device)
        session.bindSource(input.selectedSource, input.sourceCapabilities())
        input.setFaceLayout(layoutStore.load(input.selectedDescriptor))
    }

    fun setFaceLayout(layout: ControllerFaceLayout) {
        val descriptor = input.selectedDescriptor ?: return
        layoutStore.save(descriptor, layout)
        input.setFaceLayout(layout)
        session.neutralize()
    }

    fun close() {
        selfTest.unregister()
        session.close()
        motion.close()
    }
}
