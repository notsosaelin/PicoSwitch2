package dev.picoswitch.bridge.core

import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import java.io.File
import org.junit.Test

/**
 * Bridge Core must stay platform-neutral.
 *
 * The primary guard is the build graph: `:bridge-core` is a plain Kotlin/JVM
 * module with no Android dependency, so an Android type simply does not compile
 * here. This test exists for the failures a classpath cannot catch — a platform
 * name that creeps into an identifier, a string, or a diagnostic message, which
 * is how "shared" models quietly become "whatever the first platform did".
 *
 * It reads the module's own sources. That is unusual for a unit test and
 * deliberate: the alternative is a lint rule or a custom Gradle task, and § of
 * the architecture brief is explicit that elaborate tooling is not wanted here.
 */
class ArchitectureGuardTest {

    /** Gradle runs tests with the module directory as the working directory. */
    private fun sources(): List<File> {
        val root = sequenceOf(
            File("src/main/kotlin"),
            File("bridge-core/src/main/kotlin"),
        ).firstOrNull { it.isDirectory }
        if (root == null) {
            fail("bridge-core sources not found from ${File(".").absolutePath}")
            return emptyList()
        }
        return root.walkTopDown().filter { it.isFile && it.extension == "kt" }.toList()
    }

    @Test fun `the guard actually sees the sources it claims to check`() {
        // A guard that silently scans nothing passes forever.
        assertTrue("no Kotlin sources were scanned", sources().size >= 8)
    }

    @Test fun `bridge core has no platform imports`() {
        val offenders = mutableListOf<String>()
        sources().forEach { file ->
            file.readLines().forEachIndexed { index, line ->
                val trimmed = line.trim()
                if (!trimmed.startsWith("import ")) return@forEachIndexed
                val imported = trimmed.removePrefix("import ").substringBefore(" as ")
                if (FORBIDDEN_IMPORT_ROOTS.any { imported.startsWith(it) }) {
                    offenders += "${file.name}:${index + 1}  $trimmed"
                }
            }
        }
        assertTrue(
            "Bridge Core must not import platform APIs. A platform detail belongs in a backend:\n" +
                offenders.joinToString("\n"),
            offenders.isEmpty(),
        )
    }

    /**
     * Platform vocabulary in CODE (identifiers, string literals) means the shared
     * model has taken a position on one host's way of doing things. Prose is
     * exempt: naming the platform an observation came from is evidence, and
     * deleting it would cost more than it protects.
     */
    @Test fun `bridge core code names no host platform`() {
        val offenders = mutableListOf<String>()
        sources().forEach { file ->
            var inBlockComment = false
            file.readLines().forEachIndexed { index, line ->
                val trimmed = line.trim()
                if (inBlockComment) {
                    if (trimmed.contains("*/")) inBlockComment = false
                    return@forEachIndexed
                }
                if (trimmed.startsWith("/*")) {
                    if (!trimmed.contains("*/")) inBlockComment = true
                    return@forEachIndexed
                }
                if (trimmed.startsWith("*") || trimmed.startsWith("//")) return@forEachIndexed
                val code = trimmed.substringBefore("//")
                FORBIDDEN_CODE_WORDS.forEach { word ->
                    if (code.contains(word, ignoreCase = true)) {
                        offenders += "${file.name}:${index + 1}  [$word]  $trimmed"
                    }
                }
            }
        }
        assertTrue(
            "Bridge Core code must not name a host platform. Move the platform-specific\n" +
                "concept into a backend, or describe it in bridge terms:\n" +
                offenders.joinToString("\n"),
            offenders.isEmpty(),
        )
    }

    private companion object {
        val FORBIDDEN_IMPORT_ROOTS = listOf(
            "android.", "androidx.", "com.android.",
            "java.awt", "javax.swing",
            "com.sun.jna", "winapi",
        )

        /**
         * Deliberately short. Each entry is a platform whose vocabulary has
         * actually leaked into this codebase before, or is the obvious next one.
         */
        val FORBIDDEN_CODE_WORDS = listOf("Android", "Vibrator", "InputDevice", "KEYCODE_", "Surface.")
    }
}
