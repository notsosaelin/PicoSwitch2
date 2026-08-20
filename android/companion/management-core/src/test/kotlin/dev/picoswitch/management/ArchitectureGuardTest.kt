package dev.picoswitch.management

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files
import java.nio.file.Path

class ArchitectureGuardTest {
    @Test fun `management core contains no Android imports`() {
        val sourceRoot = Path.of("src", "main", "kotlin")
        val source = Files.walk(sourceRoot).use { paths ->
            paths.filter { Files.isRegularFile(it) && it.toString().endsWith(".kt") }
                .map(Files::readString)
                .toList()
                .joinToString("\n")
        }
        assertFalse(source.contains("import android."))
        assertFalse(source.contains("android.bluetooth"))
        assertFalse(source.contains("android.content"))
    }

    @Test fun `module is built by the plain JVM plugin`() {
        val build = Files.readString(Path.of("build.gradle.kts"))
        assertTrue(build.contains("org.jetbrains.kotlin.jvm"))
        assertFalse(build.contains("com.android"))
    }

    @Test fun `portable domain carries no app presentation labels or color packing`() {
        val domain = Files.readString(
            Path.of("src", "main", "kotlin", "dev", "picoswitch", "management", "Domain.kt"),
        )
        assertFalse(domain.contains("val title:"))
        assertFalse(domain.contains("fun argb("))
    }

    @Test fun `logical protocol file contains no BLE carrier mechanics`() {
        val protocol = Files.readString(
            Path.of("src", "main", "kotlin", "dev", "picoswitch", "management", "ManagementProtocol.kt"),
        )
        assertFalse(protocol.contains("SERVICE_UUID"))
        assertFalse(protocol.contains("commandChunks"))
        assertFalse(protocol.contains("MAX_REPLY_PAYLOAD_BYTES"))
    }
}
