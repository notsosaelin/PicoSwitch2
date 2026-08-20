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
        assertTrue(source.contains("BleManagementContract.SERVICE_UUID"))
        assertFalse(source.contains("7c5ad4ed-2731-417c-b316-058505c7c083"))
    }
}
