using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// One key, positioned in key units within its cluster.
/// </summary>
/// <param name="Usage">The HID keyboard usage id this key sends.</param>
/// <param name="Label">What is printed on it.</param>
/// <param name="Column">Distance from the cluster's left edge, in key units.</param>
/// <param name="Row">Distance from the cluster's top edge, in key units.</param>
/// <param name="Width">Width in key units, so a Shift is visibly a Shift.</param>
/// <param name="Height">Height in key units. Two on a numpad Plus and Enter.</param>
public sealed record KeyCap(
    int Usage,
    string Label,
    double Column,
    double Row,
    double Width = 1.0,
    double Height = 1.0)
{
    public KbmSource Source => new(KbmSourceKind.Key, Usage);

    public double Right => Column + Width;

    public double Bottom => Row + Height;

    /// <summary>Whether this key's rectangle overlaps another's.</summary>
    public bool Overlaps(KeyCap other) =>
        Column < other.Right && other.Column < Right &&
        Row < other.Bottom && other.Row < Bottom;
}

/// <summary>One physical group of keys, drawn as a unit.</summary>
public sealed record KeyboardCluster(
    string Name,
    IReadOnlyList<KeyCap> Keys,
    double Columns,
    double Rows);

/// <summary>
/// The keyboard the mapping page draws.
///
/// ## Why draw a keyboard at all
///
/// The Android app pages through a list because a phone cannot show 104 keys.
/// Windows can draw the real thing, and §16.3 says it should — clicking the key
/// you want is immediate in a way that scrolling to "Keyboard 0x1A" never is.
///
/// ## Which keyboard, given there are many
///
/// **Full-size ANSI 104, deliberately, because it is the SUPERSET.** The wire
/// carries a HID usage id and nothing about physical layout, so the drawing is
/// only ever a way to point at a usage. Drawing the largest common keyboard means
/// every smaller one is covered: a tenkeyless owner simply never presses the
/// numpad keys, and a 60% owner still has somewhere to click to bind F5 — which
/// they could not do if the picture matched their own board.
///
/// The families this is a superset of:
///
/// - **Full-size (104)** — what is drawn: main block, navigation cluster, numpad.
/// - **TKL (87)** — full-size without the numpad.
/// - **75% / 65% / 60%** — progressively drop the function row and navigation
///   cluster; their usages are all still here.
///
/// ISO boards are NOT a subset: they add two keys ANSI has no position for, and
/// Japanese boards add several more. Those cannot be drawn in an ANSI picture
/// without lying about where they sit, so they get their own small group instead
/// of being omitted — see <see cref="Other"/>.
///
/// ## Geometry, not rows
///
/// Each key carries an explicit column, row, width and height in key units. A
/// list of rows cannot express an inverted-T arrow cluster, the gaps in a function
/// row, or a numpad Plus that is two units tall, and every one of those is what
/// makes the picture readable as a keyboard rather than as a grid of buttons.
///
/// Usages are from the HID Keyboard/Keypad page (0x07), all within the
/// 0x04..0xE7 range <see cref="KbmSource"/> accepts.
/// </summary>
public static class KeyboardLayout
{
    /*
     * Row origins for the main block. The 0.25 gap under the function row is the
     * one on a real keyboard, and the nav cluster and numpad reuse these exact
     * values so the three clusters line up.
     */
    private const double FunctionRow = 0;
    private const double NumberRow = 1.25;
    private const double TopRow = 2.25;
    private const double HomeRow = 3.25;
    private const double BottomRow = 4.25;
    private const double ModifierRow = 5.25;

    /// <summary>The alphanumeric block and its function row. 15 units wide.</summary>
    public static KeyboardCluster Main { get; } = new(
        "Main",
        [
            new(0x29, "Esc", 0, FunctionRow),
            new(0x3A, "F1", 2, FunctionRow), new(0x3B, "F2", 3, FunctionRow),
            new(0x3C, "F3", 4, FunctionRow), new(0x3D, "F4", 5, FunctionRow),
            new(0x3E, "F5", 6.5, FunctionRow), new(0x3F, "F6", 7.5, FunctionRow),
            new(0x40, "F7", 8.5, FunctionRow), new(0x41, "F8", 9.5, FunctionRow),
            new(0x42, "F9", 11, FunctionRow), new(0x43, "F10", 12, FunctionRow),
            new(0x44, "F11", 13, FunctionRow), new(0x45, "F12", 14, FunctionRow),

            new(0x35, "`", 0, NumberRow),
            new(0x1E, "1", 1, NumberRow), new(0x1F, "2", 2, NumberRow),
            new(0x20, "3", 3, NumberRow), new(0x21, "4", 4, NumberRow),
            new(0x22, "5", 5, NumberRow), new(0x23, "6", 6, NumberRow),
            new(0x24, "7", 7, NumberRow), new(0x25, "8", 8, NumberRow),
            new(0x26, "9", 9, NumberRow), new(0x27, "0", 10, NumberRow),
            new(0x2D, "-", 11, NumberRow), new(0x2E, "=", 12, NumberRow),
            new(0x2A, "Backspace", 13, NumberRow, 2),

            new(0x2B, "Tab", 0, TopRow, 1.5),
            new(0x14, "Q", 1.5, TopRow), new(0x1A, "W", 2.5, TopRow),
            new(0x08, "E", 3.5, TopRow), new(0x15, "R", 4.5, TopRow),
            new(0x17, "T", 5.5, TopRow), new(0x1C, "Y", 6.5, TopRow),
            new(0x18, "U", 7.5, TopRow), new(0x0C, "I", 8.5, TopRow),
            new(0x12, "O", 9.5, TopRow), new(0x13, "P", 10.5, TopRow),
            new(0x2F, "[", 11.5, TopRow), new(0x30, "]", 12.5, TopRow),
            new(0x31, "\\", 13.5, TopRow, 1.5),

            new(0x39, "Caps", 0, HomeRow, 1.75),
            new(0x04, "A", 1.75, HomeRow), new(0x16, "S", 2.75, HomeRow),
            new(0x07, "D", 3.75, HomeRow), new(0x09, "F", 4.75, HomeRow),
            new(0x0A, "G", 5.75, HomeRow), new(0x0B, "H", 6.75, HomeRow),
            new(0x0D, "J", 7.75, HomeRow), new(0x0E, "K", 8.75, HomeRow),
            new(0x0F, "L", 9.75, HomeRow), new(0x33, ";", 10.75, HomeRow),
            new(0x34, "'", 11.75, HomeRow),
            new(0x28, "Enter", 12.75, HomeRow, 2.25),

            new(0xE1, "Shift", 0, BottomRow, 2.25),
            new(0x1D, "Z", 2.25, BottomRow), new(0x1B, "X", 3.25, BottomRow),
            new(0x06, "C", 4.25, BottomRow), new(0x19, "V", 5.25, BottomRow),
            new(0x05, "B", 6.25, BottomRow), new(0x11, "N", 7.25, BottomRow),
            new(0x10, "M", 8.25, BottomRow), new(0x36, ",", 9.25, BottomRow),
            new(0x37, ".", 10.25, BottomRow), new(0x38, "/", 11.25, BottomRow),
            new(0xE5, "Shift", 12.25, BottomRow, 2.75),

            new(0xE0, "Ctrl", 0, ModifierRow, 1.25),
            new(0xE3, "Win", 1.25, ModifierRow, 1.25),
            new(0xE2, "Alt", 2.5, ModifierRow, 1.25),
            new(0x2C, "Space", 3.75, ModifierRow, 6.25),
            new(0xE6, "Alt", 10, ModifierRow, 1.25),
            new(0xE7, "Win", 11.25, ModifierRow, 1.25),
            new(0x65, "Menu", 12.5, ModifierRow, 1.25),
            new(0xE4, "Ctrl", 13.75, ModifierRow, 1.25),
        ],
        Columns: 15,
        Rows: 6.25);

    /// <summary>
    /// The navigation cluster, including the inverted-T arrows.
    ///
    /// The arrows were previously drawn inline with Insert and Home, which is not
    /// where anyone's fingers expect them; the T is most of what makes this block
    /// recognisable at a glance.
    /// </summary>
    public static KeyboardCluster Navigation { get; } = new(
        "Navigation",
        [
            new(0x46, "PrtSc", 0, FunctionRow), new(0x47, "ScrLk", 1, FunctionRow),
            new(0x48, "Pause", 2, FunctionRow),

            new(0x49, "Ins", 0, NumberRow), new(0x4A, "Home", 1, NumberRow),
            new(0x4B, "PgUp", 2, NumberRow),
            new(0x4C, "Del", 0, TopRow), new(0x4D, "End", 1, TopRow),
            new(0x4E, "PgDn", 2, TopRow),

            new(0x52, "Up", 1, BottomRow),
            new(0x50, "Left", 0, ModifierRow), new(0x51, "Down", 1, ModifierRow),
            new(0x4F, "Right", 2, ModifierRow),
        ],
        Columns: 3,
        Rows: 6.25);

    /// <summary>
    /// The numeric keypad. Plus and Enter are two units tall, as on the hardware.
    /// </summary>
    public static KeyboardCluster Numpad { get; } = new(
        "Numpad",
        [
            new(0x53, "Num", 0, NumberRow), new(0x54, "/", 1, NumberRow),
            new(0x55, "*", 2, NumberRow), new(0x56, "-", 3, NumberRow),

            new(0x5F, "7", 0, TopRow), new(0x60, "8", 1, TopRow),
            new(0x61, "9", 2, TopRow),
            new(0x57, "+", 3, TopRow, 1, 2),

            new(0x5C, "4", 0, HomeRow), new(0x5D, "5", 1, HomeRow),
            new(0x5E, "6", 2, HomeRow),

            new(0x59, "1", 0, BottomRow), new(0x5A, "2", 1, BottomRow),
            new(0x5B, "3", 2, BottomRow),
            new(0x58, "Enter", 3, BottomRow, 1, 2),

            new(0x62, "0", 0, ModifierRow, 2), new(0x63, ".", 2, ModifierRow),
        ],
        Columns: 4,
        Rows: 6.25);

    /// <summary>
    /// Keys that exist on real keyboards but have no position on an ANSI board.
    ///
    /// ISO layouts add two (the extra key beside left Shift, and the one beside
    /// Enter); Japanese layouts add several more. Drawing them inside the ANSI
    /// picture would put them somewhere they are not, so they are offered as a
    /// plain group instead — present and bindable, without the picture claiming a
    /// position it does not have.
    /// </summary>
    public static KeyboardCluster Other { get; } = new(
        "Other",
        [
            new(0x64, "ISO \\", 0, 0, 1.5), new(0x32, "ISO #", 1.5, 0, 1.5),
            new(0x87, "Ro", 3, 0, 1.5), new(0x89, "Yen", 4.5, 0, 1.5),
            new(0x88, "Kana", 6, 0, 1.5), new(0x8A, "Henkan", 7.5, 0, 1.5),
            new(0x8B, "Muhenkan", 9, 0, 1.5),
        ],
        Columns: 10.5,
        Rows: 1);

    /// <summary>The three aligned clusters, left to right.</summary>
    public static IReadOnlyList<KeyboardCluster> Clusters { get; } = [Main, Navigation, Numpad];

    /// <summary>Everything drawn anywhere, including <see cref="Other"/>.</summary>
    public static IReadOnlyList<KeyCap> AllKeys { get; } =
        [.. Clusters.SelectMany(cluster => cluster.Keys), .. Other.Keys];

    /// <summary>
    /// The five mouse buttons.
    ///
    /// Sources on the same footing as keys — the adapter binds them through the
    /// same command — but not keys, so they are not smuggled into the key grid
    /// where they would imply a physical position they do not have.
    /// </summary>
    public static IReadOnlyList<KeyCap> MouseButtons { get; } =
    [
        new(1, "Left", 0, 0, 2), new(2, "Right", 2, 0, 2), new(3, "Middle", 4, 0, 2),
        new(4, "Back", 6, 0, 2), new(5, "Forward", 8, 0, 2),
    ];

    public static KbmSource MouseSource(KeyCap cap) => new(KbmSourceKind.MouseButton, cap.Usage);

    /// <summary>
    /// A label for any source, drawn or not.
    ///
    /// The mapping list shows every binding the adapter reports, including keys
    /// this layout does not draw, so it needs an answer for all of them. An
    /// undrawn key falls back to its usage id rather than being hidden.
    /// </summary>
    public static string Describe(KbmSource source)
    {
        if (source.Kind == KbmSourceKind.MouseButton)
        {
            return MouseButtons.FirstOrDefault(cap => cap.Usage == source.Code) is { } button
                ? $"Mouse {button.Label}"
                : $"Mouse button {source.Code}";
        }

        return AllKeys.FirstOrDefault(cap => cap.Usage == source.Code)?.Label
            ?? $"Key 0x{source.Code:X2}";
    }
}
