using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>One key as the mapping grid draws it, with whatever it is bound to.</summary>
public sealed record KeyBindingCell(
    KeyCap Cap,
    KbmDestination Destination,
    bool Custom)
{
    public bool Bound => Destination != KbmDestination.None;

    /// <summary>
    /// Whether a quick "clear this binding" gesture does anything here.
    ///
    /// Only a bound input can be cleared. Letting the gesture fire on an unmapped
    /// key would mark the draft dirty with a change that alters nothing, so Save
    /// would light up for an edit the user cannot see.
    /// </summary>
    public bool CanClear => Bound;

    /// <summary>The adapter's default, changed by the user. Worth showing as different.</summary>
    public bool Overridden => Custom;

    public string DestinationLabel => KeyboardMouseView.Describe(Destination);

    /// <summary>
    /// Screen readers get the whole fact, because the visual version of it is a
    /// key label with a smaller word underneath and a colour — none of which
    /// survives being read aloud.
    /// </summary>
    public string AccessibleName => Bound
        ? $"{Cap.Label}, mapped to {DestinationLabel}" + (Custom ? ", changed" : string.Empty)
        : $"{Cap.Label}, not mapped";

    /// <summary>
    /// The hover tooltip: what this key does, plus the shortcut for clearing it.
    ///
    /// The shortcut is advertised only where it applies. A middle-click hint on a
    /// key that has nothing to clear teaches the gesture wrong.
    /// </summary>
    public string Tooltip => CanClear
        ? $"{AccessibleName}\nMiddle-click to clear"
        : AccessibleName;
}

/// <summary>One mouse-tuning slider, with the range the ADAPTER reported.</summary>
/// <param name="Available">
/// False when there is no usable range to drag along, which happens two ways and
/// the distinction is worth knowing:
///
/// - **Nothing read yet.** The default <c>KbmMouseConfig</c> is all zeroes, so
///   every range is degenerate until <c>kbm mouse</c> answers. This is the only
///   way SENSITIVITY can be unavailable: its decoder requires
///   <c>sensitivityMax &gt; sensitivityMin</c>, so any reply that parses carries a
///   real range.
/// - **A genuinely degenerate range.** The decoder allows
///   <c>recenterMaxMs == recenterMinMs</c> and <c>antiDeadzoneMax == 0</c>, so an
///   adapter really can report those two as not adjustable.
///
/// Either way, inventing client-side bounds would let the user drag to a value
/// the adapter clamps — a slider that springs back with no explanation.
/// </param>
/// <param name="ValueText">
/// The current value, formatted for the readout column. Kept separate from
/// <paramref name="Description"/> so the page can right-align a short value
/// beside the slider and put the explanation underneath: the two were one field,
/// which is why a value and a paragraph competed for the same line.
/// </param>
/// <param name="Description">
/// What the setting actually changes, in one line. Mouse tuning is the one part
/// of this page where the control names genuinely do not explain themselves —
/// "anti-deadzone" means nothing without knowing there is a stick underneath.
/// </param>
public sealed record MouseSlider(
    KbmMouseField Field,
    string Label,
    int Value,
    int Minimum,
    int Maximum,
    bool Available,
    string ValueText,
    string Description);

/// <summary>One mouse-tuning toggle, with the same one-line explanation.</summary>
public sealed record MouseToggle(
    KbmMouseField Field,
    string Label,
    bool Value,
    string Description);

/// <summary>
/// The adapter's KB/M ingress counters, as three readable lines.
///
/// These exist because a keyboard that produces nothing at the console is
/// otherwise indistinguishable from a keyboard that is not sending anything at
/// all. Every one of these numbers was already in the <c>kbm status</c> reply
/// and already parsed; none of it was ever shown.
///
/// Read the counter NAMES literally, because two of them do not mean what they
/// look like:
///
/// - <c>KeyboardReports</c> and <c>MouseReports</c> count ACCEPTED reports, not
///   arriving ones. The firmware increments them only after admission succeeds
///   (<c>ns2_kbm_runtime_submit_keyboard()</c> does it after
///   <c>admit_and_route()</c> has already returned true). So a report that is
///   refused increments a rejection counter and NOT this one, and a report
///   dropped by one of admission's silent exits increments nothing whatsoever.
/// - <c>Publishes</c> counts states pushed toward the console. It is the only
///   number here that says the output side ran.
///
/// <c>SourceId</c> is zero while the composite does not own the console slot,
/// which is the condition <c>RejectedNotOwner</c> counts against.
/// </summary>
public sealed record KbmRuntimeCounters(
    string Roles,
    string Accepted,
    string Rejected)
{
    public string Text => string.Join(Environment.NewLine, Roles, Accepted, Rejected);
}

public sealed record KeyboardMouseView
{
    public required SectionAvailability Availability { get; init; }

    /// <summary>False only when the adapter explicitly reports no KB/M support.</summary>
    public required bool Visible { get; init; }

    public string? HiddenReason { get; init; }

    public required KbmMode Mode { get; init; }

    public required KbmMode ModeOverride { get; init; }

    /// <summary>What the adapter is doing now, which is not always what was asked for.</summary>
    public required string ModeText { get; init; }

    public required string DevicesText { get; init; }

    public required bool KeyboardConnected { get; init; }

    public required bool MouseConnected { get; init; }

    /// <summary>The LAYOUT being edited — the shape of the mapping.</summary>
    public required KbmLayout Profile { get; init; }

    /// <summary>
    /// Every profile of the layout being edited, built-in Default first.
    ///
    /// Never empty when <see cref="Readiness"/> is Ready: Default always exists,
    /// and an adapter that cannot list profiles is not Ready.
    /// </summary>
    public required IReadOnlyList<KbmProfileInfo> Profiles { get; init; }

    /// <summary>
    /// The page's top-level state. The profile workflow is reachable in exactly
    /// one of them.
    /// </summary>
    public required KeyboardMouseReadiness Readiness { get; init; }

    /// <summary>What to say when <see cref="Readiness"/> is not Ready.</summary>
    public required string NotReadyTitle { get; init; }

    public required string NotReadyDetail { get; init; }

    /// <summary>
    /// The mapping editor and the local library, which need NO adapter.
    /// </summary>
    /// <remarks>
    /// Always true. The library belongs to the user, so creating, editing and
    /// saving a profile must work with nothing paired — the canonical table the
    /// grid draws against is embedded, not fetched. Gating this on a connection
    /// is what made an unplugged app useless for the one task that never needed
    /// a device.
    ///
    /// A FAILED READ is different from being offline and still hides the editor:
    /// see <see cref="ShowNotReady"/>.
    /// </remarks>
    public bool ShowEditor => Readiness != KeyboardMouseReadiness.Error &&
                              Readiness != KeyboardMouseReadiness.FirmwareUpdateRequired;

    /// <summary>
    /// The adapter-only half: the bank, activation, switch keys.
    /// </summary>
    /// <remarks>
    /// These genuinely require a live session, so they are disabled rather than
    /// hidden — a user who cannot see them cannot tell that connecting would
    /// bring them back.
    /// </remarks>
    public bool AdapterAvailable => Readiness == KeyboardMouseReadiness.Ready &&
                                    Availability.Enabled;

    public bool ShowNotReady => Readiness is KeyboardMouseReadiness.Error or
                                             KeyboardMouseReadiness.FirmwareUpdateRequired;

    /// <summary>What the bank card says when there is nothing to talk to.</summary>
    public string? AdapterUnavailableReason => AdapterAvailable
        ? null
        : Readiness == KeyboardMouseReadiness.Ready
            ? "Connect the adapter to assign profiles to it."
            : "Connect the adapter to see and manage its profiles.";

    /// <summary>Kept for the profile controls' own enablement rules.</summary>
    public bool ProfilesSupported => true;

    /// <summary>Which profile the editor currently has open. Never null when supported.</summary>
    public KbmProfileInfo? SelectedProfile { get; init; }

    /// <summary>What the console is really running for this layout.</summary>
    public KbmActiveMapping? ActiveMapping { get; init; }

    /// <summary>Where the local draft stands against adapter truth.</summary>
    public required KbmDraftState DraftState { get; init; }

    public bool Dirty => DraftState == KbmDraftState.Dirty;

    /// <summary>
    /// Saved to the library, but the console is still running something else.
    /// The state that exists because Save and Apply are different acts.
    /// </summary>
    public bool SavedNotApplied => DraftState == KbmDraftState.SavedNotApplied;

    public bool Conflicted => DraftState == KbmDraftState.Conflict;

    /// <summary>
    /// Save is offered for a real edit, and for turning the built-in Default
    /// into a profile of the user's own. It is never offered for an unchanged
    /// draft, because there would be nothing to write.
    /// </summary>
    /// <remarks>
    /// Asked of the DRAFT, not of a matching resident profile. Save is a local
    /// operation and must be offered with nothing connected; deriving it from the
    /// adapter's profile list made it depend on a session it never needed.
    /// </remarks>
    public bool CanSave => DraftState == KbmDraftState.Dirty || EditingBuiltin;

    /// <summary>Whether the open draft is the built-in template.</summary>
    public required bool EditingBuiltin { get; init; }

    /// <summary>
    /// Apply is offered whenever the selected profile is not what the console is
    /// running. Never automatic: applying is the user saying "use this now".
    /// </summary>
    public bool CanApply =>
        ProfilesSupported && SelectedProfile is not null &&
        DraftState is not (KbmDraftState.Disconnected or KbmDraftState.Dirty) &&
        DraftState != KbmDraftState.Active;

    /// <remarks>
    /// Local operations on a local profile, so they need no connection. Only the
    /// built-in template is excluded: it is not a library profile and has nothing
    /// to rename or delete.
    /// </remarks>
    public bool CanRename => !EditingBuiltin;

    public bool CanDelete => CanRename;

    /// <summary>One line saying exactly where this profile stands.</summary>
    public string StatusText => DraftState switch
    {
        KbmDraftState.Active => "Active",
        KbmDraftState.Dirty => "Unsaved changes",
        KbmDraftState.SavedNotApplied => "Saved — not applied",
        KbmDraftState.Conflict => "Changed on another device",
        KbmDraftState.Disconnected => "Not connected",
        _ => "Saved",
    };

    /// <summary>The explanation a one-word status cannot carry.</summary>
    public string? StatusDetail => DraftState switch
    {
        KbmDraftState.Dirty =>
            "Nothing has been sent to the adapter yet. Save to store these " +
            "changes, or Discard to go back to the saved profile.",
        KbmDraftState.SavedNotApplied =>
            "The adapter has these changes saved, but the console is still " +
            "using the mapping that was applied earlier. Set Active to use them.",
        KbmDraftState.Conflict =>
            "This profile was changed from another device since you opened it. " +
            "Reload to see those changes, or save this as a new profile.",
        KbmDraftState.Disconnected =>
            "Showing the last mapping read from this adapter. What the console " +
            "is using right now cannot be confirmed while disconnected.",
        _ => null,
    };

    /// <summary>
    /// The LAYOUT the adapter is actually resolving against right now.
    ///
    /// Not user-settable, and deliberately so: the adapter derives it from which
    /// roles are filled. A keyboard alone resolves the Keyboard layout; a
    /// keyboard and a mouse resolve the Keyboard-and-mouse one. Asserting the
    /// latter with no mouse would silently drop the right stick.
    ///
    /// Which PROFILE is used within that layout is the user's choice, and that
    /// is what the selector changes.
    /// </summary>
    public required KbmLayout ActiveLayout { get; init; }

    /// <summary>
    /// True when the user is editing a layout the adapter is not using.
    ///
    /// This is worth saying out loud. A binding made here is saved and survives
    /// a reload, so every management operation reports success — and the key
    /// does nothing at the console, because the adapter is resolving the other
    /// layout. Silence there is indistinguishable from a broken keyboard.
    /// </summary>
    public bool EditingInactiveProfile => Profile != ActiveLayout;

    public string? InactiveProfileWarning => EditingInactiveProfile
        ? $"The adapter is currently using the {Label(ActiveLayout)}. Changes here " +
          "are saved, but will not affect the console until that layout is in use."
        : null;

    public static string Label(KbmLayout profile) => profile == KbmLayout.Keyboard
        ? "keyboard profile"
        : "keyboard and mouse profile";

    /// <summary>Every drawn key, flat. Used for counting and lookup.</summary>
    public required IReadOnlyList<KeyBindingCell> Keys { get; init; }

    /// <summary>The same cells grouped by physical cluster, for drawing.</summary>
    public required IReadOnlyList<KeyClusterCells> Clusters { get; init; }

    /// <summary>
    /// ISO and Japanese keys, which an ANSI picture has no honest position for.
    ///
    /// Kept separate rather than omitted: a European or Japanese keyboard really
    /// has these, and a key that cannot be drawn is still a key the user may want
    /// to bind.
    /// </summary>
    public required KeyClusterCells OtherKeys { get; init; }

    public required IReadOnlyList<KeyBindingCell> MouseButtons { get; init; }

    /// <summary>
    /// Whether mouse buttons are part of THIS profile.
    ///
    /// The keyboard profile is what the adapter uses when only a keyboard is
    /// attached, so a mouse button in it can never fire. Drawing five dead
    /// controls invites the user to bind something that will silently do nothing.
    /// </summary>
    public required bool ShowMouseButtons { get; init; }

    /// <summary>Bindings the adapter reports for keys this build does not draw.</summary>
    public required IReadOnlyList<KeyBindingCell> Undrawn { get; init; }

    public required IReadOnlyList<MouseSlider> MouseSliders { get; init; }

    /// <summary>The invert switches, carrying their own explanations.</summary>
    public required IReadOnlyList<MouseToggle> MouseToggles { get; init; }

    public required bool InvertX { get; init; }

    public required bool InvertY { get; init; }

    /// <summary>True once status has been read; an empty map before that is not "no bindings".</summary>
    public required bool Loaded { get; init; }

    /// <summary>
    /// Runtime ingress counters. Null until something has been read.
    /// </summary>
    public KbmRuntimeCounters? Counters { get; init; }

    /// <summary>Bound inputs, counting only what this profile actually shows.</summary>
    public int BoundCount =>
        Keys.Count(cell => cell.Bound) +
        (ShowMouseButtons ? MouseButtons.Count(cell => cell.Bound) : 0);

    /// <summary>The denominator behind it, so the two always agree.</summary>
    public int MappableCount => Keys.Count + (ShowMouseButtons ? MouseButtons.Count : 0);

    public static string Describe(KbmDestination destination) => destination switch
    {
        KbmDestination.None => "Not mapped",
        KbmDestination.A => "A", KbmDestination.B => "B",
        KbmDestination.X => "X", KbmDestination.Y => "Y",
        KbmDestination.L => "L", KbmDestination.R => "R",
        KbmDestination.Zl => "ZL", KbmDestination.Zr => "ZR",
        KbmDestination.Gl => "GL", KbmDestination.Gr => "GR",
        KbmDestination.L3 => "Left stick click", KbmDestination.R3 => "Right stick click",
        KbmDestination.DUp => "D-pad up", KbmDestination.DDown => "D-pad down",
        KbmDestination.DLeft => "D-pad left", KbmDestination.DRight => "D-pad right",
        KbmDestination.Minus => "Minus", KbmDestination.Plus => "Plus",
        KbmDestination.Home => "Home", KbmDestination.Capture => "Capture",
        KbmDestination.C => "C",
        KbmDestination.LStickUp => "Left stick up", KbmDestination.LStickDown => "Left stick down",
        KbmDestination.LStickLeft => "Left stick left", KbmDestination.LStickRight => "Left stick right",
        KbmDestination.RStickUp => "Right stick up", KbmDestination.RStickDown => "Right stick down",
        KbmDestination.RStickLeft => "Right stick left", KbmDestination.RStickRight => "Right stick right",
        _ => destination.ToString(),
    };
}

/// <summary>One cluster's cells, with the geometry needed to draw it.</summary>
public sealed record KeyClusterCells(KeyboardCluster Cluster, IReadOnlyList<KeyBindingCell> Cells)
{
    public string Name => Cluster.Name;

    public double Columns => Cluster.Columns;

    public double Rows => Cluster.Rows;
}

public static class KeyboardMouse
{
    /// <param name="draft">
    /// The editor's local copy, when one is open. The grid renders the DRAFT
    /// rather than the adapter's stored mapping, which is what lets an edit show
    /// immediately without any adapter write.
    /// </param>
    public static KeyboardMouseView Project(
        KeyboardMouseState state,
        KbmLayout profile,
        bool connected,
        KbmLocalDraft? draft = null)
    {
        // The LOCAL draft's effective mapping, composed from the embedded
        // canonical table -- so the grid draws with no adapter connected.
        var bindings = draft is not null
            ? draft.Effective
            : state.Mapping(profile).Bindings;
        var byKey = bindings
            .Where(binding => binding.Source.Kind == KbmSourceKind.Key)
            .ToDictionary(binding => binding.Source.Code);
        var byButton = bindings
            .Where(binding => binding.Source.Kind == KbmSourceKind.MouseButton)
            .ToDictionary(binding => binding.Source.Code);

        // The adapter's RESIDENT profiles, which the bank card lists. The picker
        // lists the LOCAL library instead; these two were the same list, and
        // that is precisely what made creating a profile write to the adapter.
        var profileRows = state.Profiles.For(profile);
        var active = state.Profiles.ActiveFor(profile);
        var selected = draft is not null
            ? profileRows.FirstOrDefault(row => row.Fingerprint == draft.Fingerprint)
            : profileRows.FirstOrDefault(row => row.Id == active?.SourceId);

        // The draft's state is a LOCAL question -- has it been edited since it
        // was saved to the library? -- and is answerable with nothing connected.
        var draftState = draft is null
            ? KbmDraftState.Clean
            : draft.Dirty
                ? KbmDraftState.Dirty
                : KbmDraftState.Clean;

        var drawnKeys = KeyboardLayout.AllKeys.Select(cap => cap.Usage).ToHashSet();

        // The page is always REACHABLE; what it shows depends on readiness. It
        // is never replaced by a pre-profile editor, and it never silently shows
        // a mapping grid it could not fully load.
        var (title, detail) = state.Readiness switch
        {
            KeyboardMouseReadiness.FirmwareUpdateRequired =>
                ("Firmware update required",
                 "This adapter's firmware predates the keyboard and mouse profile " +
                 "system, so this page cannot configure it. Update the adapter, " +
                 "then reopen this page. " + state.Fault),
            KeyboardMouseReadiness.Error =>
                ("The adapter's reply could not be used",
                 "The adapter implements this feature but returned data this app " +
                 "could not read completely. " + state.Fault),
            KeyboardMouseReadiness.Loading => ("Loading…", string.Empty),
            KeyboardMouseReadiness.Ready => (string.Empty, string.Empty),
            _ => ("Not read yet", "Reload to read this adapter's keyboard and mouse settings."),
        };

        return new KeyboardMouseView
        {
            Readiness = state.Readiness,
            NotReadyTitle = title,
            NotReadyDetail = detail,
            Visible = true,
            HiddenReason = null,
            Availability = !connected
                ? SectionAvailability.Disabled(AdapterDashboard.NotConnected)
                : SectionAvailability.Available,
            Mode = state.Status.Mode,
            ModeOverride = state.Status.ModeOverride,
            ModeText = ModeText(state.Status),
            DevicesText = DevicesText(state.Status),
            KeyboardConnected = state.Status.KeyboardConnected,
            MouseConnected = state.Status.MouseConnected,
            Profile = profile,
            ActiveLayout = state.Status.Profile,
            Profiles = profileRows,
            SelectedProfile = selected,
            EditingBuiltin = draft?.IsBuiltin ?? true,
            ActiveMapping = state.Profiles.ActiveFor(profile),
            DraftState = draftState,
            Keys = KeyboardLayout.AllKeys.Select(cap => Cell(cap, byKey)).ToArray(),
            Clusters = KeyboardLayout.Clusters
                .Select(cluster => new KeyClusterCells(
                    cluster,
                    cluster.Keys.Select(cap => Cell(cap, byKey)).ToArray()))
                .ToArray(),
            OtherKeys = new KeyClusterCells(
                KeyboardLayout.Other,
                KeyboardLayout.Other.Keys.Select(cap => Cell(cap, byKey)).ToArray()),
            MouseButtons = KeyboardLayout.MouseButtons
                .Select(cap => Cell(cap, byButton))
                .ToArray(),

            // Only the keyboard-and-mouse profile has a mouse to press.
            ShowMouseButtons = profile == KbmLayout.KeyboardMouse,

            // Everything the adapter reports that this build cannot draw. Listed
            // rather than dropped: a binding the user cannot see is one they cannot
            // remove, and a newer adapter is allowed to know keys this build does
            // not.
            Undrawn = bindings
                .Where(binding =>
                    binding.Source.Kind == KbmSourceKind.Key &&
                    !drawnKeys.Contains(binding.Source.Code) &&
                    binding.Destination != KbmDestination.None)
                .Select(binding => new KeyBindingCell(
                    // Position is meaningless for a key with no drawn place; the
                    // undrawn list is a plain list, not a picture.
                    new KeyCap(binding.Source.Code, KeyboardLayout.Describe(binding.Source), 0, 0),
                    binding.Destination,
                    binding.Custom))
                .ToArray(),
            // Sliders follow the DRAFT when one is open, so dragging one shows
            // immediately and costs no adapter write. The RANGES still come from
            // the adapter's reply -- inventing client-side bounds would let the
            // user drag to a value the adapter clamps.
            MouseSliders = Sliders(draft is not null
                                       ? state.Mouse with
                                         {
                                             SensitivityX = draft.Mouse.SensitivityX,
                                             SensitivityY = draft.Mouse.SensitivityY,
                                             VelocityWindowMs = draft.Mouse.VelocityWindowMs,
                                             AntiDeadzone = draft.Mouse.AntiDeadzone,
                                         }
                                       : state.Mouse),
            MouseToggles = Toggles(draft?.Mouse.InvertX ?? state.Mouse.InvertX,
                                   draft?.Mouse.InvertY ?? state.Mouse.InvertY),
            InvertX = draft?.Mouse.InvertX ?? state.Mouse.InvertX,
            InvertY = draft?.Mouse.InvertY ?? state.Mouse.InvertY,
            Loaded = state.Loaded,
            Counters = state.Loaded ? Counters(state.Status) : null,
        };
    }

    /// <summary>
    /// Format the ingress counters for reading, in the order a report travels:
    /// who holds which role, what was accepted and published, what was refused.
    ///
    /// Deliberately plain text rather than a table. It is meant to be read at a
    /// glance and pasted into a bug report, and the useful measurement is a
    /// DIFFERENCE -- read it, press one key, read it again.
    /// </summary>
    internal static KbmRuntimeCounters Counters(KbmStatus status)
    {
        var keyboard = status.KeyboardConnected ? $"yes (conn {status.KeyboardConn})" : "no";
        var mouse = status.MouseConnected ? $"yes (conn {status.MouseConn})" : "no";

        return new KbmRuntimeCounters(
            $"mode={KbmModes.Wire(status.Mode)} profile={KbmLayouts.Wire(status.Profile)} " +
            $"keyboard={keyboard} mouse={mouse} group={status.GroupId} source={status.SourceId}",

            $"accepted(keyboard={status.KeyboardReports} mouse={status.MouseReports}) " +
            $"published={status.Publishes} rollover={status.Rollover}",

            $"rejected(mode={status.RejectedMode} duplicate={status.RejectedDuplicate} " +
            $"notOwner={status.RejectedNotOwner} noPeerKey={status.RejectedNoPeerKey} " +
            $"unclassified={status.RejectedUnclassified} noRole={status.RejectedNoRole}) " +
            $"undecoded={status.UndecodedReports} roleLosses={status.RoleLosses}");
    }

    private static KeyBindingCell Cell(KeyCap cap, IReadOnlyDictionary<int, KbmBinding> bound) =>
        bound.TryGetValue(cap.Usage, out var binding)
            ? new KeyBindingCell(cap, binding.Destination, binding.Custom)
            : new KeyBindingCell(cap, KbmDestination.None, Custom: false);

    /// <summary>
    /// The three tuning sliders, bounded by what the ADAPTER reported.
    ///
    /// §16.3: "Slider bounds must come from the reply." Client-side constants
    /// would drift from firmware the moment either side changed, and the failure
    /// is silent — the adapter clamps, the slider springs back, and nothing says
    /// why.
    /// </summary>
    /// <summary>
    /// The mouse-tuning block.
    /// </summary>
    /// <remarks>
    /// These carry real explanations, unlike the rest of the page. The mapping
    /// grid explains itself — a key with a controller button written on it needs
    /// no paragraph — but "smoothing window" and "anti-deadzone" describe an
    /// analogue-stick emulation the user cannot see, and without one line each
    /// they are three sliders to be moved at random.
    ///
    /// Ranges come from the adapter, never invented here: a client-side bound
    /// would let the user drag to a value the firmware clamps, and the slider
    /// would spring back with no explanation.
    /// </remarks>
    private static IReadOnlyList<MouseSlider> Sliders(KbmMouseConfig mouse)
    {
        const string NoRange = "Not adjustable on this firmware";

        var smoothingRanged = mouse.VelocityWindowMaxMs > mouse.VelocityWindowMinMs;
        var antiDeadzoneRanged = mouse.AntiDeadzoneMax > 0;

        return
        [
            new MouseSlider(
                KbmMouseField.Sensitivity,
                "Sensitivity",
                mouse.SensitivityX,
                mouse.SensitivityMin,
                mouse.SensitivityMax,
                mouse.Ranged,
                mouse.Ranged ? $"×{mouse.Multiplier(mouse.SensitivityX):0.00}" : NoRange,
                "How far the stick pushes for a given amount of mouse movement. " +
                "Raise it to turn faster with a smaller hand movement."),
            new MouseSlider(
                KbmMouseField.VelocityWindow,
                "Smoothing window",
                mouse.VelocityWindowMs,
                mouse.VelocityWindowMinMs,
                mouse.VelocityWindowMaxMs,
                smoothingRanged,
                smoothingRanged ? $"{mouse.VelocityWindowMs} ms" : NoRange,
                "How long mouse movement is averaged over. Higher is steadier but " +
                "less responsive; lower reacts instantly and can feel jittery."),
            new MouseSlider(
                KbmMouseField.AntiDeadzone,
                "Anti-deadzone",
                mouse.AntiDeadzone,
                0,
                mouse.AntiDeadzoneMax,
                antiDeadzoneRanged,
                antiDeadzoneRanged ? mouse.AntiDeadzone.ToString() : NoRange,
                "Games ignore small stick movements. This adds a minimum push so " +
                "slow mouse movement still registers. Too high and the aim drifts."),
        ];
    }

    private static IReadOnlyList<MouseToggle> Toggles(bool invertX, bool invertY) =>
    [
        new MouseToggle(KbmMouseField.InvertX, "Invert horizontal", invertX,
                        "Moving the mouse left aims right."),
        new MouseToggle(KbmMouseField.InvertY, "Invert vertical", invertY,
                        "Moving the mouse forward aims down, like an aircraft stick."),
    ];

    /// <summary>
    /// What the adapter is actually doing, and whether that is what was asked.
    ///
    /// Mode and ModeOverride are separate facts. An override that has not taken
    /// effect — because the device it needs is not connected — must not read as
    /// the active mode, or the user tunes a profile the adapter is not using.
    /// </summary>
    private static string ModeText(KbmStatus status)
    {
        var active = $"Using {Label(status.Mode)}";
        return status.ModeOverride == KbmMode.Automatic || status.ModeOverride == status.Mode
            ? active
            : $"{active} · set to {Label(status.ModeOverride)}, waiting for the device";
    }

    private static string DevicesText(KbmStatus status) => (status.KeyboardConnected, status.MouseConnected) switch
    {
        (true, true) => "Keyboard and mouse connected",
        (true, false) => "Keyboard connected",
        (false, true) => "Mouse connected",

        // Not an error: KB/M can be configured with nothing plugged in, and the
        // mapping is what is being edited, not the live devices.
        _ => "No keyboard or mouse connected to the adapter",
    };

    public static string Label(KbmMode mode) => mode switch
    {
        KbmMode.Automatic => "automatic mode",
        KbmMode.Controller => "controller only",
        KbmMode.Keyboard => "keyboard",
        _ => "keyboard and mouse",
    };
}
