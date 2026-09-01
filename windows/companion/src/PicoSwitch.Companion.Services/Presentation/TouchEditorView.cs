using PicoSwitch.Bridge.Touch;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>How loudly the surface should say whatever it is saying.</summary>
public enum TouchEditorSeverity
{
    Neutral,
    Advisory,
    Blocking,
}

/// <summary>
/// What the on-screen controller is currently for.
///
/// The two modes the Android surface has, and for the same reason: in
/// <see cref="Play"/> the screen is a controller and touches are input, in
/// <see cref="Edit"/> the screen is a canvas and touches manipulate scene objects.
/// Conflating them is precisely how an edit drag becomes an A press — and, on the
/// presentation side, how the editor's chrome ends up permanently covering a surface
/// whose whole job is to be pressed.
///
/// Play is the default, exactly as on Android. The editor is entered deliberately from
/// the menu and left deliberately.
/// </summary>
public enum TouchSurfaceMode
{
    Play,
    Edit,
}

/// <summary>One line of the audit, ready to draw.</summary>
public sealed record TouchAuditLine(string Message, bool Blocking)
{
    public TouchEditorSeverity Severity =>
        Blocking ? TouchEditorSeverity.Blocking : TouchEditorSeverity.Advisory;
}

/// <summary>
/// Everything the Touch Gamepad surface DISPLAYS, decided here rather than in XAML.
///
/// The same rule as <see cref="ControllerLinkView"/>: a sentence written in markup cannot
/// be asserted, and several of these sentences are ones the project has committed to
/// getting right — the neutral state when no personality is confirmed, the honest note
/// that this build cannot drive a console (`WINDOWS_PASS.md` §15.8), and the difference
/// between an advisory finding and one that blocks play.
///
/// It decides nothing about geometry. The resolved layout arrives already composed,
/// resolved and audited by <see cref="TouchGamepadService"/>, so the canvas and this
/// banner are two views of one computation rather than two computations.
/// </summary>
public sealed record TouchEditorView
{
    /// <summary>The controller the console is being shown, as a person would name it.</summary>
    public string Title { get; init; } = "On-screen controller";

    public string ProfileName { get; init; } = string.Empty;

    /// <summary>
    /// The profile name, plus how the controller was identified when that is worth saying.
    ///
    /// A remembered personality is still a confirmation, but it can be out of date, and a
    /// surface that presented it as live would let a user tune a GameCube layout for an
    /// adapter that has since been switched to Pro Controller 2.
    /// </summary>
    public string Subtitle { get; init; } = string.Empty;

    /// <summary>The one line under the toolbar.</summary>
    public string Status { get; init; } = string.Empty;

    public TouchEditorSeverity StatusSeverity { get; init; } = TouchEditorSeverity.Neutral;

    /// <summary>
    /// Every audit finding, blocking first.
    ///
    /// All of them rather than just the first: the status line has room for one, and a
    /// user fixing a layout needs the list. Blocking first because that is the order they
    /// have to be fixed in — an advisory finding never stops play.
    /// </summary>
    public IReadOnlyList<TouchAuditLine> Findings { get; init; } = [];

    /// <summary>What the properties panel says it is acting on.</summary>
    public string SelectionSummary { get; init; } = "Nothing selected";

    /// <summary>
    /// The selection's size multiplier, when they all agree.
    ///
    /// Null for a mixed selection, so the panel can show a blank rather than pick one
    /// control's value and silently apply it to the rest on the next keystroke.
    /// </summary>
    public float? Scale { get; init; }

    public bool Editable { get; init; }

    public bool CanSave { get; init; }

    public bool CanUndo { get; init; }

    public bool CanRedo { get; init; }

    public bool CanDelete { get; init; }

    public bool CanGroup { get; init; }

    public bool CanUngroup { get; init; }

    /// <summary>
    /// The short status the gameplay surface shows, unobtrusively. Null once a link exists.
    /// </summary>
    /// <remarks>
    /// Short because of WHERE it goes: a pill in the layout's own quiet centre band, the
    /// same place and the same shape as the Android surface's link banner. The band is
    /// kept clear by the layout itself, so a status there shadows nothing the user is
    /// trying to press — and it takes no height from the controller, unlike the status
    /// strip it replaces.
    /// </remarks>
    public string? LinkNote { get; init; }

    /// <summary>
    /// The whole explanation, for the menu.
    ///
    /// §15.8 is explicit: where Controller Link is unavailable the surface still opens and
    /// remains fully editable, "and the UI must say exactly that rather than appearing
    /// broken". Saying it needs more room than a gameplay surface should give it, so the
    /// sentence lives one tap away instead of across the bottom of the controller.
    /// </summary>
    public string? LinkDetail { get; init; }

    /// <summary>What the surface is currently for.</summary>
    public TouchSurfaceMode Mode { get; init; } = TouchSurfaceMode.Play;

    /// <summary>
    /// Whether the editor's chrome may be drawn at all.
    ///
    /// The rule the Windows surface got wrong: on Android the toolbar exists only inside
    /// edit mode, and play mode is controls and nothing else. A permanently visible
    /// toolbar, inspector and status strip turn a gameplay surface into a debugging
    /// overlay — which is the state the 2026-09-01 regression screenshot captured.
    /// </summary>
    public bool ShowEditorChrome => Mode == TouchSurfaceMode.Edit && Editable;

    /// <summary>
    /// Whether "use the shipped layout" would do anything.
    ///
    /// False on the factory profile, which already IS the shipped layout.
    /// </summary>
    public bool CanResetToDefault { get; init; }

    /// <summary>Before any personality has been confirmed.</summary>
    public static TouchEditorView Neutral { get; } = new()
    {
        Title = "On-screen controller",
        Status = "Connect an adapter to see the layout for the controller it is emulating.",
        StatusSeverity = TouchEditorSeverity.Neutral,
        SelectionSummary = "Nothing selected",
        Editable = false,
    };

    public static TouchEditorView Of(
        TouchGamepadState state,
        bool controllerLinkAvailable,
        TouchSurfaceMode mode = TouchSurfaceMode.Play)
    {
        if (state.Personality is not { } personality)
        {
            return Neutral with
            {
                Mode = mode,
                LinkNote = LinkNoteFor(controllerLinkAvailable),
                LinkDetail = LinkDetailFor(controllerLinkAvailable),
            };
        }

        var profile = TouchProfileCatalog.Require(personality);
        var selection = state.Selection;
        var instances = selection
            .Select(state.Document.Instance)
            .OfType<TouchControlInstance>()
            .ToList();

        var findings = state.Resolved.Findings
            .OrderByDescending(finding => finding.Blocking)
            .Select(finding => new TouchAuditLine(finding.Message, finding.Blocking))
            .ToList();

        return new TouchEditorView
        {
            Mode = mode,
            Title = profile.DisplayName,
            ProfileName = state.Library.Selected.Name,
            CanResetToDefault = !state.Library.Selected.IsFactory,
            Subtitle = state.PersonalityRemembered
                ? $"{state.Library.Selected.Name} · last seen on this adapter"
                : state.Library.Selected.Name,
            Status = state.Warning ?? Summary(state),
            StatusSeverity = Severity(state),
            Findings = findings,
            SelectionSummary = Describe(profile, instances),
            Scale = SharedScale(instances),
            Editable = state.Editable,

            // Saving onto the factory profile is a legal action — the library turns it
            // into a new profile — so this is enabled by unsaved work, not by which
            // profile happens to be selected.
            CanSave = state.Editable && state.Dirty,
            CanUndo = state.Editable && state.CanUndo,
            CanRedo = state.Editable && state.CanRedo,
            CanDelete = state.Editable && instances.Count > 0,
            CanGroup = state.Editable && Groupable(state.Document, selection),
            CanUngroup = state.Editable &&
                instances.Any(instance => instance.GroupId is not null),
            LinkNote = LinkNoteFor(controllerLinkAvailable),
            LinkDetail = LinkDetailFor(controllerLinkAvailable),
        };
    }

    /// <summary>
    /// The pill, in the Android surface's own words for this exact state.
    ///
    /// Android's link banner says "This device cannot act as a controller" when the host
    /// cannot be one. Saying the same thing here keeps the two surfaces describing one
    /// condition the same way.
    /// </summary>
    private static string? LinkNoteFor(bool available) => available
        ? null
        : "This PC cannot act as a controller";

    private static string? LinkDetailFor(bool available) => available
        ? null
        : "This PC cannot send a controller to the adapter, so the on-screen controller " +
          "cannot drive a console here. Layouts, profiles and validation are entirely " +
          "local, so everything on this screen still works.";

    private static string Summary(TouchGamepadState state)
    {
        var visible = state.Resolved.Controls.Count;
        var controls = visible == 1 ? "1 control" : $"{visible} controls";
        return state.Dirty
            ? $"{controls} · unsaved changes"
            : controls;
    }

    private static TouchEditorSeverity Severity(TouchGamepadState state)
    {
        if (state.Warning is null)
        {
            return TouchEditorSeverity.Neutral;
        }

        // The warning line carries whichever of storage, composition or the audit spoke
        // first. Only a layout that cannot be played is Blocking; a document that was read
        // back with a complaint is advisory, because the surface in front of the user is
        // fine.
        return state.Resolved.Fits ? TouchEditorSeverity.Advisory : TouchEditorSeverity.Blocking;
    }

    private static string Describe(
        TouchControllerProfile profile, IReadOnlyList<TouchControlInstance> instances) =>
        instances.Count switch
        {
            0 => "Nothing selected",
            1 => Name(profile, instances[0]),
            _ => $"{instances.Count} controls selected",
        };

    /// <summary>
    /// The control's name, the same way an audit finding names it.
    ///
    /// Through <see cref="TouchControlNaming"/> rather than off the catalog label, so the
    /// properties panel and the finding above it call the same control the same thing —
    /// including the copy number, which is the only thing distinguishing two instances of
    /// one catalog entry. A raw catalog id is a wire identifier, not a name.
    /// </summary>
    private static string Name(TouchControllerProfile profile, TouchControlInstance instance)
    {
        var entry = profile.CatalogEntry(instance.CatalogId);
        return TouchControlNaming.NameFor(
            entry is null ? null : profile.Bindings.GetValueOrDefault(entry.Output),
            entry?.Visual.Label ?? string.Empty,
            instance.InstanceId);
    }

    /// <summary>The multiplier the whole selection shares, or null when they differ.</summary>
    private static float? SharedScale(IReadOnlyList<TouchControlInstance> instances)
    {
        if (instances.Count == 0)
        {
            return null;
        }

        var first = instances[0].Scale;
        return instances.All(instance => MathF.Abs(instance.Scale - first) < 0.0005f)
            ? first
            : null;
    }

    /// <summary>
    /// Whether Group would do anything.
    ///
    /// Asked the same way the editor asks it — over the EXPANDED selection — so the button
    /// is enabled exactly when the operation would be accepted. Computing it differently
    /// here is how a button comes to be enabled for an operation that then refuses.
    /// </summary>
    private static bool Groupable(TouchLayoutDocument document, IReadOnlySet<string> selection) =>
        TouchLayoutEditor.Group(document, selection).Changed;
}
