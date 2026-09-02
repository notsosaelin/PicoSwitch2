namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// Controller Link product states.
///
/// Path C carries controller state over the management link that is already
/// open, so several states the HOGP carrier needed no longer exist. They are
/// removed rather than reinterpreted:
///
///   Advertising           — nothing advertises. There is no second radio role.
///   WaitingForConnection  — nothing connects. The carrier is already up.
///   Connecting            — the adapter never dials this PC.
///   Reconnecting          — Controller Link cannot outlive its carrier, so a
///                           lost link is management's reconnect, not ours.
///
/// Keeping them as aliases would leave the UI able to say "waiting for the
/// adapter to pair" about something that never happens.
/// </summary>
public enum ControllerLinkPhase
{
    /// <summary>No trusted management link, or the adapter has no data plane.</summary>
    Unavailable,
    Ready,
    Starting,
    Streaming,
    Stopping,
    Stopped,
    Error,
}

/// <summary>
/// Pure user-facing projection of the Controller Link state machine.
///
/// The page paints this and decides nothing. Platform terms — ATT MTU,
/// characteristic, GATT, data plane — stay in diagnostics; a user who opened
/// the Gamepad page wants to know whether their PC is driving the console.
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
                phase, "Starting…", "Handing controller input to the adapter.",
                false, true, true),
            // The product's only running state. There is no separate "connected"
            // step because there is no second connection to establish: if input
            // is streaming, the link is up.
            ControllerLinkPhase.Streaming => new(
                phase, "Streaming", "This PC is the adapter's controller.",
                false, true, false),
            ControllerLinkPhase.Stopping => new(
                phase, "Stopping…", "Releasing controller input safely.",
                false, false, true),
            _ => new(
                ControllerLinkPhase.Error, "Controller Link error",
                detail ?? "Controller Link stopped unexpectedly.",
                managementReady, false, false, detail),
        };
}
