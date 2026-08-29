using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>One key as the mapping grid draws it, with whatever it is bound to.</summary>
public sealed record KeyBindingCell(
    KeyCap Cap,
    KbmDestination Destination,
    bool Custom)
{
    public bool Bound => Destination != KbmDestination.None;

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
public sealed record MouseSlider(
    KbmMouseField Field,
    string Label,
    int Value,
    int Minimum,
    int Maximum,
    bool Available,
    string? Detail = null);

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

    public required KbmProfile Profile { get; init; }

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

    public required bool InvertX { get; init; }

    public required bool InvertY { get; init; }

    /// <summary>True once status has been read; an empty map before that is not "no bindings".</summary>
    public required bool Loaded { get; init; }

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
    public static KeyboardMouseView Project(
        KeyboardMouseState state,
        KbmProfile profile,
        bool connected)
    {
        var mapping = state.Mapping(profile);
        var byKey = mapping.Bindings
            .Where(binding => binding.Source.Kind == KbmSourceKind.Key)
            .ToDictionary(binding => binding.Source.Code);
        var byButton = mapping.Bindings
            .Where(binding => binding.Source.Kind == KbmSourceKind.MouseButton)
            .ToDictionary(binding => binding.Source.Code);

        var drawnKeys = KeyboardLayout.AllKeys.Select(cap => cap.Usage).ToHashSet();

        // Supported unless the adapter said otherwise. Unknown keeps the page.
        var supported = state.Capability != CapabilityState.Unsupported;

        return new KeyboardMouseView
        {
            Visible = supported,
            HiddenReason = supported
                ? null
                : "This adapter's firmware does not support keyboard and mouse input. " +
                  "Everything else in this app still works.",
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
            ShowMouseButtons = profile == KbmProfile.KeyboardMouse,

            // Everything the adapter reports that this build cannot draw. Listed
            // rather than dropped: a binding the user cannot see is one they cannot
            // remove, and a newer adapter is allowed to know keys this build does
            // not.
            Undrawn = mapping.Bindings
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
            MouseSliders = Sliders(state.Mouse),
            InvertX = state.Mouse.InvertX,
            InvertY = state.Mouse.InvertY,
            Loaded = state.Loaded,
        };
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
    private static IReadOnlyList<MouseSlider> Sliders(KbmMouseConfig mouse)
    {
        const string NoRange =
            "The adapter has not reported an adjustable range for this setting, so it " +
            "cannot be changed here.";

        return
        [
            new MouseSlider(
                KbmMouseField.Sensitivity,
                "Sensitivity",
                mouse.SensitivityX,
                mouse.SensitivityMin,
                mouse.SensitivityMax,
                mouse.Ranged,
                mouse.Ranged ? $"×{mouse.Multiplier(mouse.SensitivityX):0.00}" : NoRange),
            new MouseSlider(
                KbmMouseField.VelocityWindow,
                "Smoothing window",
                mouse.VelocityWindowMs,
                mouse.VelocityWindowMinMs,
                mouse.VelocityWindowMaxMs,
                mouse.VelocityWindowMaxMs > mouse.VelocityWindowMinMs,
                mouse.VelocityWindowMaxMs > mouse.VelocityWindowMinMs
                    ? $"{mouse.VelocityWindowMs} ms"
                    : NoRange),
            new MouseSlider(
                KbmMouseField.AntiDeadzone,
                "Anti-deadzone",
                mouse.AntiDeadzone,
                0,
                mouse.AntiDeadzoneMax,
                mouse.AntiDeadzoneMax > 0,
                mouse.AntiDeadzoneMax > 0 ? mouse.AntiDeadzone.ToString() : NoRange),
        ];
    }

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
