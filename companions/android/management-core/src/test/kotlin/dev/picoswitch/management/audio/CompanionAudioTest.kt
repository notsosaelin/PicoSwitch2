package dev.picoswitch.management.audio

import dev.picoswitch.management.CompanionAudioState
import dev.picoswitch.management.ManagementCommands
import dev.picoswitch.management.ManagementProtocol
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Mirrors the Windows CompanionAudioDataPlaneTests / CompanionAudioPlayout tests. */
class CompanionAudioPlaneAndPlayoutTest {
    @Test fun `frames decode at the 96 kbit per s session size only`() {
        assertEquals(244, CompanionAudioDataPlane.MAX_FRAME_BYTES)
        assertEquals(96, CompanionAudioDataPlane.kbps(244))
        assertEquals(247, CompanionAudioDataPlane.minimumAttMtu(244))
        val frame = ByteArray(244).also {
            it[0] = 1; it[1] = 0x90.toByte(); it[2] = 0xEF.toByte(); it[3] = 0xBE.toByte(); it[4] = 0xFC.toByte()
        }
        val decoded = CompanionAudioDataPlane.decode(frame, 244)!!
        assertEquals(0xBEEF, decoded.sequence)
        assertEquals(240, decoded.opus.size)
        assertEquals(0xFC.toByte(), decoded.opus[0])
        assertNull(CompanionAudioDataPlane.decode(frame, 164))
        assertNull(CompanionAudioDataPlane.decode(frame.copyOf(243), 244))
        assertNull(CompanionAudioDataPlane.decode(frame.copyOf().also { it[1] = 0 }, 244))
    }

    @Test fun `sequence gaps are wrap safe`() {
        assertEquals(0, CompanionAudioDataPlane.gap(0xFFFF, 0))
        assertEquals(2, CompanionAudioDataPlane.gap(10, 13))
        assertEquals(-1, CompanionAudioDataPlane.gap(10, 10))
        assertTrue(CompanionAudioDataPlane.gap(10, 5) < 0)
    }

    @Test fun `policy constants match the simulated Windows policy`() {
        assertEquals(3, CompanionAudioJitterPolicy.MIN_PREBUFFER_BLOCKS)
        assertEquals(8, CompanionAudioJitterPolicy.MAX_PREBUFFER_BLOCKS)
        assertEquals(30_000, CompanionAudioJitterPolicy.STABLE_BLOCKS_TO_SHRINK)
        assertEquals(6, CompanionAudioPlayout.CLASSIC_SHARED_PREBUFFER_BLOCKS)
        assertEquals(10.0, CompanionAudioPlayout.LOW_WATER_MILLIS, 0.0)
    }

    @Test fun `a classic shared session starts deep and never shrinks below it`() {
        val policy = CompanionAudioJitterPolicy(CompanionAudioPlayout.CLASSIC_SHARED_PREBUFFER_BLOCKS)
        assertEquals(120, policy.prebufferMillis)
        policy.onUnderrun()
        repeat(CompanionAudioJitterPolicy.STABLE_BLOCKS_TO_SHRINK * 4) { policy.onBlockQueued() }
        assertEquals(6, policy.prebufferBlocks)
    }

    @Test fun `a window that needed the deepest block gives nothing back`() {
        val policy = CompanionAudioJitterPolicy()
        policy.onUnderrun()
        repeat(CompanionAudioJitterPolicy.STABLE_BLOCKS_TO_SHRINK - 1) { policy.onBlockQueued() }
        policy.onBlockQueued(spareBlock = false)
        policy.onBlockQueued()
        assertEquals(4, policy.prebufferBlocks)
    }

    @Test fun `drift control holds the target by one frame per block`() {
        val drift = CompanionAudioDriftControl()
        repeat(CompanionAudioDriftControl.SETTLE_BLOCKS - 1) { assertEquals(960, drift.framesFor(70.0, 60.0)) }
        assertEquals(959, drift.framesFor(70.0, 60.0))
        var frames = 0
        repeat(400) { frames = drift.framesFor(40.0, 60.0) }
        assertEquals(961, frames)
        repeat(400) { frames = drift.framesFor(60.0, 60.0) }
        assertEquals(960, frames)
    }

    @Test fun `a target that moves past the queue reverses the correction`() {
        val drift = CompanionAudioDriftControl()
        repeat(300) { drift.framesFor(40.0, 60.0) }
        var frames = 0
        repeat(5) { frames = drift.framesFor(40.0, 20.0) }
        assertEquals(959, frames)
    }

    @Test fun `stretch keeps both ends and stays monotonic`() {
        val input = ShortArray(960 * 2) { i -> if (i % 2 == 0) (i / 2 * 10).toShort() else (-(i / 2) * 10).toShort() }
        for (frames in listOf(959, 961)) {
            val output = ShortArray(frames * 2)
            assertEquals(frames * 2, CompanionAudioDriftControl.stretch(input, input.size, 2, frames, output))
            assertEquals(0.toShort(), output[0])
            assertEquals(9590.toShort(), output[output.size - 2])
            assertEquals((-9590).toShort(), output[output.size - 1])
            for (i in 1 until frames) assertTrue(output[i * 2] >= output[(i - 1) * 2])
        }
    }

    @Test fun `playout starts on the prebuffer, underruns when dry, grows before it is dry`() {
        val playout = CompanionAudioPlayout()
        for (i in 0 until 3) {
            playout.accept(i, i * 20.0)
            assertEquals(i == 2, playout.queued(i + 1))
        }
        val near = playout.accept(1, CompanionAudioPlayout.LOW_WATER_MILLIS - 1)
        assertTrue(near.extend)
        assertEquals(60.0, playout.targetMillis, 0.0)
        val dry = playout.accept(0, 0.0)
        assertTrue(dry.underrun)
        assertEquals(1L, playout.underruns)
        assertTrue(playout.accept(playout.maxQueuedBlocks, 300.0).drop)
    }
}

class CompanionAudioReceiverTest {
    private class CountingDecoder : CompanionAudioDecoder {
        var concealments = 0
        override fun decode(opus: ByteArray?, pcm: ShortArray): Int {
            if (opus == null) concealments++
            return CompanionAudioDataPlane.SAMPLES_PER_CHANNEL
        }
        override fun reset() = Unit
    }

    private fun frame(sequence: Int) = ByteArray(244).also {
        it[0] = 1; it[1] = 0x90.toByte(); it[2] = (sequence and 0xFF).toByte(); it[3] = (sequence shr 8).toByte()
        it[4] = 0xFC.toByte()
    }

    @Test fun `gaps are concealed up to the cap and stale frames dropped`() {
        val decoder = CountingDecoder()
        var blocks = 0
        val receiver = CompanionAudioReceiver(decoder, { _, _ -> blocks++ }, 244)
        receiver.accept(frame(1))
        receiver.accept(frame(7)) // 5 lost, 3 concealed
        receiver.accept(frame(7)) // duplicate
        receiver.accept(ByteArray(10))
        assertEquals(2L, receiver.framesDecoded)
        assertEquals(5L, receiver.framesLost)
        assertEquals(3L, receiver.framesConcealed)
        assertEquals(1L, receiver.framesStale)
        assertEquals(1L, receiver.framesMalformed)
        assertEquals(5, blocks)
        receiver.concealOne()
        assertEquals(1L, receiver.framesGrown)
        assertEquals(4, decoder.concealments)
    }
}

class CompanionAudioProtocolTest {
    @Test fun `caudio status parses including kbps and nested frame counters`() {
        val state = ManagementProtocol.companionAudio(
            ManagementCommands.COMPANION_AUDIO_STATUS,
            """{"caudio":{"supported":true,"active":true,"subscribed":true,"usb_speaker":false,""" +
                """"version":1,"codec":"opus","sample_rate":48000,"channels":2,"frame_ms":20,""" +
                """"frame_bytes":244,"kbps":96,"att_mtu":517,"min_att_mtu":247,"generation":3,""" +
                """"admit":"ok","last_stop":"","frames":{"sent":10,"busy":1,"failed":0,"rejected":0},""" +
                """"send_max_gap_us":21000}}""",
        )
        assertTrue(state.active)
        assertFalse(state.usbSpeakerActive)
        assertEquals(244, state.frameBytes)
        assertEquals(96, state.kbps)
        assertEquals(10L, state.framesSent)
        assertNull(CompanionAudioSession.geometryProblem(state))
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class CompanionAudioSessionTest {
    private fun accepted(usbSpeaker: Boolean = true) = CompanionAudioState(
        supported = true, active = true, usbSpeakerActive = usbSpeaker, version = 1, codec = "opus",
        sampleRate = 48_000, channels = 2, frameMillis = 20, frameBytes = 244, attMtu = 517,
        minimumAttMtu = 247, admit = "ok", kbps = 96,
    )

    private class FakeControl(var startReply: CompanionAudioState, var statusReply: CompanionAudioState) :
        CompanionAudioControl {
        var stops = 0
        var statusQueries = 0
        var startMicReply: CompanionAudioState? = null
        val startRequests = mutableListOf<Boolean>()
        override suspend fun start(microphone: Boolean): CompanionAudioState {
            startRequests += microphone
            return if (microphone) startMicReply ?: startReply.copy(mic = true) else startReply
        }
        override suspend fun stop(): CompanionAudioState { stops++; return CompanionAudioState(supported = true) }
        override suspend fun status(): CompanionAudioState { statusQueries++; return statusReply }
    }

    private class FakeDownlink : CompanionAudioDownlink {
        var onFrame: ((ByteArray) -> Unit)? = null
        var closed = false
        override val attMtu = 517
        override suspend fun open(onFrame: (ByteArray) -> Unit): Boolean { this.onFrame = onFrame; return true }
        override suspend fun close() { closed = true }
    }

    private class FakeSink : CompanionAudioSink {
        var started = false
        var stopped = false
        var blocks = 0
        override fun start(sampleRate: Int, channels: Int) { started = true }
        override fun write(pcm: ShortArray, samples: Int): Boolean { blocks++; return true }
        override fun stop() { stopped = true }
        override val underruns = 0L
        override val prebufferMillis = 120
        override fun takeConcealRequest() = false
    }

    private object Decoder : CompanionAudioDecoder {
        override fun decode(opus: ByteArray?, pcm: ShortArray) = CompanionAudioDataPlane.SAMPLES_PER_CHANNEL
        override fun reset() = Unit
    }

    private class Rig(scope: TestScope, link: Boolean = true, start: CompanionAudioState, status: CompanionAudioState = start) {
        var now = 0L
        val ready = MutableStateFlow(true)
        val controllerLink = MutableStateFlow(link)
        val control = FakeControl(start, status)
        val downlink = FakeDownlink()
        val sink = FakeSink()
        val session = CompanionAudioSession(
            scope = scope.backgroundScope,
            control = control,
            downlinks = { downlink },
            sinks = { sink },
            decoders = { Decoder },
            managementReady = ready,
            controllerLink = controllerLink,
            clockMillis = { now },
            runWatchdog = false,
        )
    }

    private fun frame(sequence: Int) = ByteArray(244).also {
        it[0] = 1; it[1] = 0x90.toByte(); it[2] = (sequence and 0xFF).toByte(); it[4] = 0xFC.toByte()
    }

    @Test fun `plays when accepted and tells the adapter to stop`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this, start = accepted())
        runCurrent()
        rig.session.start()
        assertEquals(CompanionAudioPhase.Playing, rig.session.view.value.phase)
        rig.downlink.onFrame!!(frame(1))
        assertEquals(1, rig.sink.blocks)
        rig.session.stop()
        assertEquals(CompanionAudioPhase.Off, rig.session.view.value.phase)
        assertEquals(1, rig.control.stops)
        assertTrue(rig.downlink.closed)
        assertTrue(rig.sink.stopped)
    }

    @Test fun `the headset cannot be turned on without controller link`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this, link = false, start = accepted())
        runCurrent()
        assertFalse(rig.session.view.value.canToggle)
        rig.session.start()
        assertEquals(CompanionAudioPhase.Off, rig.session.view.value.phase)
        assertFalse(rig.sink.started)
        rig.controllerLink.value = true
        runCurrent()
        assertTrue(rig.session.view.value.canToggle)
    }

    @Test fun `controller link ending ends the headset`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this, start = accepted())
        runCurrent()
        rig.session.start()
        rig.controllerLink.value = false
        runCurrent()
        assertEquals(CompanionAudioPhase.Ended, rig.session.view.value.phase)
        assertFalse(rig.session.view.value.canToggle)
        assertEquals(1, rig.control.stops)
        assertTrue(rig.sink.stopped)
    }

    @Test fun `until the console opens its stream the player is told to press a button`() =
        runTest(StandardTestDispatcher()) {
            val rig = Rig(this, start = accepted(usbSpeaker = false))
            runCurrent()
            rig.session.start()
            assertTrue(rig.session.view.value.awaitingConsole)
            rig.downlink.onFrame!!(frame(1))
            assertFalse(rig.session.view.value.awaitingConsole)
        }

    @Test fun `silence asks once and an adapter-ended session ends with its reason`() =
        runTest(StandardTestDispatcher()) {
            val rig = Rig(
                this, start = accepted(),
                status = accepted().copy(active = false, lastStop = "preempted_controller_headset"),
            )
            runCurrent()
            rig.session.start()
            rig.now = CompanionAudioSession.SILENCE_BEFORE_CHECK_MILLIS - 1
            rig.session.checkAdapterSession()
            assertEquals(0, rig.control.statusQueries)
            rig.now = CompanionAudioSession.SILENCE_BEFORE_CHECK_MILLIS
            rig.session.checkAdapterSession()
            assertEquals(1, rig.control.statusQueries)
            assertEquals(CompanionAudioPhase.Ended, rig.session.view.value.phase)
            assertTrue(rig.session.view.value.detail!!.contains("plugged into your controller"))
            assertEquals(0, rig.control.stops)
        }

    @Test fun `refusals and ends name the fix`() {
        assertTrue(CompanionAudioSession.refusalText(CompanionAudioState(supported = true, admit = "no_controller_link"))
            .contains("controller first"))
        assertTrue(CompanionAudioSession.endedText("source_changed").contains("Another controller"))
        assertTrue(CompanionAudioSession.geometryProblem(accepted().copy(frameBytes = 300))!!.contains("300 B"))
    }
}
