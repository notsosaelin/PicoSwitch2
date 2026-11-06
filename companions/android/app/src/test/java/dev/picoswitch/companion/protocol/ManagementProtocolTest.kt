package dev.picoswitch.companion.protocol

import dev.picoswitch.management.BleManagementContract
import dev.picoswitch.management.ManagementProtocol
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files
import java.nio.file.Path

/** Guards Android's role as a consumer, not an authority, of management. */
class ManagementProtocolTest {
    @Test fun `app consumes core logical framing and BLE contract`() {
        assertArrayEquals("ping\n".encodeToByteArray(), ManagementProtocol.frame("ping"))
        assertTrue(BleManagementContract.SERVICE_UUID.startsWith("7c5ad4ed"))
    }

    @Test fun `repository and UI contain no raw command or JSON parsing authority`() {
        val roots = listOf(
            Path.of("src", "main", "java", "dev", "picoswitch", "companion", "data", "AdapterRepository.kt"),
            Path.of("src", "main", "java", "dev", "picoswitch", "companion", "ui", "CompanionViewModel.kt"),
        )
        val source = roots.joinToString("\n") { String(Files.readAllBytes(it)) }
        assertFalse(source.contains("kotlinx.serialization.json"))
        assertFalse(source.contains("transport.transact("))
        assertFalse(source.contains("ManagementCommands."))
        assertFalse(source.contains("ManagementProtocol.parse"))
    }

    @Test fun `Android GATT backend delegates serialization and constants`() {
        val source = String(Files.readAllBytes(
            Path.of("src", "main", "java", "dev", "picoswitch", "companion", "transport", "BleGattManagementTransport.kt"),
        ))
        assertTrue(source.contains("SerializedManagementSession"))
        assertTrue(source.contains("BleReplyAssembler"))
        assertTrue(source.contains("BleManagementContract.SERVICE_UUID"))
        assertTrue(source.contains("notifications.trySend(ByteArray(0))"))
        assertFalse(source.contains("7c5ad4ed-2731-417c-b316-058505c7c083"))
    }

    /**
     * A command must not be timed on the main thread.
     *
     * THE DEFECT THIS GUARDS. Callers reach the transport from viewModelScope,
     * whose dispatcher is Main. Every fragment's write callback and every
     * notification of the reply then resumes there, dozens of main-thread
     * dispatches inside one command's timeout budget — so the timeout became a
     * measure of how busy the UI was. A busy library browser could push a chunk
     * past it, and the timeout path invalidates the session, which took the
     * adapter connection down mid-upload. Observed stalling at offsets 64, 96
     * and 288 of the same file: arbitrary, because the cause was scheduling.
     *
     * Asserted against the source because the alternative is instrumenting a
     * GATT stack to prove which thread a continuation resumed on. This is a
     * cheap guard on a property that is easy to remove by accident and
     * expensive to rediscover.
     */
    @Test fun `management transactions do not run on the main dispatcher`() {
        val source = String(Files.readAllBytes(
            Path.of("src", "main", "java", "dev", "picoswitch", "companion", "transport", "BleGattManagementTransport.kt"),
        ))
        assertTrue(
            "transact must move off the caller's dispatcher before exchanging",
            source.contains("withContext(Dispatchers.IO) { session.exchange {"),
        )
    }
}
