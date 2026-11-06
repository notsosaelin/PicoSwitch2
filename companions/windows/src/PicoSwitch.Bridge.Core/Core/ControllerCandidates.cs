namespace PicoSwitch.Bridge.Core;

/// <summary>
/// One host input source the platform offered, reduced to the four signals the
/// bridge needs in order to decide whether it can drive a console.
///
/// Every desktop and mobile platform enumerates more "gamepads" than the user
/// physically owns — synthetic keyboards, mapping services, virtual devices left
/// behind by other software. Filling this in is a platform backend's job;
/// deciding what is usable is not, because the rule is the same everywhere and
/// getting it wrong either hides real hardware or offers a source that can never
/// produce input.
///
/// ## The case this rule was written from
///
/// Identified on Android, 2026-08-14: an entry named <c>Virtual</c> with VID/PID
/// <c>0000:0000</c> appeared even on a phone with no built-in controller at all,
/// which ruled out a game-assistant or touch-mapping service and identified it as
/// the platform's own virtual keyboard. It reached the list because it advertises
/// a D-pad source, and the enumeration accepted a D-pad alone as evidence of a
/// controller. It is not one, and it can never produce stick, trigger, or
/// gamepad-button input.
///
/// ## What the exclusion is based on (and what it deliberately is NOT)
///
/// No device-name blacklist: names are unstable across vendors and controller
/// modes (the same physical controls enumerate as <c>Odin Controller</c>, then
/// <c>Xbox Gamepad</c> after a mode switch). Three ordered signals, strongest
/// first:
///
///  1. <c>IsVirtual</c> — the platform's own "this device is synthetic"
///     classification, which is exactly what identifies the entry above.
///  2. No gamepad or joystick source. A real controller classifies as a gamepad
///     or joystick; a D-pad/keyboard classification alone describes a
///     keyboard-like device.
///  3. Anonymous AND capability-less: no VID/PID and no motion axes and no
///     gamepad buttons.
///
/// (3) requires both halves so legitimate unusual hardware stays visible: a
/// device with a real VID/PID is never hidden however odd its capabilities, and a
/// VID/PID-less device that genuinely reports sticks or buttons is kept too —
/// some kernel-level built-in controllers look like that. <c>IsVirtual</c> is
/// deliberately NOT inferred from being backed by a virtual kernel node: the
/// audited Retroid and AYN Thor built-in controllers are, and are classified
/// external, while being entirely real. A backend must pass through the
/// platform's classification rather than deriving one.
/// </summary>
public sealed record ControllerCandidate(
    int Id,
    string Descriptor,
    string Name,
    int VendorId,
    int ProductId,

    // Reports at least one stick/trigger motion axis.
    bool HasMotionAxes,

    // Reports at least one standard gamepad button.
    bool HasGamepadButtons,

    // The platform classifies this as a synthetic device. Pass it through; never
    // derive it.
    bool IsVirtual = false,

    // The platform classifies this as a gamepad or joystick. A D-pad/keyboard
    // classification alone does not count -- that is what let a virtual keyboard
    // into the list.
    bool HasGamepadSource = true)
{
    /// <summary>True when the device carries a real USB/Bluetooth identity.</summary>
    public bool HasRealIdentity => VendorId != 0 || ProductId != 0;

    /// <summary>True when the device demonstrates any usable controller capability.</summary>
    public bool HasAnyCapability => HasMotionAxes || HasGamepadButtons;

    /// <summary>
    /// Why this device was hidden, or null when it is usable. Surfaced in
    /// Diagnostics so a wrongly-hidden device can be identified from the field
    /// instead of guessed at.
    /// </summary>
    public string? ExclusionReason =>
        IsVirtual ? "The system reports this as a virtual device"
        : !HasGamepadSource ? "Not a gamepad or joystick (D-pad/keyboard source only)"
        : !HasRealIdentity && !HasAnyCapability
            ? "No VID/PID and no sticks or gamepad buttons (virtual or mapping device)"
        : null;

    public bool IsUsable => ExclusionReason is null;
}

public static class ControllerCandidates
{
    /// <summary>Devices that can actually drive the console, in enumeration order.</summary>
    public static IReadOnlyList<ControllerCandidate> Usable(
        IReadOnlyList<ControllerCandidate> candidates) =>
        candidates.Where(candidate => candidate.IsUsable).ToList();

    public static IReadOnlyList<ControllerCandidate> Excluded(
        IReadOnlyList<ControllerCandidate> candidates) =>
        candidates.Where(candidate => !candidate.IsUsable).ToList();

    /// <summary>
    /// The device to use without asking, or null when the user genuinely has a
    /// choice (or nothing usable).
    ///
    /// Exactly one usable controller is the overwhelmingly common case on a
    /// handheld, and making the user select the only possible option is pure
    /// friction. With two or more, the app must not guess.
    /// </summary>
    public static ControllerCandidate? AutoSelect(IReadOnlyList<ControllerCandidate> candidates) =>
        SingleOrNull(Usable(candidates));

    /// <summary>
    /// Resolve the selection for a refresh: keep the user's existing choice when
    /// it is still present and usable, otherwise auto-select when unambiguous.
    /// Returns null when the user must choose (or nothing is usable).
    /// </summary>
    public static ControllerCandidate? ResolveSelection(
        IReadOnlyList<ControllerCandidate> candidates,
        string? currentDescriptor)
    {
        var usable = Usable(candidates);
        var kept = usable.FirstOrDefault(candidate => candidate.Descriptor == currentDescriptor);
        return kept ?? SingleOrNull(usable);
    }

    /// <summary>True when selection UI is worth showing at all.</summary>
    public static bool NeedsUserChoice(IReadOnlyList<ControllerCandidate> candidates) =>
        Usable(candidates).Count > 1;

    // Kotlin's `singleOrNull()` returns null for a list of two or more rather than
    // throwing, which is the behaviour the auto-select rule depends on.
    // LINQ's SingleOrDefault throws instead, so the rule is spelled out here.
    private static ControllerCandidate? SingleOrNull(IReadOnlyList<ControllerCandidate> candidates) =>
        candidates.Count == 1 ? candidates[0] : null;
}
