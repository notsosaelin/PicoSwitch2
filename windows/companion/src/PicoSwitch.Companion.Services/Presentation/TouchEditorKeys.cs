using PicoSwitch.Bridge.Touch;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// What a key press means in the layout editor.
///
/// Named commands rather than key codes so the surface's handler is a switch over
/// intentions, and so the whole table can be asserted without a window.
/// </summary>
public enum TouchEditorCommand
{
    None,
    NudgeLeft,
    NudgeRight,
    NudgeUp,
    NudgeDown,
    Grow,
    Shrink,
    Undo,
    Redo,
    Save,
    Delete,
    SelectAll,
    Deselect,
    Group,
    Ungroup,
    ToggleSnap,
    ToggleGrid,
    NextControl,
    PreviousControl,
    Exit,
}

/// <summary>
/// A resolved key press: what to do, and whether the user asked for the coarse step.
/// </summary>
public readonly record struct TouchEditorKeyStroke(TouchEditorCommand Command, bool Coarse)
{
    public static readonly TouchEditorKeyStroke None = new(TouchEditorCommand.None, false);
}

/// <summary>
/// The editor's keyboard, as a table.
///
/// `WINDOWS_PASS.md` §26.5 requires the layout editor to be driven **entirely by
/// keyboard** before it is driven by mouse, pen or touch, so this is a first-class input
/// path rather than an accessibility afterthought — and a first-class input path deserves
/// a tested table rather than a chain of `if` statements inside an event handler.
///
/// ## Why the key arrives as a string
///
/// The caller passes <c>VirtualKey.ToString()</c>. That keeps this file free of any WinRT
/// type (the Services layer may not carry presentation types) WITHOUT introducing a
/// second enum that something would have to translate into — an untested translation
/// table between two enums is exactly where a key quietly stops working. The names below
/// are the real ones WinRT produces, including the two that come back as numbers because
/// <c>VirtualKey</c> has no member for them.
/// </summary>
public static class TouchEditorKeys
{
    /// <summary>A nudge, in layout units. Small enough to be a correction, large enough to see.</summary>
    public const float NudgeUnits = 2f;

    /// <summary>The Shift step: about half a control, for crossing the screen deliberately.</summary>
    public const float CoarseNudgeUnits = 10f;

    /// <summary>One press of Grow or Shrink.</summary>
    public const float ScaleStep = 1.05f;

    public static TouchEditorKeyStroke Resolve(string? key, bool control, bool shift)
    {
        if (string.IsNullOrEmpty(key))
        {
            return TouchEditorKeyStroke.None;
        }

        if (control)
        {
            return new TouchEditorKeyStroke(key switch
            {
                // Shift+Ctrl+Z is the other half of the redo idiom; both are offered
                // because both are muscle memory somewhere.
                "Z" => shift ? TouchEditorCommand.Redo : TouchEditorCommand.Undo,
                "Y" => TouchEditorCommand.Redo,
                "S" => TouchEditorCommand.Save,
                "A" => TouchEditorCommand.SelectAll,
                "G" => shift ? TouchEditorCommand.Ungroup : TouchEditorCommand.Group,
                _ => TouchEditorCommand.None,
            }, Coarse: false);
        }

        var command = key switch
        {
            "Left" => TouchEditorCommand.NudgeLeft,
            "Right" => TouchEditorCommand.NudgeRight,
            "Up" => TouchEditorCommand.NudgeUp,
            "Down" => TouchEditorCommand.NudgeDown,

            // Tab walks the layout, which is what makes a keyboard-only session able to
            // reach a control at all. Shift+Tab walks it backwards.
            "Tab" => shift ? TouchEditorCommand.PreviousControl : TouchEditorCommand.NextControl,

            "Delete" or "Back" => TouchEditorCommand.Delete,
            "Escape" => TouchEditorCommand.Deselect,

            // VirtualKey has no OemPlus/OemMinus member, so the main-row keys arrive as
            // their raw codes. The numpad ones have names. Both are accepted; a user
            // should not have to know which keyboard row the editor was written against.
            "Add" or "187" => TouchEditorCommand.Grow,
            "Subtract" or "189" => TouchEditorCommand.Shrink,

            "G" => TouchEditorCommand.ToggleGrid,
            "S" => TouchEditorCommand.ToggleSnap,
            _ => TouchEditorCommand.None,
        };

        return new TouchEditorKeyStroke(command, Coarse: shift && IsNudge(command));
    }

    /// <summary>
    /// A nudge in SCREEN pixels, for <see cref="TouchLayoutEditor.Move"/>.
    ///
    /// Units are converted here rather than at the call site because the conversion needs
    /// the layout's own resolved scale: the same two-unit nudge is a different number of
    /// pixels on a small window than on a large one, and a keyboard step that changed
    /// meaning with the window would be unusable for precise work.
    /// </summary>
    public static float NudgePixels(ResolvedTouchLayout resolved, bool coarse)
    {
        var units = coarse ? CoarseNudgeUnits : NudgeUnits;
        var pixels = units * resolved.Scale * resolved.Region.UnitScale;
        return float.IsFinite(pixels) && pixels > 0f ? pixels : units;
    }

    private static bool IsNudge(TouchEditorCommand command) => command is
        TouchEditorCommand.NudgeLeft or TouchEditorCommand.NudgeRight or
        TouchEditorCommand.NudgeUp or TouchEditorCommand.NudgeDown;
}
