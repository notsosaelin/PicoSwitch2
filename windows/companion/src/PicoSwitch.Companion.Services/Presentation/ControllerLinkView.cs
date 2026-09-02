namespace PicoSwitch.Companion.Services.Presentation;

public enum ControllerLinkPhase
{
    Unavailable,
    Ready,
    Starting,
    Advertising,
    WaitingForConnection,
    Connecting,
    Connected,
    Reconnecting,
    Stopping,
    Stopped,
    Error,
}

/// <summary>
/// Pure user-facing projection of the production Controller Link state machine.
/// Platform terms (AUMID, AppContainer, HCI) remain diagnostics-only.
/// </summary>
public sealed record ControllerLinkView(
    ControllerLinkPhase Phase,
    string Headline,
    string Explanation,
    bool CanStart,
    bool CanStop,
    bool Busy,
    string? Error = null)
{
    public static ControllerLinkView Of(
        ControllerLinkPhase phase,
        string? detail = null,
        bool managementReady = false) => phase switch
        {
            ControllerLinkPhase.Unavailable => new(
                phase, "Controller Link unavailable",
                detail ?? "Connect to a trusted PicoSwitch adapter first.",
                false, false, false),
            ControllerLinkPhase.Ready or ControllerLinkPhase.Stopped => new(
                phase, "Ready", "Use this PC as the adapter's controller source.",
                managementReady, false, false),
            ControllerLinkPhase.Starting => new(
                phase, "Starting…", "Starting the Windows Bluetooth controller host.",
                false, true, true),
            ControllerLinkPhase.Advertising => new(
                phase, "Advertising", "The controller is visible; waiting for the adapter.",
                false, true, true),
            ControllerLinkPhase.WaitingForConnection => new(
                phase, "Waiting for adapter…", "The controller is visible; waiting for the adapter.",
                false, true, true),
            ControllerLinkPhase.Connecting => new(
                phase, "Connecting…", detail ?? "The adapter is connecting to this PC.",
                false, true, true),
            ControllerLinkPhase.Connected => new(
                phase, "Connected", "Controller input is streaming to PicoSwitch.",
                false, true, false),
            ControllerLinkPhase.Reconnecting => new(
                phase, "Reconnecting…", detail ?? "The controller link was interrupted.",
                false, true, true),
            ControllerLinkPhase.Stopping => new(
                phase, "Stopping…", "Releasing controller input safely.",
                false, false, true),
            _ => new(
                ControllerLinkPhase.Error, "Controller Link error",
                detail ?? "Controller Link stopped unexpectedly.",
                managementReady, false, false, detail),
        };
}
