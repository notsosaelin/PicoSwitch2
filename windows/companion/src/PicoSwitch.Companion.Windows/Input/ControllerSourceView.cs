namespace PicoSwitch.Companion.Windows.Input;

/// <summary>One row in the input-source chooser.</summary>
/// <param name="Id">
/// The <c>RawGameController.NonRoamableId</c>, which is what a remembered
/// choice is keyed on.
/// </param>
/// <param name="Label">
/// What the row reads as. Name plus where the device is, because on a handheld
/// "Controller" and "Controller" are otherwise the same row twice.
/// </param>
/// <param name="CanDrive">
/// Whether choosing this row actually produces input. A row that cannot is still
/// offered — hiding it would leave a user staring at a list missing the
/// controller they are holding — but it says so rather than going quietly mute.
/// </param>
public sealed record ControllerSourceRow(string Id, string Label, bool CanDrive);

/// <summary>
/// Pure projection of the Windows input-source selection, for the Gamepad page
/// to paint.
///
/// The page decides nothing: which source is used is settled by
/// <see cref="ControllerSourceSelection"/>, and this only renders that outcome
/// into sentences. Kept out of the page so the wording is testable, and out of
/// Services because the enumeration surfaces are Windows-only.
/// </summary>
public sealed record ControllerSourceView(
    IReadOnlyList<ControllerSourceRow> Rows,
    string? SelectedId,
    string Headline,
    string Detail,
    bool CanChoose)
{
    public static ControllerSourceView Of(
        IReadOnlyList<WindowsControllerSource> sources,
        WindowsControllerSource? selected,
        string? unresolvedReason)
    {
        var rows = sources
            .Select(s => new ControllerSourceRow(
                s.Id,
                s.CanDrive ? $"{s.Name} ({s.AttachmentLabel})"
                           : $"{s.Name} ({s.AttachmentLabel}) — cannot be read",
                s.CanDrive))
            .ToList();

        // A chooser for one option is a decision the user cannot make. It appears
        // when there is a genuine choice, or when the single option is one the app
        // deliberately would not take by itself — the adapter's own echo, or a
        // device Windows will not give us named input for.
        var canChoose = rows.Count > 1 || (rows.Count == 1 && selected is null);

        if (selected is null)
        {
            return new ControllerSourceView(
                rows,
                null,
                "No controller selected",
                unresolvedReason ?? "No usable controller is connected to this PC.",
                canChoose);
        }

        var detail = selected.UnreadableReason is { } reason
            // Chosen explicitly despite not being readable: say so plainly rather
            // than showing a healthy-looking selection that sends nothing.
            ? $"{reason}. Controller Link will not receive input from it."
            : selected.MayBeThisAdapter
                // Only reachable by explicit choice. Someone may be testing the
                // loop deliberately; they should still be told it is a loop.
                ? $"{selected.AttachmentLabel}. This looks like the adapter's own " +
                  "output — using it feeds the adapter back into itself."
                : $"{selected.AttachmentLabel} and ready.";

        return new ControllerSourceView(
            rows, selected.Id, selected.Name, detail, canChoose);
    }
}
