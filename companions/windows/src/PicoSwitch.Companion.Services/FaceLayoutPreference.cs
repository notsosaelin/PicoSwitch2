using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Storage;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The user's printed-legend choice, persisted.
///
/// Persisted because it describes the user's HARDWARE, not this session: a
/// controller does not change which letters are printed on it between launches,
/// and re-picking every time is the kind of friction that makes a setting feel
/// broken.
///
/// Shared by every companion's composition root so the document name and its
/// vocabulary (<see cref="ControllerFaceLayouts.Key"/>) have one owner. Anything
/// unrecognised — a missing file, a damaged one, a value from a future build —
/// is Auto, which is also what a fresh install gets.
/// </summary>
public static class FaceLayoutPreference
{
    public const string DocumentName = "face-layout";

    public static ControllerFaceLayout Load(DocumentStore documents) =>
        documents.Read(DocumentName)?.Trim() switch
        {
            "nintendo" => ControllerFaceLayout.Nintendo,
            "xbox" => ControllerFaceLayout.Xbox,
            _ => ControllerFaceLayout.Auto,
        };

    /// <summary>Apply the stored choice to a session. Auto leaves the session's own default alone.</summary>
    public static void Restore(DocumentStore documents, ControllerInputSession input)
    {
        var layout = Load(documents);
        if (layout != ControllerFaceLayout.Auto)
        {
            input.SetFaceLayout(layout);
        }
    }

    /// <summary>Choose the legend for the session, and remember it.</summary>
    public static void Set(DocumentStore documents, ControllerInputSession input, ControllerFaceLayout layout)
    {
        input.SetFaceLayout(layout);
        _ = documents.Write(DocumentName, layout.Key());
    }
}
