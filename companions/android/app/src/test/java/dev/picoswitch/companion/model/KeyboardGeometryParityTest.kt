package dev.picoswitch.companion.model

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The Android drawn keyboard and the desktop one must be the same picture.
 *
 * [KeyboardGeometry] is a hand copy of the C# `KeyboardLayout` (two languages,
 * no shared code), so this parses the C# source and compares every key of
 * every cluster: usage, label, column, row, width, height. A key moved on one
 * platform and not the other fails here instead of shipping as a keyboard that
 * binds a different key on each companion.
 */
class KeyboardGeometryParityTest {
    private val repoRoot = File("../../..").canonicalFile
    private val source = File(
        repoRoot,
        "companions/windows/src/PicoSwitch.Companion.Services/Presentation/KeyboardLayout.cs",
    )

    private val rowConstants = mapOf(
        "FunctionRow" to 0.0, "NumberRow" to 1.25, "TopRow" to 2.25,
        "HomeRow" to 3.25, "BottomRow" to 4.25, "ModifierRow" to 5.25,
    )

    // new(0x29, "Esc", 0, FunctionRow) / new(0x57, "+", 3, TopRow, 1, 2)
    private val keyPattern = Regex(
        """new\((0x[0-9A-Fa-f]+), "((?:[^"\\]|\\.)*)", ([\w.]+), ([\w.]+)(?:, ([\d.]+))?(?:, ([\d.]+))?\)""",
    )

    private fun number(token: String): Double = rowConstants[token] ?: token.toDouble()

    /** The keys of one `public static KeyboardCluster <name> { get; } = new(...)` block. */
    private fun csharpCluster(text: String, name: String): List<KeyCap> {
        val start = text.indexOf("public static KeyboardCluster $name ")
        assertTrue("cluster $name not found in ${source.path}", start >= 0)
        val end = text.indexOf("Columns:", start)
        return keyPattern.findAll(text.substring(start, end)).map { match ->
            val (usage, label, column, row, width, height) = match.destructured
            KeyCap(
                usage = usage.removePrefix("0x").toInt(16),
                label = label.replace("\\\\", "\\"),
                column = number(column),
                row = number(row),
                width = width.ifEmpty { "1" }.toDouble(),
                height = height.ifEmpty { "1" }.toDouble(),
            )
        }.toList()
    }

    @Test fun `every cluster matches the desktop layout key for key`() {
        assertTrue("desktop layout not found at ${source.path}", source.isFile)
        val text = source.readText()

        for ((name, kotlin) in listOf(
            "Main" to KeyboardGeometry.main,
            "Navigation" to KeyboardGeometry.navigation,
            "Numpad" to KeyboardGeometry.numpad,
            "Other" to KeyboardGeometry.other,
        )) {
            val desktop = csharpCluster(text, name)
            assertFalse("parsed no keys for $name", desktop.isEmpty())
            assertEquals("cluster $name differs from KeyboardLayout.cs", desktop, kotlin.keys)
        }
    }

    @Test fun `no two keys in a cluster overlap`() {
        for (cluster in KeyboardGeometry.clusters + KeyboardGeometry.other) {
            cluster.keys.forEachIndexed { index, a ->
                cluster.keys.drop(index + 1).forEach { b ->
                    val overlap = a.column < b.column + b.width && b.column < a.column + a.width &&
                        a.row < b.row + b.height && b.row < a.row + a.height
                    assertFalse("${a.label} overlaps ${b.label} in ${cluster.name}", overlap)
                }
            }
        }
    }

    @Test fun `every drawn usage is one the firmware accepts`() {
        KeyboardGeometry.allKeys.forEach {
            assertTrue("${it.label} 0x%02X".format(it.usage), it.usage in KbmSource.KEY_USAGE_MIN..KbmSource.KEY_USAGE_MAX)
        }
    }
}
