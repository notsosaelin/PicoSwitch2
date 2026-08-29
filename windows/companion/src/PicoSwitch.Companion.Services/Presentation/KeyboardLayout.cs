using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>One key on the rendered keyboard.</summary>
/// <param name="Usage">The HID keyboard usage id this key sends.</param>
/// <param name="Label">What is printed on it.</param>
/// <param name="Width">Width in key units, so a Shift is visibly a Shift.</param>
public sealed record KeyCap(int Usage, string Label, double Width = 1.0)
{
    public KbmSource Source => new(KbmSourceKind.Key, Usage);
}

/// <summary>
/// The keyboard the mapping page draws.
///
/// ## Why a layout table exists at all
///
/// The Android app pages through a list because a phone cannot show 104 keys.
/// Windows can draw the actual keyboard, and §16.3 says it should — clicking the
/// key you want to rebind is immediate in a way that scrolling to
/// "Keyboard 0x1A" never is.
///
/// ## What this is and is not
///
/// It is a PRESENTATION table: HID usage ids arranged the way a US ANSI keyboard
/// is arranged, with labels a person recognises. It is not protocol truth — the
/// wire only ever carries the usage id, and <see cref="KbmSource"/> owns what is
/// valid. A key absent from this table can still be bound by the adapter and will
/// still appear in the mapping list; it simply is not drawn.
///
/// The usages are the HID Keyboard/Keypad page (0x07) values that
/// <c>KbmSource</c> already accepts, 0x04..0xE7.
/// </summary>
public static class KeyboardLayout
{
    /// <summary>US ANSI, in the order it is drawn. Row breaks are physical rows.</summary>
    public static IReadOnlyList<IReadOnlyList<KeyCap>> Rows { get; } =
    [
        [
            new(0x29, "Esc"),
            new(0x3A, "F1"), new(0x3B, "F2"), new(0x3C, "F3"), new(0x3D, "F4"),
            new(0x3E, "F5"), new(0x3F, "F6"), new(0x40, "F7"), new(0x41, "F8"),
            new(0x42, "F9"), new(0x43, "F10"), new(0x44, "F11"), new(0x45, "F12"),
        ],
        [
            new(0x35, "`"),
            new(0x1E, "1"), new(0x1F, "2"), new(0x20, "3"), new(0x21, "4"), new(0x22, "5"),
            new(0x23, "6"), new(0x24, "7"), new(0x25, "8"), new(0x26, "9"), new(0x27, "0"),
            new(0x2D, "-"), new(0x2E, "="),
            new(0x2A, "Backspace", 2.0),
        ],
        [
            new(0x2B, "Tab", 1.5),
            new(0x14, "Q"), new(0x1A, "W"), new(0x08, "E"), new(0x15, "R"), new(0x17, "T"),
            new(0x1C, "Y"), new(0x18, "U"), new(0x0C, "I"), new(0x12, "O"), new(0x13, "P"),
            new(0x2F, "["), new(0x30, "]"), new(0x31, "\\", 1.5),
        ],
        [
            new(0x39, "Caps", 1.75),
            new(0x04, "A"), new(0x16, "S"), new(0x07, "D"), new(0x09, "F"), new(0x0A, "G"),
            new(0x0B, "H"), new(0x0D, "J"), new(0x0E, "K"), new(0x0F, "L"),
            new(0x33, ";"), new(0x34, "'"),
            new(0x28, "Enter", 2.25),
        ],
        [
            new(0xE1, "Shift", 2.25),
            new(0x1D, "Z"), new(0x1B, "X"), new(0x06, "C"), new(0x19, "V"), new(0x05, "B"),
            new(0x11, "N"), new(0x10, "M"),
            new(0x36, ","), new(0x37, "."), new(0x38, "/"),
            new(0xE5, "Shift", 2.75),
        ],
        [
            new(0xE0, "Ctrl", 1.25), new(0xE3, "Win", 1.25), new(0xE2, "Alt", 1.25),
            new(0x2C, "Space", 6.25),
            new(0xE6, "Alt", 1.25), new(0xE7, "Win", 1.25), new(0xE4, "Ctrl", 1.25),
        ],
        [
            new(0x49, "Ins"), new(0x4A, "Home"), new(0x4B, "PgUp"),
            new(0x4C, "Del"), new(0x4D, "End"), new(0x4E, "PgDn"),
            new(0x52, "Up"), new(0x51, "Down"), new(0x50, "Left"), new(0x4F, "Right"),
        ],
    ];

    /// <summary>
    /// The five mouse buttons, drawn beside the keyboard.
    ///
    /// Sources on the same footing as keys — the adapter binds them through the
    /// same command — but they are not keys, so they are not smuggled into the key
    /// grid where they would imply a physical position they do not have.
    /// </summary>
    public static IReadOnlyList<KeyCap> MouseButtons { get; } =
    [
        new(1, "Left"), new(2, "Right"), new(3, "Middle"), new(4, "Back"), new(5, "Forward"),
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

        return Rows
            .SelectMany(row => row)
            .FirstOrDefault(cap => cap.Usage == source.Code)?.Label
            ?? $"Key 0x{source.Code:X2}";
    }
}
