package dev.picoswitch.companion.audio

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import androidx.core.content.ContextCompat
import dev.picoswitch.companion.protocol.ManagementTransport
import dev.picoswitch.management.audio.CompanionMicCapture
import dev.picoswitch.management.audio.CompanionMicDataPlane
import dev.picoswitch.management.audio.CompanionMicEncoder
import dev.picoswitch.management.audio.CompanionMicUplink
import io.github.jaredmdobson.concentus.OpusApplication
import io.github.jaredmdobson.concentus.OpusBandwidth
import io.github.jaredmdobson.concentus.OpusEncoder
import java.util.concurrent.atomic.AtomicLong

/**
 * The microphone uplink's encoder, via Concentus (the encoder the Windows
 * companion uses in its .NET form): restricted-low-delay (CELT only), 48 kHz
 * mono, 64 kbit/s CBR, full band, 20 ms, complexity 10. Every packet is exactly
 * 160 bytes with TOC `0xF8`, the only shape the adapter decodes.
 */
class ConcentusCompanionMicEncoder : CompanionMicEncoder {
    private val encoder = OpusEncoder(
        CompanionMicDataPlane.SAMPLE_RATE,
        CompanionMicDataPlane.CHANNELS,
        OpusApplication.OPUS_APPLICATION_RESTRICTED_LOWDELAY,
    ).apply {
        bitrate = CompanionMicDataPlane.KBPS * 1000
        useVBR = false
        complexity = 10
        bandwidth = OpusBandwidth.OPUS_BANDWIDTH_FULLBAND
    }
    private val packet = ByteArray(1275)

    override fun encode(pcm: ShortArray): ByteArray? {
        val n = runCatching {
            encoder.encode(pcm, 0, CompanionMicDataPlane.SAMPLES_PER_FRAME, packet, 0, packet.size)
        }.getOrDefault(-1)
        return if (n > 0 && CompanionMicDataPlane.isValidPacket(packet, n)) packet.copyOf(n) else null
    }
}

/**
 * Captures the microphone with [AudioRecord] at 48 kHz mono 16-bit, in whole
 * 20 ms blocks on its own thread.
 *
 * Source VOICE_COMMUNICATION, with the platform's [AcousticEchoCanceler] when
 * the device offers one: on a handheld the console's audio plays from the same
 * device's speakers, and without cancellation the console's chat would hear
 * itself. Whether a given device's AEC cancels this app's own AudioTrack
 * output is device-specific (HYPOTHESIS until measured); headphones avoid the
 * question entirely, as with a real headset. Falls back to the plain MIC
 * source on a device that will not open VOICE_COMMUNICATION at 48 kHz.
 */
class AudioRecordCompanionMicCapture(private val context: Context) : CompanionMicCapture {
    private val captured = AtomicLong()
    @Volatile private var running = false
    private var record: AudioRecord? = null
    private var echo: AcousticEchoCanceler? = null
    private var thread: Thread? = null

    override val blocksCaptured: Long get() = captured.get()

    @SuppressLint("MissingPermission") // checked explicitly below
    override fun start(onBlock: (ShortArray) -> Unit) {
        if (record != null) return
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            throw SecurityException("microphone permission is not granted")
        }
        val bytesPerBlock = CompanionMicDataPlane.SAMPLES_PER_FRAME * 2
        val minimum = AudioRecord.getMinBufferSize(
            CompanionMicDataPlane.SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
        )
        // Four blocks of slack against a late capture thread, as on Windows.
        val buffer = maxOf(minimum, bytesPerBlock * 4)
        val opened = open(MediaRecorder.AudioSource.VOICE_COMMUNICATION, buffer)
            ?: open(MediaRecorder.AudioSource.MIC, buffer)
            ?: throw IllegalStateException("the microphone would not open at 48 kHz")
        record = opened
        echo = if (AcousticEchoCanceler.isAvailable()) {
            runCatching { AcousticEchoCanceler.create(opened.audioSessionId)?.apply { enabled = true } }.getOrNull()
        } else {
            null
        }
        opened.startRecording()
        running = true
        thread = Thread({ run(opened, onBlock) }, "companion-mic").apply {
            priority = Thread.MAX_PRIORITY
            isDaemon = true
            start()
        }
    }

    @SuppressLint("MissingPermission")
    private fun open(source: Int, bufferBytes: Int): AudioRecord? {
        val candidate = runCatching {
            AudioRecord(
                source,
                CompanionMicDataPlane.SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferBytes,
            )
        }.getOrNull() ?: return null
        if (candidate.state != AudioRecord.STATE_INITIALIZED) {
            candidate.release()
            return null
        }
        return candidate
    }

    private fun run(input: AudioRecord, onBlock: (ShortArray) -> Unit) {
        while (running) {
            val block = ShortArray(CompanionMicDataPlane.SAMPLES_PER_FRAME)
            var filled = 0
            while (running && filled < block.size) {
                val n = input.read(block, filled, block.size - filled, AudioRecord.READ_BLOCKING)
                if (n <= 0) {
                    running = false
                    break
                }
                filled += n
            }
            if (filled != block.size) break
            captured.incrementAndGet()
            try {
                onBlock(block)
            } catch (_: Exception) {
                // A consumer fault must not stop the device.
            }
        }
    }

    override fun stop() {
        running = false
        val input = record
        record = null
        runCatching { input?.stop() } // unblocks a blocking read
        thread?.join(1000)
        thread = null
        echo?.release()
        echo = null
        input?.release()
    }
}

/** The MIC-UP characteristic on this app's management link. */
class TransportCompanionMicUplink(private val transport: ManagementTransport) : CompanionMicUplink {
    override fun available(): Boolean = transport.companionMicAvailable

    override suspend fun write(frame: ByteArray): Boolean = transport.writeCompanionMic(frame)
}
