package dev.picoswitch.companion.diagnostics

import org.junit.Assert.*
import org.junit.Test

class DiagnosticLogTest {
    @Test fun `amiibo payload and addresses are redacted from exported errors`() {
        val log = DiagnosticLog()
        log.error("management", "amiibo chunk 0 DEADBEEF", IllegalStateException(
            "amiibo chunk 0 001122AABB timed out near AA:BB:CC:DD:EE:FF",
        ))
        val export = log.export(emptyMap())
        assertFalse(export.contains("001122AABB"))
        assertFalse(export.contains("AA:BB:CC:DD:EE:FF"))
        assertTrue(export.contains("data omitted"))
        assertTrue(export.contains("address omitted"))
    }

    @Test fun `ring buffer remains bounded`() {
        val log = DiagnosticLog(3)
        repeat(10) { log.event("test", "event", it.toString()) }
        assertEquals(3, log.entries.value.size)
        assertEquals("9", log.entries.value.last().detail)
    }
}
