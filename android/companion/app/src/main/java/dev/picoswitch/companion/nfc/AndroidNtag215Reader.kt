package dev.picoswitch.companion.nfc

import android.app.Activity
import android.nfc.NfcAdapter
import android.nfc.Tag
import android.nfc.tech.NfcA
import dev.picoswitch.companion.data.Ntag215Protocol
import dev.picoswitch.companion.data.Ntag215ReadResult
import dev.picoswitch.companion.data.Ntag215Rejection
import dev.picoswitch.companion.data.Ntag215Transceiver
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import android.os.Handler
import android.os.Looper

/**
 * Foreground-only, one-shot bridge from Android NfcA to [Ntag215Protocol].
 *
 * Reader mode is armed only by the Amiibo screen.  The protocol work runs on
 * an IO dispatcher; no NFC I/O or raw tag bytes are persisted by this class.
 */
class AndroidNtag215Reader(
    private val activity: Activity,
    private val onTagDetected: () -> Unit,
    private val onAccepted: (Ntag215ReadResult.Success) -> Unit,
    private val onRejected: (Ntag215Rejection) -> Unit,
    private val onReaderError: (String) -> Unit,
) : AutoCloseable {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val armed = AtomicBoolean(false)
    private val session = AtomicInteger(0)
    private val adapter: NfcAdapter? = NfcAdapter.getDefaultAdapter(activity)

    val isAvailable: Boolean get() = adapter != null

    private val callback = NfcAdapter.ReaderCallback { tag ->
        val token = session.get()
        if (!armed.compareAndSet(true, false)) return@ReaderCallback
        disableReaderModeOnly()
        onTagDetected()
        scope.launch {
            val result = read(tag)
            mainHandler.post {
                // A pause or explicit re-arm supersedes work from this tag.
                if (token != session.get()) return@post
                when (result) {
                    is Ntag215ReadResult.Success -> onAccepted(result)
                    is Ntag215ReadResult.Rejected -> onRejected(result.reason)
                }
            }
        }
    }

    /** Arm exactly one foreground reader session. Must be called from the UI. */
    fun arm(): Boolean {
        val nfc = adapter ?: return false
        if (!armed.compareAndSet(false, true)) return true
        // Supersede any still-running work from the previous one-shot tag.
        session.incrementAndGet()
        return try {
            nfc.enableReaderMode(
                activity,
                callback,
                NfcAdapter.FLAG_READER_NFC_A or
                    NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK or
                    NfcAdapter.FLAG_READER_NO_PLATFORM_SOUNDS,
                null,
            )
            true
        } catch (error: Throwable) {
            armed.set(false)
            onReaderError(error.message?.take(160) ?: "Android could not arm NFC reader mode")
            false
        }
    }

    /** Disable reader mode on pause or after a completed one-shot scan. */
    fun disable() {
        armed.set(false)
        session.incrementAndGet()
        if (Looper.myLooper() == Looper.getMainLooper()) disableReaderModeOnly()
        else mainHandler.post(::disableReaderModeOnly)
    }

    override fun close() {
        disable()
        scope.cancel()
    }

    private fun disableReaderModeOnly() {
        runCatching { adapter?.disableReaderMode(activity) }
    }

    private fun read(tag: Tag): Ntag215ReadResult {
        val nfcA = NfcA.get(tag) ?: return Ntag215ReadResult.Rejected(Ntag215Rejection.TRANSPORT_ERROR)
        return try {
            nfcA.connect()
            Ntag215Protocol.read(NfcATransceiver(nfcA))
        } catch (_: Throwable) {
            Ntag215ReadResult.Rejected(Ntag215Rejection.TRANSPORT_ERROR)
        } finally {
            runCatching { nfcA.close() }
        }
    }

    private class NfcATransceiver(private val nfcA: NfcA) : Ntag215Transceiver {
        override fun transceive(command: ByteArray): ByteArray = nfcA.transceive(command)
    }
}
