using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// Settings' "Face buttons" row on every platform: the choices, in order, and
/// the one line that says what Automatic is doing right now.
/// </summary>
public static class FaceLayoutChoices
{
    public static readonly IReadOnlyList<(ControllerFaceLayout Layout, string Label)> Options =
    [
        (ControllerFaceLayout.Auto, "Automatic"),
        (ControllerFaceLayout.Nintendo, "Match the letters (A sends A)"),
        (ControllerFaceLayout.Xbox, "Match the positions (bottom sends bottom)"),
    ];

    public static int IndexOf(ControllerFaceLayout layout)
    {
        for (var index = 0; index < Options.Count; index++)
        {
            if (Options[index].Layout == layout)
            {
                return index;
            }
        }

        return -1;
    }

    /// <summary>
    /// The one line under the choice: what Automatic is doing, and nothing for a
    /// manual choice (the selector already says it). WHY Automatic resolved that
    /// way is detail for a tooltip, not the line: on screen it made the row a
    /// paragraph (companion-shell-layout.md, "Full explanations live in
    /// tooltips").
    /// </summary>
    /// <param name="chosen">The stored choice.</param>
    /// <param name="resolved">What Automatic resolved to.</param>
    public static string Reason(ControllerFaceLayout chosen, ControllerFaceLayout resolved) =>
        chosen == ControllerFaceLayout.Auto
            ? (resolved == ControllerFaceLayout.Nintendo ? "Matching the letters." : "Matching the positions.")
            : "";
}
