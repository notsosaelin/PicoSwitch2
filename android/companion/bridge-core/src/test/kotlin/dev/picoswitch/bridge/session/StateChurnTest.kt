package dev.picoswitch.bridge.session

import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.ControllerSourceIdentity
import dev.picoswitch.bridge.core.RumbleRequest
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestCoroutineScheduler
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * How often does the observable [BridgeState] change while motion streams?
 *
 * The application collects this flow straight into its UI state, so every
 * emission is a recomposition. At the 125 Hz report cadence, anything that
 * writes state per report is a sustained UI load — and on Android the platform
 * delivers sensor callbacks on the same main thread that load lands on.
 *
 * This test MEASURES the rate instead of arguing about it, and pins the one
 * property that is actually controllable: motion diagnostics must not contribute
 * a per-report emission. Their sample text changes every single frame, so
 * comparing the whole diagnostics object republishes state forever.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class StateChurnTest {

    /** Sample text that differs on EVERY call, exactly like live sensor values. */
    private class ChurningMotion : MotionBackend {
        var reads = 0
        override val available = true
        private var running = false
        override fun start() { running = true }
        override fun stop() { running = false }
        override fun sample() =
            if (running) ControllerMotion(gyroX = 5, accelZ = 8192, valid = true)
            else ControllerMotion.None

        override fun diagnostics(): MotionDiagnostics {
            reads++
            return MotionDiagnostics(
                platformRaw = "gyro sample #$reads",
                canonical = "canonical sample #$reads",
                frameRotationDegrees = 90,
                frameRotationMeasured = true,
            )
        }
    }

    private class Transport : BridgeTransport {
        lateinit var listener: BridgeTransport.Listener
        var sent = 0
        override fun attach(listener: BridgeTransport.Listener) { this.listener = listener }
        override fun attachedListener(): BridgeTransport.Listener? =
            if (this::listener.isInitialized) listener else null
        override fun knownHosts() = emptyList<BridgeHost>()
        override fun start(preferredHost: BridgeHost?) = Unit
        override fun connect(host: BridgeHost) = Unit
        override fun send(reportId: Int, payload: ByteArray): Boolean { sent++; return true }
        override fun stop() = Unit
        override fun close() = Unit
    }

    private class Battery : BatteryBackend {
        override val available = true
        override fun read() = ControllerBattery(50, false, true)
    }

    private class Output : OutputBackend {
        override fun bindToSource(source: ControllerSourceIdentity?) = Unit
        override fun apply(request: RumbleRequest) = Unit
        override fun keepAlive() = Unit
        override fun stop() = Unit
        override fun diagnostics() = OutputDiagnostics(route = "fake", motors = 1)
    }

    @Test fun `motion diagnostics do not republish state on every report`() = runTest {
        val scheduler: TestCoroutineScheduler = testScheduler
        val transport = Transport()
        val motion = ChurningMotion()
        val session = BridgeSession(
            transport = transport,
            input = ControllerInputState(),
            motion = motion,
            battery = Battery(),
            output = Output(),
            dispatcher = StandardTestDispatcher(scheduler),
            clock = { scheduler.currentTime },
        )

        // Count emissions attributable to the motion field alone. reportCount also
        // changes per report -- that is pre-existing and separate -- so compare the
        // motion field specifically rather than whole-state emissions.
        var motionFieldChanges = 0
        var lastMotion = session.state.value.motion
        var totalEmissions = 0
        val collector = launch(Dispatchers.Unconfined) {
            session.state.collect {
                totalEmissions++
                if (it.motion != lastMotion) {
                    motionFieldChanges++
                    lastMotion = it.motion
                }
            }
        }

        transport.listener.onLinkUp(null)
        transport.listener.onOutputReport(byteArrayOf(0, 0, 1, 1), 2)
        scheduler.advanceTimeBy(60 * BridgeSession.REPORT_INTERVAL_MS)
        scheduler.runCurrent()
        collector.cancel()
        session.close()
        scheduler.runCurrent()

        val reports = transport.sent
        // Measured, not assumed: print before asserting so a failure still reports
        // the numbers instead of hiding them behind the first failed check.
        println("StateChurnTest: reports=$reports totalEmissions=$totalEmissions " +
            "motionFieldChanges=$motionFieldChanges diagnosticsReads=${motion.reads}")
        assertTrue("expected a streaming sender, got $reports reports", reports >= 30)

        // THE INVARIANT: the rotation never changed, so the motion field must have
        // settled after at most one publish, no matter how many reports went out.
        assertTrue(
            "motion field republished $motionFieldChanges times across $reports reports; " +
                "sample text must not reach observable state on the report path",
            motionFieldChanges <= 1,
        )

        // And the backend IS still consulted on the report path for the cheap
        // rotation check, which is what makes the comparison above load-bearing:
        // the fix is "do not publish", not "do not look".
        assertTrue("diagnostics() reads: ${motion.reads}", motion.reads >= 30)
    }

    /** The detailed text is still reachable — just only when someone asks. */
    @Test fun `on demand refresh publishes the live sample text`() = runTest {
        val scheduler: TestCoroutineScheduler = testScheduler
        val transport = Transport()
        val motion = ChurningMotion()
        val session = BridgeSession(
            transport = transport,
            input = ControllerInputState(),
            motion = motion,
            battery = Battery(),
            output = Output(),
            dispatcher = StandardTestDispatcher(scheduler),
            clock = { scheduler.currentTime },
        )
        transport.listener.onLinkUp(null)
        transport.listener.onOutputReport(byteArrayOf(0, 0, 1, 1), 2)
        scheduler.advanceTimeBy(10 * BridgeSession.REPORT_INTERVAL_MS)
        scheduler.runCurrent()

        session.refreshMotionDiagnostics()
        val text = session.state.value.motion
        assertTrue(text.platformRaw, text.platformRaw.startsWith("gyro sample #"))
        assertEquals(90, text.frameRotationDegrees)

        session.close()
        scheduler.runCurrent()
    }
}
