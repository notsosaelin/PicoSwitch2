package dev.picoswitch.management.audio

import dev.picoswitch.management.CompanionAudioState
import dev.picoswitch.management.ManagementCommands
import dev.picoswitch.management.ManagementProtocol
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Mirrors the Windows CompanionMicTests; the wire contract is firmware ns2_companion_mic.h. */
class CompanionMicPlaneTest {
    @Test fun `wire geometry matches the firmware contract`() {
        assertEquals(1, CompanionMicDataPlane.VERSION)
        assertEquals(0x91, CompanionMicDataPlane.OPCODE_OPUS)
        assertEquals(160, CompanionMicDataPlane.OPUS_BYTES)
        assertEquals(164, CompanionMicDataPlane.FRAME_BYTES)
        assertEquals(0xF8, CompanionMicDataPlane.TOC)
        assertEquals(960, CompanionMicDataPlane.SAMPLES_PER_FRAME)
        assertEquals(167, CompanionMicDataPlane.MINIMUM_ATT_MTU)
    }

    @Test fun `only the exact packet shape is framed`() {
        val packet = ByteArray(160).also { it[0] = 0xF8.toByte() }
        val frame = CompanionMicDataPlane.encode(0x1234, packet)!!
        assertArrayEquals(byteArrayOf(1, 0x91.toByte(), 0x34, 0x12, 0xF8.toByte()), frame.copyOfRange(0, 5))
        assertNull(CompanionMicDataPlane.encode(0, packet, 159))
        packet[0] = 0xFC.toByte()
        assertNull(CompanionMicDataPlane.encode(0, packet))
    }

    @Test fun `caudio start mic and the mic fields`() {
        assertEquals("caudio start mic", ManagementCommands.COMPANION_AUDIO_START_MIC)
        val state = ManagementProtocol.companionAudio(
            "caudio start mic",
            """{"caudio":{"supported":true,"active":true,"mic":true,"mic_supported":true}}""",
        )
        assertTrue(state.mic)
        assertTrue(state.micSupported)
        val old = ManagementProtocol.companionAudio("caudio status", """{"caudio":{"supported":true}}""")
        assertFalse(old.mic)
        assertFalse(old.micSupported)
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class CompanionMicWriterTest {
    private class Writes(val fail: Boolean = false) {
        val pending = ArrayDeque<Pair<ByteArray, CompletableDeferred<Boolean>>>()
        val order = mutableListOf<Int>()
        var maxInFlight = 0
        suspend fun write(frame: ByteArray): Boolean {
            val done = CompletableDeferred<Boolean>()
            pending.addLast(frame to done)
            maxInFlight = maxOf(maxInFlight, pending.size)
            return done.await()
        }
        fun completeAll(scope: TestScope) {
            scope.runCurrent()
            while (pending.isNotEmpty()) {
                val (frame, done) = pending.removeFirst()
                order += frame[0].toInt()
                done.complete(!fail)
                scope.runCurrent()
            }
        }
    }

    private fun frame(id: Int) = byteArrayOf(id.toByte())

    @Test fun `order kept with one write in flight`() = runTest(StandardTestDispatcher()) {
        val writes = Writes()
        val writer = CompanionMicWriter(backgroundScope, writes::write)
        repeat(5) { writer.enqueue(frame(it)) }
        runCurrent()
        assertEquals(1, writes.pending.size)
        assertEquals(4, writer.waiting)
        writes.completeAll(this)
        assertEquals(listOf(0, 1, 2, 3, 4), writes.order)
        assertEquals(CompanionMicWriter.Counters(5, 5, 0, 0), writer.stats)
        assertEquals(1, writes.maxInFlight)
    }

    @Test fun `a backed up radio displaces the oldest waiting frame`() = runTest(StandardTestDispatcher()) {
        val writes = Writes()
        val writer = CompanionMicWriter(backgroundScope, writes::write)
        writer.enqueue(frame(0))
        runCurrent() // frame 0 in flight
        for (i in 1..CompanionMicWriter.DEPTH + 3) writer.enqueue(frame(i))
        assertEquals(CompanionMicWriter.DEPTH, writer.waiting)
        writes.completeAll(this)
        assertEquals(listOf(0, 4, 5, 6, 7, 8, 9, 10, 11), writes.order)
        assertEquals(3L, writer.stats.displaced)
    }

    @Test fun `failed writes are counted and never retried`() = runTest(StandardTestDispatcher()) {
        val writes = Writes(fail = true)
        val writer = CompanionMicWriter(backgroundScope, writes::write)
        writer.enqueue(frame(0))
        writer.enqueue(frame(1))
        writes.completeAll(this)
        assertEquals(listOf(0, 1), writes.order)
        assertEquals(2L, writer.stats.failed)
        assertEquals(0L, writer.stats.written)
    }

    @Test fun `a closed writer sends nothing more`() = runTest(StandardTestDispatcher()) {
        val writes = Writes()
        val writer = CompanionMicWriter(backgroundScope, writes::write)
        writer.enqueue(frame(0))
        runCurrent()
        writer.enqueue(frame(1))
        writer.close()
        writer.enqueue(frame(2))
        writes.completeAll(this)
        assertEquals(listOf(0), writes.order)
    }
}

/** Shared fakes for the stream and session tests. */
internal class FakeMicUplink(var available: Boolean = true) : CompanionMicUplink {
    val sent = mutableListOf<ByteArray>()
    override fun available() = available
    override suspend fun write(frame: ByteArray): Boolean { sent += frame; return true }
}

internal class FakeMicCapture(private val failStart: Boolean = false) : CompanionMicCapture {
    var onBlock: ((ShortArray) -> Unit)? = null
    var stopped = false
    override var blocksCaptured = 0L
    override fun start(onBlock: (ShortArray) -> Unit) {
        if (failStart) throw IllegalStateException("device busy")
        this.onBlock = onBlock
    }
    fun deliver(level: Short = 0) { blocksCaptured++; onBlock?.invoke(ShortArray(960) { level }) }
    override fun stop() { stopped = true }
}

internal class ScriptedMicEncoder : CompanionMicEncoder {
    var reject = false
    val peaks = mutableListOf<Int>()
    override fun encode(pcm: ShortArray): ByteArray? {
        peaks += pcm.maxOf { kotlin.math.abs(it.toInt()) }
        return if (reject) null else ByteArray(160).also { it[0] = 0xF8.toByte() }
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class CompanionMicStreamTest {
    @Test fun `sequence advances per captured block so a reject is a gap`() = runTest(StandardTestDispatcher()) {
        val uplink = FakeMicUplink()
        val capture = FakeMicCapture()
        val encoder = ScriptedMicEncoder()
        val (stream, problem) = CompanionMicStream.open(backgroundScope, uplink, capture, encoder)
        assertNull(problem)
        capture.deliver(); runCurrent()
        encoder.reject = true
        capture.deliver(); runCurrent()
        encoder.reject = false
        capture.deliver(); runCurrent()
        assertEquals(listOf(0, 2), uplink.sent.map { (it[2].toInt() and 0xFF) or ((it[3].toInt() and 0xFF) shl 8) })
        assertEquals(2L, stream!!.metrics.framesEncoded)
        assertEquals(1L, stream.metrics.encodeRejected)
        stream.close()
        capture.deliver(); runCurrent()
        assertEquals(2, uplink.sent.size)
        assertTrue(capture.stopped)
    }

    @Test fun `mute sends silence on the same cadence and sequence`() = runTest(StandardTestDispatcher()) {
        val uplink = FakeMicUplink()
        val capture = FakeMicCapture()
        val encoder = ScriptedMicEncoder()
        val stream = CompanionMicStream.open(backgroundScope, uplink, capture, encoder).first!!
        capture.deliver(1000); runCurrent()
        stream.muted = true
        capture.deliver(1000); runCurrent()
        stream.muted = false
        capture.deliver(1000); runCurrent()
        assertEquals(listOf(1000, 0, 1000), encoder.peaks)
        assertEquals(listOf(0, 1, 2), uplink.sent.map { (it[2].toInt() and 0xFF) or ((it[3].toInt() and 0xFF) shl 8) })
    }

    @Test fun `failures leave nothing open and say why`() = runTest(StandardTestDispatcher()) {
        assertTrue(CompanionMicStream.open(backgroundScope, FakeMicUplink(available = false), FakeMicCapture(), ScriptedMicEncoder())
            .second!!.contains("does not accept a microphone"))
        assertTrue(CompanionMicStream.open(backgroundScope, FakeMicUplink(), null, ScriptedMicEncoder())
            .second!!.contains("No microphone"))
        val busy = FakeMicCapture(failStart = true)
        val (stream, problem) = CompanionMicStream.open(backgroundScope, FakeMicUplink(), busy, ScriptedMicEncoder())
        assertNull(stream)
        assertTrue(problem!!.contains("Could not open the microphone"))
        assertTrue(busy.stopped)
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
class CompanionAudioSessionMicTest {
    private fun accepted() = CompanionAudioState(
        supported = true, active = true, usbSpeakerActive = false, version = 1, codec = "opus",
        sampleRate = 48_000, channels = 2, frameMillis = 20, frameBytes = 244, attMtu = 517,
        minimumAttMtu = 247, admit = "ok", kbps = 96,
    )

    private class Control(val reply: CompanionAudioState, val micReply: CompanionAudioState?) : CompanionAudioControl {
        val requests = mutableListOf<Boolean>()
        var stops = 0
        override suspend fun start(microphone: Boolean): CompanionAudioState {
            requests += microphone
            return if (microphone) micReply ?: reply.copy(mic = true) else reply
        }
        override suspend fun stop(): CompanionAudioState { stops++; return CompanionAudioState(supported = true) }
        override suspend fun status() = reply
    }

    private class Downlink : CompanionAudioDownlink {
        var onFrame: ((ByteArray) -> Unit)? = null
        var closed = false
        override val attMtu = 517
        override suspend fun open(onFrame: (ByteArray) -> Unit): Boolean { this.onFrame = onFrame; return true }
        override suspend fun close() { closed = true }
    }

    private class Sink : CompanionAudioSink {
        override fun start(sampleRate: Int, channels: Int) = Unit
        override fun write(pcm: ShortArray, samples: Int) = true
        override fun stop() = Unit
        override val underruns = 0L
        override val prebufferMillis = 120
        override fun takeConcealRequest() = false
    }

    private object Decoder : CompanionAudioDecoder {
        override fun decode(opus: ByteArray?, pcm: ShortArray) = CompanionAudioDataPlane.SAMPLES_PER_CHANNEL
        override fun reset() = Unit
    }

    private inner class Rig(
        scope: TestScope,
        micReply: CompanionAudioState? = null,
        val capture: FakeMicCapture? = FakeMicCapture(),
        val uplink: FakeMicUplink = FakeMicUplink(),
    ) {
        val ready = MutableStateFlow(true)
        val link = MutableStateFlow(true)
        val control = Control(accepted(), micReply)
        val downlink = Downlink()
        val session = CompanionAudioSession(
            scope = scope.backgroundScope,
            control = control,
            downlinks = { downlink },
            sinks = { Sink() },
            decoders = { Decoder },
            managementReady = ready,
            controllerLink = link,
            runWatchdog = false,
            micUplinks = { uplink },
            micCaptures = { capture },
            micEncoders = { ScriptedMicEncoder() },
        )
    }

    private fun audioFrame() = ByteArray(244).also { it[0] = 1; it[1] = 0x90.toByte(); it[2] = 1; it[4] = 0xFC.toByte() }

    @Test fun `with the microphone on the session starts as a headset and streams up`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.setMicrophone(true)
        rig.session.start()
        assertEquals(listOf(true), rig.control.requests)
        assertTrue(rig.session.view.value.microphone)
        rig.capture!!.deliver(); rig.capture.deliver(); runCurrent()
        assertEquals(2, rig.uplink.sent.size)
        assertEquals(CompanionMicDataPlane.FRAME_BYTES, rig.uplink.sent[0].size)
        rig.session.stop()
        assertTrue(rig.capture.stopped)
        assertFalse(rig.session.view.value.microphone)
    }

    @Test fun `mute never re-plugs and survives into the stream`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.setMuted(true)
        rig.session.setMicrophone(true)
        rig.session.start()
        assertTrue(rig.session.view.value.muted)
        rig.session.setMuted(false)
        rig.session.setMuted(true)
        // One start as a headset; mute itself sent no adapter command.
        assertEquals(listOf(true), rig.control.requests)
        assertTrue(rig.session.view.value.microphone)
        assertTrue(rig.session.view.value.muted)
        rig.capture!!.deliver(); runCurrent()
        assertEquals(1, rig.uplink.sent.size)
    }

    @Test fun `a plugged-in headset re-plugs when Controller Link returns`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.setMicrophone(true)
        rig.session.start()
        assertEquals(CompanionAudioPhase.Playing, rig.session.view.value.phase)
        rig.link.value = false
        runCurrent()
        assertEquals(CompanionAudioPhase.Ended, rig.session.view.value.phase)
        rig.link.value = true
        runCurrent()
        assertEquals(CompanionAudioPhase.Playing, rig.session.view.value.phase)
        assertTrue(rig.session.view.value.microphone)
        assertEquals(listOf(true, true), rig.control.requests)
    }

    @Test fun `an unplugged headset stays unplugged across a link cycle`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.start()
        rig.session.stop()
        rig.link.value = false
        runCurrent()
        rig.link.value = true
        runCurrent()
        assertEquals(listOf(false), rig.control.requests)
        assertFalse(rig.session.view.value.on)
    }

    @Test fun `without the microphone nothing is captured`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.start()
        assertEquals(listOf(false), rig.control.requests)
        assertNull(rig.capture!!.onBlock)
    }

    @Test fun `an adapter that cannot decode the microphone still plays`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this, micReply = CompanionAudioState(supported = true, active = false, admit = "mic_unsupported"))
        runCurrent()
        rig.session.setMicrophone(true)
        rig.session.start()
        assertEquals(listOf(true, false), rig.control.requests)
        assertEquals(CompanionAudioPhase.Playing, rig.session.view.value.phase)
        assertFalse(rig.session.view.value.microphone)
        assertTrue(rig.session.view.value.microphoneDetail!!.contains("cannot take a microphone"))
    }

    @Test fun `no microphone device replugs as headphones and keeps playing`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this, capture = null)
        runCurrent()
        rig.session.setMicrophone(true)
        rig.session.start()
        assertEquals(listOf(true, false), rig.control.requests)
        assertEquals(CompanionAudioPhase.Playing, rig.session.view.value.phase)
        assertTrue(rig.session.view.value.microphoneDetail!!.contains("No microphone"))
    }

    @Test fun `toggling while playing replugs without ending the session`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.start()
        rig.session.setMicrophone(true)
        assertTrue(rig.session.view.value.microphone)
        rig.session.setMicrophone(false)
        assertFalse(rig.session.view.value.microphone)
        assertTrue(rig.capture!!.stopped)
        assertEquals(listOf(false, true, false), rig.control.requests)
        assertEquals(CompanionAudioPhase.Playing, rig.session.view.value.phase)
        assertFalse(rig.downlink.closed)
        assertEquals(0, rig.control.stops)
    }

    @Test fun `a running session that refuses the microphone stays headphones`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this, micReply = accepted().copy(admit = "mic_unsupported", mic = false))
        runCurrent()
        rig.session.start()
        rig.session.setMicrophone(true)
        assertFalse(rig.session.view.value.microphone)
        assertNull(rig.capture!!.onBlock)
        assertNotNull(rig.session.view.value.microphoneDetail)
    }

    @Test fun `management loss closes the microphone`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.setMicrophone(true)
        rig.session.start()
        rig.ready.value = false
        runCurrent()
        assertEquals(CompanionAudioPhase.Unavailable, rig.session.view.value.phase)
        assertTrue(rig.capture!!.stopped)
    }

    @Test fun `the first console frame keeps the microphone state`() = runTest(StandardTestDispatcher()) {
        val rig = Rig(this)
        runCurrent()
        rig.session.setMicrophone(true)
        rig.session.start()
        assertTrue(rig.session.view.value.awaitingConsole)
        rig.downlink.onFrame!!(audioFrame())
        assertFalse(rig.session.view.value.awaitingConsole)
        assertTrue(rig.session.view.value.microphone)
    }
}
