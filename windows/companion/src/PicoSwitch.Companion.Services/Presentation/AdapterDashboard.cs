using PicoSwitch.Bridge.Protocol;
using PicoSwitch.Companion.Windows.Bluetooth;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// Whether a dashboard section can be used, and if not, why.
///
/// A disabled control with no reason is the worst of both worlds: the user
/// cannot act and cannot find out why not. Every section that can be disabled
/// carries the sentence explaining it, and the sentence is produced here rather
/// than in XAML so it can be asserted.
/// </summary>
public sealed record SectionAvailability(bool Enabled, string? DisabledReason)
{
    public static readonly SectionAvailability Available = new(true, null);

    public static SectionAvailability Disabled(string reason) => new(false, reason);
}

/// <summary>
/// The bridge-contract verdict, ready to render.
///
/// <see cref="Visible"/> IS the silence rule (I15). <c>Pending</c> — "we have not
/// asked yet" — must stay silent, because it is a transient state on every
/// healthy connection and warning during it trains people to ignore the warning
/// that matters. <c>Unknown</c> — "we asked, and this firmware reports no
/// contract" — must warn. Collapsing the two made a healthy adapter flash an
/// incompatibility warning for the second before its `info` reply landed.
/// </summary>
public sealed record ContractVerdict(bool Visible, bool IsWarning, string Summary)
{
    public static ContractVerdict Of(BridgeContract.Compatibility compatibility) => new(
        Visible: BridgeContract.WarrantsWarning(compatibility),
        IsWarning: BridgeContract.WarrantsWarning(compatibility),
        Summary: compatibility.Summary);
}

public sealed record ControllerModeSection(
    SectionAvailability Availability,
    Personality Current,
    IReadOnlyList<Personality> Available)
{
    public string CurrentLabel => Label(Current);

    /// <summary>
    /// The product names, not the wire names.
    ///
    /// `joycon2_l` is a protocol token; nobody owns a controller called that.
    /// Kept here rather than in Management.Core because the wire vocabulary is
    /// protocol truth and must not drift toward whatever reads nicely this year.
    /// </summary>
    public static string Label(Personality value) => value switch
    {
        Personality.Pro2 => "Pro Controller 2",
        Personality.GameCube => "GameCube controller",
        Personality.JoyConLeft => "Joy-Con 2 (L)",
        Personality.JoyConRight => "Joy-Con 2 (R)",
        _ => "Unknown",
    };
}

public sealed record AppearanceSection(
    SectionAvailability Availability,
    RgbColor Body,
    RgbColor LeftAccent,
    RgbColor RightAccent)
{
    /// <summary>
    /// Colours are not host-visible until the adapter re-enumerates (I7), so the
    /// page must offer that as an explicit action rather than pretending a saved
    /// colour is a live one.
    /// </summary>
    public string ApplyHint =>
        "Colours are saved to the adapter immediately, but the console only sees " +
        "them after the adapter re-enumerates.";
}

public sealed record InputSourceChoice(long Id, string Label, bool IsActive);

public sealed record ConsoleInputSection(
    SectionAvailability Availability,
    IReadOnlyList<InputSourceChoice> Sources,
    long ActiveId,
    bool Truncated)
{
    public string ActiveLabel =>
        Sources.FirstOrDefault(source => source.IsActive)?.Label ?? "None";
}

/// <summary>
/// Everything the adapter dashboard renders, derived from state and nothing else.
///
/// A pure projection rather than an observable view-model object: every input is
/// already an observable owned by <see cref="AdapterConnectionService"/>, and
/// re-deriving the whole view on change is both simpler and exhaustively
/// testable. The page's only job is to paint this record.
/// </summary>
public sealed record AdapterDashboardView
{
    public required string Title { get; init; }

    public required string PhaseText { get; init; }

    public string? PhaseDetail { get; init; }

    public required bool Connected { get; init; }

    /// <summary>Empty when nothing has been read; never a placeholder version.</summary>
    public required string FirmwareLine { get; init; }

    public required ContractVerdict Contract { get; init; }

    public required string ControllerLine { get; init; }

    public string? BatteryLine { get; init; }

    public required ControllerModeSection ControllerMode { get; init; }

    public required AppearanceSection Appearance { get; init; }

    public required ConsoleInputSection ConsoleInput { get; init; }

    /// <summary>
    /// Home / Capture / C, held while pressed.
    ///
    /// Always disabled in Phase 3: these are Controller Link actions and require a
    /// live bridge session, which is Phase 6 and gated on the HOGP peripheral-role
    /// experiment (§14.5). Rendered-but-disabled with the real reason rather than
    /// hidden, because "this PC cannot act as a controller" is exactly the fact a
    /// user needs to discover before filing a bug about it.
    /// </summary>
    public required SectionAvailability ConsoleButtons { get; init; }
}

public static class AdapterDashboard
{
    public const string NotConnected = "Connect an adapter to change this.";

    /// <summary>
    /// Build the whole view.
    ///
    /// Capability rules that are load-bearing:
    ///
    /// - <c>Unsupported</c> disables a section and says which firmware feature is
    ///   missing;
    /// - <c>Unknown</c> does **NOT**. "The probe did not establish an answer" and
    ///   "this firmware does not have that command" are different statements, and
    ///   only the second may disable a control. Rendering Unknown as Unsupported
    ///   would hide working features behind a probe that merely timed out.
    /// </summary>
    public static AdapterDashboardView Project(
        AdapterSnapshot snapshot,
        AdapterRelationshipStatus relationship,
        ConnectionState connection,
        AdapterRecord? selected)
    {
        var connected = connection.Connected;
        var firmware = snapshot.Firmware;

        // Pending is "the identity reply has not arrived", which is precisely
        // "no product name yet" -- not "this adapter has no firmware version".
        var infoAvailable = !string.IsNullOrWhiteSpace(firmware.Id);

        return new AdapterDashboardView
        {
            Title = selected?.DisplayName ?? connection.DeviceName ?? "No adapter",
            PhaseText = PhaseText(relationship.Phase),
            PhaseDetail = relationship.Message,
            Connected = connected,
            FirmwareLine = FirmwareLine(firmware),
            Contract = ContractVerdict.Of(
                BridgeContract.Evaluate(firmware.BridgeContract, connected, infoAvailable)),
            ControllerLine = snapshot.Controller.Attached
                ? snapshot.Controller.Name
                : "No controller",
            BatteryLine = BatteryLine(snapshot.Controller),
            ControllerMode = new ControllerModeSection(
                Gate(connected, snapshot.Capabilities.Personality, "switch the emulated controller"),
                snapshot.Personality.Current,
                snapshot.Personality.Available.ToArray()),
            Appearance = new AppearanceSection(
                Gate(connected, snapshot.Capabilities.Colors, "change its colours"),
                snapshot.Config.BodyColor,
                snapshot.Config.LeftAccent,
                snapshot.Config.RightAccent),
            ConsoleInput = new ConsoleInputSection(
                Gate(connected, snapshot.Capabilities.ActiveInput, "choose the console input source"),
                snapshot.Input.Sources
                    .Select(source => new InputSourceChoice(
                        source.Id,
                        SourceLabel(source),
                        source.Id == snapshot.Input.ActiveId))
                    .ToArray(),
                snapshot.Input.ActiveId,
                snapshot.Input.Truncated),
            ConsoleButtons = SectionAvailability.Disabled(
                "Console buttons need this PC acting as a controller, which is not " +
                "available yet."),
        };
    }

    /// <summary>
    /// Disabled when not connected, or when the adapter's firmware explicitly
    /// lacks the feature. Never disabled merely because a probe failed.
    /// </summary>
    private static SectionAvailability Gate(bool connected, CapabilityState capability, string action)
    {
        if (!connected)
        {
            return SectionAvailability.Disabled(NotConnected);
        }

        return capability == CapabilityState.Unsupported
            ? SectionAvailability.Disabled($"This adapter's firmware cannot {action}.")
            : SectionAvailability.Available;
    }

    private static string FirmwareLine(FirmwareInfo firmware)
    {
        if (string.IsNullOrWhiteSpace(firmware.Version))
        {
            return string.Empty;
        }

        // The build id is what makes a support report actionable -- "2.0" is true
        // of a hundred different builds.
        return string.IsNullOrWhiteSpace(firmware.Build)
            ? $"Firmware {firmware.Version}"
            : $"Firmware {firmware.Version} · build {firmware.Build}";
    }

    private static string? BatteryLine(ControllerInfo controller)
    {
        if (!controller.Attached || !controller.BatteryValid)
        {
            // An invalid battery reading is not 0%. Showing a flat battery for a
            // controller that simply does not report one is a false alarm.
            return null;
        }

        return controller.Charging
            ? $"Battery {controller.BatteryPercent}% · charging"
            : $"Battery {controller.BatteryPercent}%";
    }

    private static string SourceLabel(AdapterInputSource source) =>
        string.IsNullOrWhiteSpace(source.Name) ? $"Source {source.Id}" : source.Name;

    private static string PhaseText(AdapterRelationshipPhase phase) => phase switch
    {
        AdapterRelationshipPhase.NoRelationship => "No adapter",
        AdapterRelationshipPhase.Idle => "Selected, not connected",
        AdapterRelationshipPhase.Discovering => "Looking for the adapter",
        AdapterRelationshipPhase.Pairing => "Pairing",
        AdapterRelationshipPhase.Connecting => "Connecting",
        AdapterRelationshipPhase.Validating => "Verifying the adapter",
        AdapterRelationshipPhase.Connected => "Connected",
        AdapterRelationshipPhase.RepairRequired => "Repair required",
        _ => "Not connected",
    };
}
