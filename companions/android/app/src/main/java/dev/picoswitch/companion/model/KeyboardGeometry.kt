package dev.picoswitch.companion.model

/**
 * One key, positioned in key units within its cluster.
 *
 * [usage] is the HID Keyboard/Keypad (page 0x07) usage the key binds; the
 * geometry is only ever a way to POINT at a usage, because the wire carries
 * no physical layout.
 */
data class KeyCap(
    val usage: Int,
    val label: String,
    val column: Double,
    val row: Double,
    val width: Double = 1.0,
    val height: Double = 1.0,
)

/** One physical group of keys, drawn as a unit. */
data class KeyboardCluster(
    val name: String,
    val keys: List<KeyCap>,
    val columns: Double,
    val rows: Double,
)

/**
 * The keyboard the Keyboard & Mouse page draws: full-size ANSI 104, the
 * superset every smaller board's usages fit in, plus an "Other" group for the
 * ISO and Japanese keys an ANSI picture has no honest position for.
 *
 * A KOTLIN COPY of the desktop companions' layout,
 * `companions/windows/src/PicoSwitch.Companion.Services/Presentation/KeyboardLayout.cs`,
 * which carries the reasoning (why ANSI, why geometry rather than rows). The
 * two cannot share code across languages, so `KeyboardGeometryParityTest`
 * parses that file and fails if a key's usage, label, position or size
 * differs here. Change both together.
 */
object KeyboardGeometry {
    // Row origins for the main block; the nav cluster and numpad reuse them so
    // the three clusters line up. The 0.25 gap under the function row is real.
    private const val FunctionRow = 0.0
    private const val NumberRow = 1.25
    private const val TopRow = 2.25
    private const val HomeRow = 3.25
    private const val BottomRow = 4.25
    private const val ModifierRow = 5.25

    val main = KeyboardCluster(
        "Main",
        listOf(
            KeyCap(0x29, "Esc", 0.0, FunctionRow),
            KeyCap(0x3A, "F1", 2.0, FunctionRow), KeyCap(0x3B, "F2", 3.0, FunctionRow),
            KeyCap(0x3C, "F3", 4.0, FunctionRow), KeyCap(0x3D, "F4", 5.0, FunctionRow),
            KeyCap(0x3E, "F5", 6.5, FunctionRow), KeyCap(0x3F, "F6", 7.5, FunctionRow),
            KeyCap(0x40, "F7", 8.5, FunctionRow), KeyCap(0x41, "F8", 9.5, FunctionRow),
            KeyCap(0x42, "F9", 11.0, FunctionRow), KeyCap(0x43, "F10", 12.0, FunctionRow),
            KeyCap(0x44, "F11", 13.0, FunctionRow), KeyCap(0x45, "F12", 14.0, FunctionRow),

            KeyCap(0x35, "`", 0.0, NumberRow),
            KeyCap(0x1E, "1", 1.0, NumberRow), KeyCap(0x1F, "2", 2.0, NumberRow),
            KeyCap(0x20, "3", 3.0, NumberRow), KeyCap(0x21, "4", 4.0, NumberRow),
            KeyCap(0x22, "5", 5.0, NumberRow), KeyCap(0x23, "6", 6.0, NumberRow),
            KeyCap(0x24, "7", 7.0, NumberRow), KeyCap(0x25, "8", 8.0, NumberRow),
            KeyCap(0x26, "9", 9.0, NumberRow), KeyCap(0x27, "0", 10.0, NumberRow),
            KeyCap(0x2D, "-", 11.0, NumberRow), KeyCap(0x2E, "=", 12.0, NumberRow),
            KeyCap(0x2A, "Backspace", 13.0, NumberRow, 2.0),

            KeyCap(0x2B, "Tab", 0.0, TopRow, 1.5),
            KeyCap(0x14, "Q", 1.5, TopRow), KeyCap(0x1A, "W", 2.5, TopRow),
            KeyCap(0x08, "E", 3.5, TopRow), KeyCap(0x15, "R", 4.5, TopRow),
            KeyCap(0x17, "T", 5.5, TopRow), KeyCap(0x1C, "Y", 6.5, TopRow),
            KeyCap(0x18, "U", 7.5, TopRow), KeyCap(0x0C, "I", 8.5, TopRow),
            KeyCap(0x12, "O", 9.5, TopRow), KeyCap(0x13, "P", 10.5, TopRow),
            KeyCap(0x2F, "[", 11.5, TopRow), KeyCap(0x30, "]", 12.5, TopRow),
            KeyCap(0x31, "\\", 13.5, TopRow, 1.5),

            KeyCap(0x39, "Caps", 0.0, HomeRow, 1.75),
            KeyCap(0x04, "A", 1.75, HomeRow), KeyCap(0x16, "S", 2.75, HomeRow),
            KeyCap(0x07, "D", 3.75, HomeRow), KeyCap(0x09, "F", 4.75, HomeRow),
            KeyCap(0x0A, "G", 5.75, HomeRow), KeyCap(0x0B, "H", 6.75, HomeRow),
            KeyCap(0x0D, "J", 7.75, HomeRow), KeyCap(0x0E, "K", 8.75, HomeRow),
            KeyCap(0x0F, "L", 9.75, HomeRow), KeyCap(0x33, ";", 10.75, HomeRow),
            KeyCap(0x34, "'", 11.75, HomeRow),
            KeyCap(0x28, "Enter", 12.75, HomeRow, 2.25),

            KeyCap(0xE1, "Shift", 0.0, BottomRow, 2.25),
            KeyCap(0x1D, "Z", 2.25, BottomRow), KeyCap(0x1B, "X", 3.25, BottomRow),
            KeyCap(0x06, "C", 4.25, BottomRow), KeyCap(0x19, "V", 5.25, BottomRow),
            KeyCap(0x05, "B", 6.25, BottomRow), KeyCap(0x11, "N", 7.25, BottomRow),
            KeyCap(0x10, "M", 8.25, BottomRow), KeyCap(0x36, ",", 9.25, BottomRow),
            KeyCap(0x37, ".", 10.25, BottomRow), KeyCap(0x38, "/", 11.25, BottomRow),
            KeyCap(0xE5, "Shift", 12.25, BottomRow, 2.75),

            KeyCap(0xE0, "Ctrl", 0.0, ModifierRow, 1.25),
            KeyCap(0xE3, "Win", 1.25, ModifierRow, 1.25),
            KeyCap(0xE2, "Alt", 2.5, ModifierRow, 1.25),
            KeyCap(0x2C, "Space", 3.75, ModifierRow, 6.25),
            KeyCap(0xE6, "Alt", 10.0, ModifierRow, 1.25),
            KeyCap(0xE7, "Win", 11.25, ModifierRow, 1.25),
            KeyCap(0x65, "Menu", 12.5, ModifierRow, 1.25),
            KeyCap(0xE4, "Ctrl", 13.75, ModifierRow, 1.25),
        ),
        columns = 15.0,
        rows = 6.25,
    )

    val navigation = KeyboardCluster(
        "Navigation",
        listOf(
            KeyCap(0x46, "PrtSc", 0.0, FunctionRow), KeyCap(0x47, "ScrLk", 1.0, FunctionRow),
            KeyCap(0x48, "Pause", 2.0, FunctionRow),

            KeyCap(0x49, "Ins", 0.0, NumberRow), KeyCap(0x4A, "Home", 1.0, NumberRow),
            KeyCap(0x4B, "PgUp", 2.0, NumberRow),
            KeyCap(0x4C, "Del", 0.0, TopRow), KeyCap(0x4D, "End", 1.0, TopRow),
            KeyCap(0x4E, "PgDn", 2.0, TopRow),

            KeyCap(0x52, "Up", 1.0, BottomRow),
            KeyCap(0x50, "Left", 0.0, ModifierRow), KeyCap(0x51, "Down", 1.0, ModifierRow),
            KeyCap(0x4F, "Right", 2.0, ModifierRow),
        ),
        columns = 3.0,
        rows = 6.25,
    )

    val numpad = KeyboardCluster(
        "Numpad",
        listOf(
            KeyCap(0x53, "Num", 0.0, NumberRow), KeyCap(0x54, "/", 1.0, NumberRow),
            KeyCap(0x55, "*", 2.0, NumberRow), KeyCap(0x56, "-", 3.0, NumberRow),

            KeyCap(0x5F, "7", 0.0, TopRow), KeyCap(0x60, "8", 1.0, TopRow),
            KeyCap(0x61, "9", 2.0, TopRow),
            KeyCap(0x57, "+", 3.0, TopRow, 1.0, 2.0),

            KeyCap(0x5C, "4", 0.0, HomeRow), KeyCap(0x5D, "5", 1.0, HomeRow),
            KeyCap(0x5E, "6", 2.0, HomeRow),

            KeyCap(0x59, "1", 0.0, BottomRow), KeyCap(0x5A, "2", 1.0, BottomRow),
            KeyCap(0x5B, "3", 2.0, BottomRow),
            KeyCap(0x58, "Enter", 3.0, BottomRow, 1.0, 2.0),

            KeyCap(0x62, "0", 0.0, ModifierRow, 2.0), KeyCap(0x63, ".", 2.0, ModifierRow),
        ),
        columns = 4.0,
        rows = 6.25,
    )

    /** Keys real keyboards have that an ANSI picture has no position for. Listed, never placed. */
    val other = KeyboardCluster(
        "Other",
        listOf(
            KeyCap(0x64, "ISO \\", 0.0, 0.0, 1.5), KeyCap(0x32, "ISO #", 1.5, 0.0, 1.5),
            KeyCap(0x87, "Ro", 3.0, 0.0, 1.5), KeyCap(0x89, "Yen", 4.5, 0.0, 1.5),
            KeyCap(0x88, "Kana", 6.0, 0.0, 1.5), KeyCap(0x8A, "Henkan", 7.5, 0.0, 1.5),
            KeyCap(0x8B, "Muhenkan", 9.0, 0.0, 1.5),
        ),
        columns = 10.5,
        rows = 1.0,
    )

    /** The three aligned clusters, left to right. */
    val clusters: List<KeyboardCluster> = listOf(main, navigation, numpad)

    /** Every key drawn anywhere, including [other]. */
    val allKeys: List<KeyCap> = clusters.flatMap { it.keys } + other.keys

    /** Usages that have a key on screen; anything else bound goes in a plain list. */
    val drawnUsages: Set<Int> = allKeys.map { it.usage }.toSet()
}
