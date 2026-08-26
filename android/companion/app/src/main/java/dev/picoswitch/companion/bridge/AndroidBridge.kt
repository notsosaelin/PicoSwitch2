package dev.picoswitch.companion.bridge

import android.content.Context
import dev.picoswitch.bridge.core.BridgeCounters
import dev.picoswitch.bridge.core.BridgeDiagnostics
import dev.picoswitch.bridge.core.ControllerFaceLayout
import dev.picoswitch.bridge.session.BridgeSession
import dev.picoswitch.bridge.touch.TouchGamepad
import dev.picoswitch.bridge.touch.TouchLatchEvent
import dev.picoswitch.bridge.touch.TouchReleaseReason

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

    /**
     * The on-screen controller, above the same shared state machine.
     *
     * Not a second input path: it feeds [AndroidInputBackend.controller] exactly
     * as the key/motion adapter does, and the session below it cannot tell which
     * one produced the state it is encoding.
     */
    val touch = TouchGamepad(input.controller)

    /**
     * The physical source to hand input back to when the on-screen controller is
     * put away.
     *
     * Held here rather than left in the input backend because the two answer
     * different questions while touch mode is active: the backend still knows
     * which pad the user chose, and this knows which pad to re-bind the session's
     * actuators to on the way out.
     */
    private var restoreDescriptor: String? = null

    init {
        selfTest.register()
        // Transitions only -- a double tap, an unlatch, or a boundary that
        // dropped holds that were still on. This is the one part of the touch
        // path that can leave the console holding a button with nothing on
        // screen touching it, so the log that explains a stuck control has to
        // exist; it stays readable because none of these happen per contact.
        touch.setLatchObserver { event ->
            val detail = when (event) {
                is TouchLatchEvent.Engaged ->
                    "control=${event.controlId} state=latched reason=double_tap_hold_slide"
                is TouchLatchEvent.Released ->
                    "control=${event.controlId} state=released reason=press_hold"
                is TouchLatchEvent.Cleared ->
                    "controls=${event.controlIds.sorted().joinToString(",")} " +
                        "state=released reason=${event.reason.name}"
            }
            diagnostics.event("controller", "touch latch", detail)
        }
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
        if (touch.active) {
            // The on-screen controller owns gameplay input AND the host's own
            // actuator right now. Applying a physical selection here would rebind
            // rumble to a pad that is driving nothing, so the choice is recorded
            // and applied on the way out instead.
            restoreDescriptor = device?.descriptor
            return
        }
        input.select(device)
        session.bindSource(input.selectedSource, input.sourceCapabilities())
        input.setFaceLayout(layoutStore.load(input.selectedDescriptor))
    }

    fun setFaceLayout(layout: ControllerFaceLayout) {
        if (touch.active) {
            layoutStore.saveTouch(layout)
            applyLayout(layout)
            return
        }
        val descriptor = input.selectedDescriptor ?: return
        layoutStore.save(descriptor, layout)
        applyLayout(layout)
    }

    /**
     * Remap face controls without leaving one held.
     *
     * The engine has to be released alongside the state machine: neutralizing
     * only the state machine leaves the engine believing a control is still down,
     * and the next contact event would republish it under the NEW mapping — a
     * button the user pressed before the change arriving as a different one.
     */
    private fun applyLayout(layout: ControllerFaceLayout) {
        touch.release(TouchReleaseReason.AuthorityChanged)
        input.setFaceLayout(layout)
        session.neutralize()
    }

    /**
     * Make the touchscreen the controller.
     *
     * Order matters and is the whole point of doing this in one place:
     *
     * 1. remember the physical selection, before anything disturbs it;
     * 2. neutralize while the link is still up, so whatever the physical controls
     *    were holding is cleared on the CONSOLE rather than merely forgotten here;
     * 3. take input authority (which releases the engine and neutralizes again);
     * 4. rebind the session to the host itself — the phone's own vibrator is the
     *    legitimate actuator when the phone is the controller, and binding to no
     *    source is exactly what resolves it. No synthetic device is invented;
     * 5. apply the on-screen controller's own face presentation.
     */
    fun enterTouchMode() {
        if (touch.active) return
        restoreDescriptor = input.selectedDescriptor
        session.neutralize()
        touch.activate()
        session.bindSource(null, input.touchCapabilities)
        input.setFaceLayout(layoutStore.loadTouch())
        diagnostics.event("controller", "touch mode", "on-screen controller is authoritative")
    }

    /** Put the on-screen controller away and give the physical pad its input back. */
    fun exitTouchMode() {
        if (!touch.active) return
        touch.deactivate()
        session.neutralize()
        val descriptor = restoreDescriptor
        restoreDescriptor = null
        selectSource(descriptor)
        diagnostics.event("controller", "touch mode", "physical input restored")
    }

    /** Drop every held on-screen control without giving up authority. */
    fun releaseTouchInput(reason: TouchReleaseReason) {
        if (!touch.active) return
        touch.release(reason)
    }

    fun close() {
        selfTest.unregister()
        touch.deactivate()
        session.close()
        motion.close()
    }
}
