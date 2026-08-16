package dev.picoswitch.bridge.session

import dev.picoswitch.bridge.core.ControllerBattery
import dev.picoswitch.bridge.core.ControllerButton
import dev.picoswitch.bridge.core.ControllerInputState
import dev.picoswitch.bridge.core.ControllerMotion
import dev.picoswitch.bridge.core.ControllerSourceIdentity
import dev.picoswitch.bridge.core.ControllerState
import dev.picoswitch.bridge.core.DeviceCapabilities
import dev.picoswitch.bridge.core.RumbleRequest
import dev.picoswitch.bridge.protocol.ControllerReportEncoder
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestCoroutineScheduler
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The reusable center, driven entirely by fakes.
 *
 * Every rule proved here used to live inside the Android HID bridge and could
 * only be checked on a phone. That is the concrete payoff of the transport /
 * backend split: link handling, motion gating, rumble delivery and neutralization
 * are now testable on the JVM, and a future platform inherits the proofs.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class BridgeSessionTest {

    // ------------------------------------------------------------------- fakes

    private class FakeHost(
        override val address: String = "AA:BB",
        override val name: String? = "PicoSwitch2",
    ) : BridgeHost

    private class FakeTransport : BridgeTransport {
        lateinit var listener: BridgeTransport.Listener
        val sent = mutableListOf<ByteArray>()
        var acceptSends = true
        var stopped = false
        var closed = false

        override fun attach(listener: BridgeTransport.Listener) { this.listener = listener }
        override fun attachedListener(): BridgeTransport.Listener? =
            if (this::listener.isInitialized) listener else null
        override fun knownHosts() = listOf<BridgeHost>(FakeHost())
        override fun start(preferredHost: BridgeHost?) = Unit
        override fun connect(host: BridgeHost) = Unit
        override fun send(reportId: Int, payload: ByteArray): Boolean {
            if (!acceptSends) return false
            sent += payload
            return true
        }
        override fun stop() { stopped = true }
        override fun close() { closed = true }
    }

    private class FakeMotion(override var available: Boolean = true) : MotionBackend {
        var running = false
        var startCount = 0
        val sample = ControllerMotion(gyroX = 5, accelZ = 8192, timestampTicks = 7, valid = true)
        override fun start() { running = true; startCount++ }
        override fun stop() { running = false }
        override fun sample() = if (running) sample else ControllerMotion.None
        override fun diagnostics() = MotionDiagnostics(
            platformRaw = if (running) "raw" else "idle",
            canonical = if (running) "canonical" else "idle",
            frameRotationDegrees = 90,
            frameRotationMeasured = true,
        )
    }

    private class FakeBattery : BatteryBackend {
        override val available = true
        var reads = 0
        override fun read(): ControllerBattery {
            reads++
            return ControllerBattery(levelPercent = 62, charging = false, valid = true)
        }
    }

    private class FakeOutput(private val motors: Int = 2) : OutputBackend {
        var boundTo: ControllerSourceIdentity? = null
        val applied = mutableListOf<RumbleRequest>()
        var stops = 0
        var keepAlives = 0
        override fun bindToSource(source: ControllerSourceIdentity?) { boundTo = source }
        override fun apply(request: RumbleRequest) { applied += request }
        override fun keepAlive() { keepAlives++ }
        override fun stop() { stops++ }
        override fun diagnostics() = OutputDiagnostics(route = "fake", motors = motors)
    }

    private class Fixture(private val scheduler: TestCoroutineScheduler) {
        val transport = FakeTransport()
        val input = ControllerInputState()
        val motion = FakeMotion()
        val battery = FakeBattery()
        val output = FakeOutput()

        /** Wall clock, advanced explicitly; independent of virtual coroutine time. */
        var now = 0L

        val session = BridgeSession(
            transport = transport,
            input = input,
            motion = motion,
            battery = battery,
            output = output,
            dispatcher = StandardTestDispatcher(scheduler),
            clock = { now },
        )

        val listener: BridgeTransport.Listener get() = transport.listener

        /**
         * Advance both clocks together, which is what the production loop sees.
         *
         * Deliberately never "advance until idle": while motion streams, the
         * sender is an unbounded `delay()` loop and a virtual-time scheduler
         * never becomes idle. A test that asks for idle there hangs forever.
         */
        fun tick(millis: Long) {
            now += millis
            scheduler.advanceTimeBy(millis)
            scheduler.runCurrent()
        }

        /** Let queued work run without moving time. */
        fun settle() = scheduler.runCurrent()
    }

    private inline fun withSession(scheduler: TestCoroutineScheduler, block: (Fixture) -> Unit) {
        val fixture = Fixture(scheduler)
        try {
            block(fixture)
        } finally {
            // The sender must be cancelled or the scheduler still has pending work
            // when the test framework checks.
            fixture.session.close()
            scheduler.runCurrent()
        }
    }

    private fun outputReport(left: Int, right: Int, player: Int, motion: Boolean) =
        byteArrayOf(left.toByte(), right.toByte(), player.toByte(), if (motion) 1 else 0)

    // -------------------------------------------------------------- capabilities

    @Test fun `host capabilities are known before any source is selected`() =
        withSession(TestCoroutineScheduler()) { f ->
            val caps = f.session.state.value.capabilities
            // The UI must be able to say "this device has no gyroscope" on an empty
            // controller screen, before the user picks anything.
            assertTrue(caps.motion)
            assertEquals(2, caps.rumbleMotors)
            assertTrue(caps.battery)
            // ...and claim nothing about a source it has not been told about.
            assertFalse(caps.gamepadButtons)
            assertEquals(0, caps.analogSticks)
        }

    @Test fun `binding a source merges the source and host halves`() =
        withSession(TestCoroutineScheduler()) { f ->
            val identity = ControllerSourceIdentity("desc", "Odin Controller", 0x2020, 0x0111)

            f.session.bindSource(identity, DeviceCapabilities(gamepadButtons = true, analogSticks = 2))

            assertEquals(identity, f.output.boundTo)
            val caps = f.session.state.value.capabilities
            assertTrue(caps.gamepadButtons)
            assertEquals(2, caps.analogSticks)
            // Host half is filled in by the session, not trusted from the caller.
            assertTrue(caps.motion)
            assertEquals(2, caps.rumbleMotors)
            assertEquals("fake", f.session.state.value.output.route)
        }

    // ------------------------------------------------------------ output reports

    @Test fun `an output report becomes rumble a player indicator and a motion gate`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onOutputReport(outputReport(200, 40, 3, motion = true), 2)

            assertEquals(listOf(RumbleRequest(200, 40)), f.output.applied)
            assertTrue(f.motion.running)
            assertEquals(3, f.session.state.value.playerIndicator)
            assertEquals(200, f.session.state.value.rumbleAmplitude)
            assertTrue(f.session.state.value.motionActive)

            // Clearing the gate must release the sensors rather than latch them.
            f.listener.onOutputReport(outputReport(0, 0, 3, motion = false), 2)
            assertFalse(f.motion.running)
            assertFalse(f.session.state.value.motionActive)
        }
    }

    /**
     * The gate is edge-triggered. Re-registering sensors on every repeated report
     * would be a silent battery cost at the report cadence.
     */
    @Test fun `a repeated motion request does not restart the sensors`() = runTest {
        withSession(testScheduler) { f ->
            repeat(5) { f.listener.onOutputReport(outputReport(0, 0, 1, motion = true), 2) }
            assertEquals(1, f.motion.startCount)
        }
    }

    @Test fun `a report with a foreign id is never applied as rumble`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onOutputReport(outputReport(255, 255, 1, motion = true), reportId = 9)
            assertTrue(f.output.applied.isEmpty())
            assertFalse(f.motion.running)
        }
    }

    // -------------------------------------------------------------- sender rules

    @Test fun `input changes are sent once the link is up`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp("PicoSwitch2")
            f.settle()

            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS)

            assertTrue(f.transport.sent.isNotEmpty())
            val report = f.transport.sent.last()
            assertEquals(ControllerReportEncoder.PAYLOAD_SIZE_V2, report.size)
            // Button 1 (A) is bit 0 of the first button byte.
            assertEquals(0x01, report[6].toInt() and 0xFF)
            assertEquals(BridgeLinkPhase.Playing, f.session.state.value.phase)
            assertTrue(f.session.state.value.reportCount > 0)
        }
    }

    /**
     * With motion off the loop is change-driven, exactly as the hardware-validated
     * v1 path was: an unchanged state must not generate traffic.
     */
    @Test fun `an unchanged state generates no traffic while motion is off`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS * 2)
            val after = f.transport.sent.size
            assertTrue(after > 0)

            f.tick(500)
            assertEquals(after, f.transport.sent.size)
        }
    }

    /**
     * With motion live the loop becomes time-driven, because a fresh IMU sample
     * has to go out every interval even when no button or axis moved.
     */
    @Test fun `motion makes the sender time driven`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.listener.onOutputReport(outputReport(0, 0, 1, motion = true), 2)
            f.settle()
            val before = f.transport.sent.size

            f.tick(10 * BridgeSession.REPORT_INTERVAL_MS)

            assertTrue(
                "expected repeated reports while motion streams, got ${f.transport.sent.size - before}",
                f.transport.sent.size - before >= 5,
            )
            // The sample really is in the report: gyro X at payload offset 9.
            val report = f.transport.sent.last()
            assertEquals(5, report[9].toInt() and 0xFF)
            assertEquals(
                ControllerReportEncoder.FLAG_MOTION_VALID,
                report[22].toInt() and ControllerReportEncoder.FLAG_MOTION_VALID,
            )
        }
    }

    @Test fun `motion is omitted from the report until the console asks for it`() = runTest {
        withSession(testScheduler) { f ->
            f.motion.running = true // sensors live, but no request from the console
            val report = f.listener.currentReport()
            assertEquals(0, report[22].toInt() and ControllerReportEncoder.FLAG_MOTION_VALID)
            assertEquals(0, report[9].toInt())
        }
    }

    @Test fun `battery is polled on its slow timer and published`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.listener.onOutputReport(outputReport(0, 0, 1, motion = true), 2)
            f.tick(BridgeSession.REPORT_INTERVAL_MS)
            assertEquals(62, f.session.state.value.batteryPercent)

            val firstReads = f.battery.reads
            // Well inside the poll interval: no second read.
            f.tick(BridgeSession.BATTERY_POLL_MS / 4)
            assertEquals(firstReads, f.battery.reads)

            f.tick(BridgeSession.BATTERY_POLL_MS)
            assertTrue(f.battery.reads > firstReads)
        }
    }

    // ---------------------------------------------------------------- teardown

    /**
     * The single most important teardown rule: losing the link must not leave the
     * host vibrating, its IMU registered, or a button held on the console.
     */
    @Test fun `losing the link releases every resource and clears held input`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.listener.onOutputReport(outputReport(255, 255, 2, motion = true), 2)
            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS * 2)

            f.listener.onLinkDown("dropped")
            f.settle()

            assertFalse(f.motion.running)
            assertTrue(f.output.stops > 0)
            assertEquals(ControllerState.Neutral, f.input.state.value)
            assertEquals(BridgeLinkPhase.Idle, f.session.state.value.phase)
            assertEquals(0, f.session.state.value.playerIndicator)
            assertEquals(0, f.session.state.value.rumbleAmplitude)
            assertFalse(f.session.state.value.motionActive)

            // And the sender is really gone, not merely idle.
            val after = f.transport.sent.size
            f.input.pressButton(ControllerButton.B, true)
            f.tick(200)
            assertEquals(after, f.transport.sent.size)
        }
    }

    @Test fun `stopping sends a neutral report before tearing the link down`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS * 2)
            assertNotEquals(0, f.transport.sent.last()[6].toInt())

            f.session.stop()
            f.settle()

            assertTrue(f.transport.stopped)
            assertNeutralPrefix(f.transport.sent.last())
        }
    }

    @Test fun `neutralize pushes a neutral report without ending the session`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS * 2)

            f.session.neutralize()
            f.settle()

            assertFalse(f.transport.stopped)
            assertNeutralPrefix(f.transport.sent.last())
        }
    }

    @Test fun `a rejected report is reported rather than counted`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.transport.acceptSends = false
            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS * 2)

            assertEquals(0L, f.session.state.value.reportCount)
            assertTrue(f.session.state.value.message!!.contains("rejected"))
        }
    }

    /**
     * The watchdog is its own low-rate timer, deliberately not a hook in the
     * sender loop: with motion off that loop blocks waiting for input, so a
     * sender-driven watchdog would false-trip during ordinary idle.
     */
    @Test fun `the output watchdog ticks while the link is live`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.tick(4 * BridgeSession.OUTPUT_WATCHDOG_TICK_MS)
            assertTrue("watchdog ticks: ${f.output.keepAlives}", f.output.keepAlives >= 3)
        }
    }

    // -------------------------------------------------------------- observability

    @Test fun `preparing resets per-link counters but keeps known capabilities`() = runTest {
        withSession(testScheduler) { f ->
            f.session.bindSource(
                ControllerSourceIdentity("d", "n", 1, 2),
                DeviceCapabilities(gamepadButtons = true),
            )
            f.listener.onLinkUp(null)
            f.input.pressButton(ControllerButton.A, true)
            f.tick(BridgeSession.REPORT_INTERVAL_MS * 2)
            assertTrue(f.session.state.value.reportCount > 0)

            f.listener.onPhase(BridgeLinkPhase.Preparing, message = "again")
            assertEquals(0L, f.session.state.value.reportCount)
            assertTrue(f.session.state.value.capabilities.gamepadButtons)
        }
    }

    @Test fun `motion diagnostics reach the observable state while motion streams`() = runTest {
        withSession(testScheduler) { f ->
            f.listener.onLinkUp(null)
            f.listener.onOutputReport(outputReport(0, 0, 1, motion = true), 2)
            f.tick(4 * BridgeSession.REPORT_INTERVAL_MS)

            val diagnostics = f.session.state.value.motion
            assertEquals(90, diagnostics.frameRotationDegrees)
            assertTrue(diagnostics.frameRotationMeasured)
            assertEquals("raw", diagnostics.platformRaw)
            assertEquals("canonical", diagnostics.canonical)
        }
    }

    private fun assertNeutralPrefix(actual: ByteArray) {
        val expected = ControllerReportEncoder.encodeV1(ControllerState.Neutral)
        assertTrue("report too short: ${actual.size}", actual.size >= expected.size)
        expected.indices.forEach { index ->
            assertEquals("byte $index", expected[index], actual[index])
        }
    }
}
