using PicoSwitch.Bridge.Core;

namespace PicoSwitch.Companion.Windows.Input;

/// <summary>
/// Where a controller physically is, as Windows reports it.
/// </summary>
public enum ControllerAttachment
{
    /// <summary>
    /// Part of this machine's chassis: a handheld's built-in sticks and
    /// buttons. Windows says so by putting the device in the machine's own
    /// container (<c>{00000000-0000-0000-FFFF-FFFFFFFFFFFF}</c>) rather than
    /// giving it one of its own.
    /// </summary>
    BuiltIn,

    /// <summary>Plugged in or paired: its own device container.</summary>
    External,

    /// <summary>Windows did not say. Treated as external.</summary>
    Unknown,
}

/// <summary>
/// One Windows input source, as offered to Controller Link.
///
/// Wraps the shared <see cref="ControllerCandidate"/> — whose usability rule is
/// the same on every platform and must not be re-litigated here — with the two
/// things only Windows can answer: where the device physically is, and whether
/// this app can actually read it.
/// </summary>
/// <param name="Candidate">
/// The platform-neutral view. <c>Descriptor</c> carries the
/// <c>RawGameController.NonRoamableId</c>, which is the stable per-device string
/// a remembered selection is keyed on.
/// </param>
/// <param name="Attachment">Built-in, external, or unknown.</param>
/// <param name="IsGamepadClass">
/// Whether Windows maps this device onto <c>Windows.Gaming.Input.Gamepad</c>.
///
/// This matters more than it looks. <c>Gamepad</c> is a SUBSET —
/// XInput-class devices only — and it is what the first implementation polled,
/// so a DualSense, a DualShock 4 or a Switch Pro Controller produced nothing at
/// all while appearing perfectly healthy. Measured on this bench 2026-09-02: a
/// Switch 2 Pro Controller gave <c>RawGameController</c> count 1 and
/// <c>Gamepad</c> count 0.
///
/// A device that is not gamepad-class still enumerates, is still offered, and is
/// still honestly labelled — it simply reports named buttons through a different
/// surface, and rumble through none.
/// </param>
/// <param name="MayBeThisAdapter">
/// The device's USB identity matches the personality the connected PicoSwitch
/// adapter is currently emulating.
///
/// A PicoSwitch plugged into this PC enumerates as an ordinary controller of
/// whatever it is pretending to be, so selecting it would feed the adapter's own
/// output back in as its input. It is NOT hidden — a genuine Pro Controller 2
/// carries the same VID/PID and a user is entitled to use one — but it is kept
/// out of automatic selection, because a silent feedback loop is much worse than
/// one extra click.
/// </param>
public sealed record WindowsControllerSource(
    ControllerCandidate Candidate,
    ControllerAttachment Attachment,
    bool IsGamepadClass,
    bool MayBeThisAdapter = false)
{
    public string Id => Candidate.Descriptor;

    public string Name => Candidate.Name;

    public bool IsUsable => Candidate.IsUsable;

    /// <summary>
    /// What the picker shows beside the name. Deliberately plain: a user
    /// choosing an input source cares where the thing is, not what enumerated it.
    /// </summary>
    public string AttachmentLabel => Attachment switch
    {
        ControllerAttachment.BuiltIn => "Built-in",
        ControllerAttachment.External => "Connected",
        _ => "Connected",
    };

    /// <summary>
    /// Why this source cannot currently be read, or null when it can.
    ///
    /// Honest rather than convenient: a source Windows will not give us named
    /// input is offered and labelled, not silently selected and then mute.
    /// </summary>
    public string? UnreadableReason =>
        !IsUsable ? Candidate.ExclusionReason
        : !IsGamepadClass
            ? "Windows does not expose this controller's buttons to apps in a standard layout"
        : null;

    public bool CanDrive => UnreadableReason is null;
}

/// <summary>
/// Which Windows source Controller Link should use.
///
/// The ambiguity rule itself is shared and lives in
/// <see cref="ControllerCandidates"/>: exactly one usable device is chosen
/// without asking, two or more means the user decides. That is not re-decided
/// here — this only removes options that are known-bad to choose automatically,
/// then defers.
/// </summary>
public static class ControllerSourceSelection
{
    /// <summary>
    /// Sources fit to be chosen automatically: usable, readable, and not the
    /// adapter's own echo.
    /// </summary>
    public static IReadOnlyList<WindowsControllerSource> AutoSelectable(
        IReadOnlyList<WindowsControllerSource> sources) =>
        [.. sources.Where(s => s.CanDrive && !s.MayBeThisAdapter)];

    /// <summary>
    /// Resolve the source for a refresh.
    ///
    /// A remembered choice wins whenever it is still present and still usable —
    /// including a non-gamepad-class device or the adapter itself, because an
    /// explicit choice is a decision the app has no business overriding.
    /// Otherwise fall back to the shared auto-select rule over what is fit to be
    /// chosen automatically. Null means the user must pick.
    /// </summary>
    public static WindowsControllerSource? Resolve(
        IReadOnlyList<WindowsControllerSource> sources,
        string? rememberedId)
    {
        if (!string.IsNullOrEmpty(rememberedId))
        {
            var remembered = sources.FirstOrDefault(
                s => s.Id == rememberedId && s.IsUsable);
            if (remembered is not null)
            {
                return remembered;
            }
        }

        var selectable = AutoSelectable(sources);

        // A handheld's built-in controls are the one case where "more than one
        // usable device" still has an obvious answer: the machine's own sticks
        // are the controller the user is holding, and an external pad they also
        // plugged in is the deliberate act they can express by choosing it.
        var builtIn = selectable.Where(s => s.Attachment == ControllerAttachment.BuiltIn).ToList();
        if (builtIn.Count == 1)
        {
            return builtIn[0];
        }

        return selectable.Count == 1 ? selectable[0] : null;
    }

    /// <summary>
    /// Why nothing was chosen, for the UI to say out loud. Null when a source
    /// was resolved.
    /// </summary>
    public static string? UnresolvedReason(
        IReadOnlyList<WindowsControllerSource> sources,
        WindowsControllerSource? resolved)
    {
        if (resolved is not null)
        {
            return null;
        }

        if (sources.Count == 0)
        {
            return "No controller is connected to this PC.";
        }

        var selectable = AutoSelectable(sources);
        if (selectable.Count > 1)
        {
            return "More than one controller is connected. Choose which one to use.";
        }

        if (sources.Any(s => s.MayBeThisAdapter))
        {
            return "The only controller found looks like this adapter's own output. " +
                   "Connect a different controller, or choose it anyway.";
        }

        var unreadable = sources.FirstOrDefault(s => !s.CanDrive);
        return unreadable?.UnreadableReason
            ?? "No usable controller is connected to this PC.";
    }
}
