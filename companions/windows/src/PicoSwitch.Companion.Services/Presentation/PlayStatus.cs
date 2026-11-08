namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// The colour family a status dot belongs to. Each platform maps it to its own
/// brush; the meaning is decided here once.
/// </summary>
public enum StatusTone
{
    Good,
    Busy,
    Bad,
    Idle,
}

/// <summary>
/// The adapter's state as a dot and a few words, and the one action that moves
/// it forward. Shared by the top bar and Play's adapter card on every platform
/// (docs/architecture/companion-shell-layout.md), so they can never disagree.
/// </summary>
public static class AdapterStatus
{
    public static (StatusTone Tone, string Words) Of(bool connected, AdapterRelationshipPhase phase) =>
        connected
            ? (StatusTone.Good, "Connected")
            : phase switch
            {
                AdapterRelationshipPhase.NoRelationship => (StatusTone.Idle, "Not paired"),
                AdapterRelationshipPhase.Discovering => (StatusTone.Busy, "Looking…"),
                AdapterRelationshipPhase.Pairing => (StatusTone.Busy, "Pairing…"),
                AdapterRelationshipPhase.Connecting => (StatusTone.Busy, "Connecting…"),
                AdapterRelationshipPhase.Validating => (StatusTone.Busy, "Verifying…"),
                AdapterRelationshipPhase.Failed => (StatusTone.Bad, "Could not connect"),
                AdapterRelationshipPhase.RepairRequired => (StatusTone.Bad, "Needs repair"),
                _ => (StatusTone.Idle, "Not connected"),
            };

    /// <summary>True while a connection attempt is in progress.</summary>
    public static bool InProgress(AdapterRelationshipPhase phase) => phase is
        AdapterRelationshipPhase.Discovering or
        AdapterRelationshipPhase.Pairing or
        AdapterRelationshipPhase.Connecting or
        AdapterRelationshipPhase.Validating;

    /// <summary>
    /// The adapter card's button: it names the next step, so the card is never
    /// a dead end. "Manage", "Pair adapter" and "Repair" open the Adapter page;
    /// "Connect" connects in place.
    /// </summary>
    public static string NextStep(bool connected, AdapterRelationshipPhase phase) =>
        connected
            ? "Manage"
            : phase switch
            {
                AdapterRelationshipPhase.NoRelationship => "Pair adapter",
                AdapterRelationshipPhase.RepairRequired => "Repair",
                _ when InProgress(phase) => "Connecting…",
                _ => "Connect",
            };

    /// <summary>Whether <see cref="NextStep"/> connects in place rather than opening the Adapter page.</summary>
    public static bool NextStepConnects(bool connected, AdapterRelationshipPhase phase, bool hasRemembered) =>
        !connected &&
        hasRemembered &&
        phase is not (AdapterRelationshipPhase.NoRelationship or AdapterRelationshipPhase.RepairRequired);
}

/// <summary>
/// Play's controller card: which controller, and a few words of state.
/// </summary>
public static class ControllerCardStatus
{
    /// <param name="selectedName">The selected source's name, or null when none is selected.</param>
    /// <param name="selectedCanDrive">Whether the selected source produces input.</param>
    /// <param name="sourceCount">How many sources are offered.</param>
    public static (string Name, StatusTone Tone, string Words) Of(
        string? selectedName, bool selectedCanDrive, int sourceCount) =>
        selectedName is not null
            ? selectedCanDrive
                ? (selectedName, StatusTone.Good, "Ready")
                : (selectedName, StatusTone.Busy, "Can't be read")
            : sourceCount == 0
                ? ("No controller", StatusTone.Idle, "Plug one in")
                : ("No controller chosen", StatusTone.Idle, "Choose one below");
}

/// <summary>Play's one line under Start: what is happening, or what is in the way.</summary>
public static class PlayLine
{
    /// <summary>Nothing when the button already says it all.</summary>
    public static string Of(ControllerLinkView view) =>
        view.CanStop || view.Busy
            ? view.Headline
            : view.CanStart ? string.Empty : view.Explanation;
}

/// <summary>A notice the Play page shows about companion audio, and how serious it is.</summary>
public sealed record HeadsetNotice(string Text, NoticeTone Tone);

/// <summary>The weight of a notice; each platform maps it to its own banner style.</summary>
public enum NoticeTone
{
    Informational,
    Warning,
    Error,
}

/// <summary>
/// Play's Headset and Mic toggles in words (companion audio). Shared by the
/// Windows and Linux Play pages so the two cannot disagree; the Android page
/// keeps its own Compose strings with the same meaning.
/// </summary>
public static class HeadsetLine
{
    /// <summary>The Headset toggle's label.</summary>
    public static string Headset(CompanionAudioView view) => view.Phase switch
    {
        CompanionAudioPhase.Starting => "Headset starting...",
        CompanionAudioPhase.Playing when view.AwaitingConsole =>
            "Headset on · press any button so the console picks it up",
        CompanionAudioPhase.Playing => "Headset on",
        _ when !view.ControllerLink => "Headset · start Controller Link to use it",
        _ => "Headset off",
    };

    /// <summary>The Mic toggle's label: the wish, and whether the adapter took it.</summary>
    public static string Mic(CompanionAudioView view, bool wanted) => (wanted, view.Phase, view.Microphone) switch
    {
        (false, _, _) => "Mic off",
        (true, CompanionAudioPhase.Playing, true) => "Mic on",
        (true, CompanionAudioPhase.Playing, false) => "Mic unavailable",
        _ => "Mic on with headset",
    };

    /// <summary>The Mic toggle can change while the headset is off or playing.</summary>
    public static bool MicCanToggle(CompanionAudioView view) =>
        view.Phase is not CompanionAudioPhase.Unavailable and not CompanionAudioPhase.Starting;

    /// <summary>
    /// What, if anything, to tell the user. A session that ended, was refused
    /// or failed says why; otherwise a microphone the adapter would not take
    /// says why the headset is speaker only.
    /// </summary>
    public static HeadsetNotice? Notice(CompanionAudioView view)
    {
        if (view.Phase is CompanionAudioPhase.Refused or CompanionAudioPhase.Ended or CompanionAudioPhase.Error &&
            view.Detail is { Length: > 0 } detail)
        {
            return new HeadsetNotice(detail, view.Phase switch
            {
                CompanionAudioPhase.Ended => NoticeTone.Informational,
                CompanionAudioPhase.Refused => NoticeTone.Warning,
                _ => NoticeTone.Error,
            });
        }

        return view.Phase is CompanionAudioPhase.Playing && view.MicrophoneDetail is { Length: > 0 } mic
            ? new HeadsetNotice(mic, NoticeTone.Warning)
            : null;
    }
}

/// <summary>
/// The controller card's one-phrase state on Play's home layout
/// (companion-shell-layout.md, "Play"): the same four phrases as Android's
/// HeroCard, plus Stopping, which the desktop state machine has and Android's
/// does not.
/// </summary>
public static class PlayHeadline
{
    public static string Of(ControllerLinkView view) => view.Phase switch
    {
        ControllerLinkPhase.Streaming => "On the console",
        ControllerLinkPhase.Starting => "Connecting…",
        ControllerLinkPhase.Stopping => "Stopping…",
        _ => view.CanStart ? "Ready to play" : "Not ready",
    };

    /// <summary>The one line under the headline: only what blocks starting, otherwise nothing.</summary>
    public static string Blocker(ControllerLinkView view) =>
        view.CanStart || view.CanStop || view.Busy ? string.Empty : view.Explanation;
}

/// <summary>
/// Play's headset strip: one tap plugs the headset in (LIVE), one tap pulls it
/// out. Android's HeadsetTile words. The desktops keep their separate
/// microphone toggle beside it (the microphone is a wish the adapter may
/// refuse, <see cref="HeadsetLine.Mic"/>), so this says nothing about the mic
/// beyond "Headphones only" when the adapter took none.
/// </summary>
public sealed record HeadsetStrip(string Title, string Line, bool Live, bool Busy, bool Enabled)
{
    public static HeadsetStrip Of(CompanionAudioView view) => view.Phase switch
    {
        CompanionAudioPhase.Unavailable =>
            new("Headset", view.Detail ?? "Connect the adapter to use it", false, false, false),
        CompanionAudioPhase.Starting => new("Headset", "Plugging in…", false, true, false),
        CompanionAudioPhase.Playing => new(
            "Headset",
            view.AwaitingConsole
                ? "Press any button so the console picks it up"
                : view.Microphone
                    ? string.Empty
                    : "Headphones only" + (view.MicrophoneDetail is { Length: > 0 } why ? " · " + why : string.Empty),
            true, false, true),
        _ => new(
            "Headset",
            !view.ControllerLink
                ? "Start playing to plug it in"
                : view.Phase == CompanionAudioPhase.Off ? "Click to plug in" : view.Detail ?? "Click to plug in",
            false, false, view.CanToggle),
    };
}
