package dev.picoswitch.companion.diagnostics

import android.util.Log
import dev.picoswitch.companion.BuildConfig
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

data class DiagnosticEntry(val timeMillis: Long, val area: String, val event: String, val detail: String = "")

data class DiagnosticSummary(
    val lastCommand: String = "None",
    val lastResult: String = "None",
    val lastError: String = "None",
)

/** Bounded in-memory development log. It stores event classes, never raw Amiibo bytes or JSON replies. */
class DiagnosticLog(private val capacity: Int = 400) {
    private val _entries = MutableStateFlow<List<DiagnosticEntry>>(emptyList())
    val entries: StateFlow<List<DiagnosticEntry>> = _entries.asStateFlow()
    private val _summary = MutableStateFlow(DiagnosticSummary())
    val summary: StateFlow<DiagnosticSummary> = _summary.asStateFlow()

    @Synchronized
    fun event(area: String, event: String, detail: String = "") {
        val safe = detail.replace(Regex("[\\r\\n]+"), " ").take(240)
        _entries.value = (_entries.value + DiagnosticEntry(System.currentTimeMillis(), area, event, safe)).takeLast(capacity)
        mirrorToLogcat(area, event, safe)
    }

    fun commandStarted(command: String) {
        val type = commandType(command)
        _summary.value = _summary.value.copy(lastCommand = type)
        event("management", "command", type)
    }

    fun commandFinished(command: String, replyBytes: Int) {
        val result = "${commandType(command)}: complete ($replyBytes bytes)"
        _summary.value = _summary.value.copy(lastResult = result)
        event("management", "result", result)
    }

    fun error(area: String, operation: String, error: Throwable) {
        val raw = error.message ?: error.javaClass.simpleName
        val redacted = raw
            .replace(Regex("amiibo chunk\\s+\\d+\\s+[0-9A-Fa-f]+", RegexOption.IGNORE_CASE), "amiibo chunk [data omitted]")
            .replace(Regex("amiibo begin\\s+\\d+\\s+[0-9A-Fa-f]+", RegexOption.IGNORE_CASE), "amiibo begin [checksum omitted]")
            .replace(Regex("(?i)\\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\\b"), "[address omitted]")
            .replace(Regex("[\\r\\n\\p{Cntrl}]+"), " ")
        val detail = "${commandType(operation).take(80)}: ${redacted.take(150)}"
        _summary.value = _summary.value.copy(lastError = detail)
        event(area, "error", detail)
    }

    fun export(header: Map<String, String>): String = buildString {
        appendLine("PicoSwitch Companion diagnostic report")
        appendLine("Privacy: no raw Amiibo bytes, management JSON, keys, or Bluetooth addresses are included.")
        header.forEach { (key, value) -> appendLine("$key: ${value.replace(Regex("[\\r\\n]+"), " ")}") }
        appendLine()
        appendLine("Events (UTC)")
        _entries.value.forEach { entry ->
            append(timestamp(entry.timeMillis)).append(' ')
                .append(entry.area).append('/').append(entry.event)
            if (entry.detail.isNotBlank()) append(": ").append(entry.detail)
            appendLine()
        }
    }

    /**
     * Mirror to logcat so the companion is observable over ADB during hardware
     * bring-up. The in-memory ring is only visible on the phone's own screen,
     * which makes bridge behaviour (motion requests, player LED, rumble, report
     * counts) impossible to verify from a workstation. Debug builds only, and it
     * reuses the already-redacted text so nothing sensitive reaches the system log.
     */
    private fun mirrorToLogcat(area: String, event: String, detail: String) {
        if (!BuildConfig.DEBUG) return
        val line = if (detail.isBlank()) "$area/$event" else "$area/$event: $detail"
        Log.d(LOG_TAG, line)
    }

    companion object {
        /** Single tag so `adb logcat -s PicoSwitch` captures the whole bridge. */
        const val LOG_TAG = "PicoSwitch"

        fun commandType(command: String): String = when {
            command.startsWith("amiibo chunk ") -> "amiibo chunk [data omitted]"
            command.startsWith("amiibo begin ") -> "amiibo begin [checksum omitted]"
            command.startsWith("amiibo read ") -> "amiibo read"
            command.startsWith("bonds remove ") -> "bonds remove"
            command.startsWith("body ") || command.startsWith("jcl ") || command.startsWith("jcr ") -> command.substringBefore(' ') + " color"
            command.startsWith("personality ") -> "personality set"
            command.startsWith("mgmt ") -> "mgmt set/status"
            else -> command.substringBefore(' ').take(40)
        }

        private fun timestamp(value: Long): String = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply {
            timeZone = TimeZone.getTimeZone("UTC")
        }.format(Date(value))
    }
}
