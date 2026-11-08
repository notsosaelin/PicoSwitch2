using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Storage;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Services.Presentation;

/// <summary>
/// Where the navigation sits (docs/architecture/companion-shell-layout.md,
/// "Navigation"). The player's choice (Settings → Navigation); Automatic puts
/// it at the side of a window wider than tall, where height is the scarce
/// dimension, and at the bottom otherwise. Measured, never a device name.
/// Mirrors Android's <c>NavPlacement</c> key for key.
/// </summary>
public enum NavPlacement
{
    Auto,
    Left,
    Right,
    Top,
    Bottom,
}

public static class NavPlacements
{
    public static readonly IReadOnlyList<NavPlacement> Options =
        [NavPlacement.Auto, NavPlacement.Left, NavPlacement.Right, NavPlacement.Top, NavPlacement.Bottom];

    public static string Key(this NavPlacement placement) => placement switch
    {
        NavPlacement.Left => "left",
        NavPlacement.Right => "right",
        NavPlacement.Top => "top",
        NavPlacement.Bottom => "bottom",
        _ => "auto",
    };

    public static string Title(this NavPlacement placement) => placement switch
    {
        NavPlacement.Left => "Left",
        NavPlacement.Right => "Right",
        NavPlacement.Top => "Top",
        NavPlacement.Bottom => "Bottom",
        _ => "Automatic",
    };

    /// <summary>Anything unrecognised (a missing or damaged document, a future value) is Automatic.</summary>
    public static NavPlacement FromKey(string? key) =>
        Options.FirstOrDefault(option => option.Key() == key?.Trim(), NavPlacement.Auto);

    public static NavPlacement Resolve(NavPlacement chosen, bool landscape) => chosen switch
    {
        NavPlacement.Auto => landscape ? NavPlacement.Left : NavPlacement.Bottom,
        _ => chosen,
    };

    /// <summary>At the side: a column of icons. At the top or bottom: a row with names.</summary>
    public static bool IsVertical(this NavPlacement placement) =>
        placement is NavPlacement.Left or NavPlacement.Right;
}

/// <summary>The stored placement. The same document name and vocabulary on Windows and Linux.</summary>
public static class NavPlacementPreference
{
    public const string DocumentName = "nav-placement";

    public static NavPlacement Load(DocumentStore documents) => NavPlacements.FromKey(documents.Read(DocumentName));

    public static void Save(DocumentStore documents, NavPlacement placement) =>
        _ = documents.Write(DocumentName, placement.Key());
}

/// <summary>
/// The navigation's pages, in the agreed order, and the shoulder buttons'
/// stepping through them. Settings is not among them (it sits apart at the
/// navigation's end) and steps from Play, as on Android.
/// </summary>
public static class ShellPages
{
    public static readonly IReadOnlyList<string> Docked = ["play", "kbm", "amiibo", "customize", "adapter"];

    public static string Step(string current, int step)
    {
        var index = Docked.ToList().IndexOf(current);
        if (index < 0)
        {
            index = 0;
        }

        var count = Docked.Count;
        return Docked[(((index + step) % count) + count) % count];
    }
}

/// <summary>
/// The Back key as two gestures (owner, 2026-11-08), Android's
/// <c>BackKeyGesture</c> ported as is. On the desktops Back is Escape.
///
/// - Tap: ordinary Back. Whatever is open closes first (a dialog, a pane);
///   otherwise it shows or hides the navigation.
/// - Hold (<see cref="HoldMillis"/>) while Controller Link streams: release
///   Controller Link. It fires while the key is still down; the release that
///   follows does nothing. Not streaming, a hold is a tap.
///
/// Pure so the timing is unit-tested; the shell owns the timer.
/// </summary>
public sealed class BackKeyGesture(long holdMillis = BackKeyGesture.HoldMillis)
{
    /// <summary>Long enough that a tap never becomes a release; short enough to feel deliberate.</summary>
    public const long HoldMillis = 800;

    public enum Result
    {
        None,
        Tap,
        Hold,
    }

    private long? downAt;
    private bool held;

    public bool Pressed => downAt is not null;

    /// <summary>Key down. Auto-repeats of a key already down change nothing.</summary>
    public void Down(long nowMillis, bool repeat)
    {
        if (repeat && downAt is not null)
        {
            return;
        }

        downAt = nowMillis;
        held = false;
    }

    /// <summary>The hold timer fired. <paramref name="holdAllowed"/>: Controller Link streams right now.</summary>
    public Result Tick(long nowMillis, bool holdAllowed)
    {
        if (downAt is not { } start || held || !holdAllowed || nowMillis - start < holdMillis)
        {
            return Result.None;
        }

        held = true;
        return Result.Hold;
    }

    /// <summary>Key up: a tap unless the hold already fired.</summary>
    public Result Up()
    {
        var wasDown = downAt is not null;
        var wasHeld = held;
        downAt = null;
        held = false;
        return wasDown && !wasHeld ? Result.Tap : Result.None;
    }

    /// <summary>Focus left the window mid-press: nothing happens.</summary>
    public void Cancel()
    {
        downAt = null;
        held = false;
    }
}

/// <summary>
/// Who owns this computer's controller while Controller Link streams
/// (companion-shell-layout.md, "Who owns the handheld's controller").
///
/// The console only on the bare game screen: Play in front, the navigation
/// hidden, no dialog. Anything of the app's own on top takes the pad, like
/// Steam's overlay. Before 2026-11-08 (Android) streaming alone decided, and the
/// Amiibo library could not be driven while a game asked for a figure.
/// </summary>
public static class PadOwnership
{
    public static bool ConsoleOwnsPad(bool streaming, string page, bool overlayOpen, bool navigationShown) =>
        streaming && page == "play" && !overlayOpen && !navigationShown;
}

/// <summary>What the console is doing with this controller, as Play shows it.</summary>
public static class ConsoleCues
{
    /// <summary>"Player 2" beside the controller's state, or null when the console assigned none.</summary>
    public static string? PlayerLabel(bool connected, ConsoleView console) =>
        connected && console.Player is >= 1 and <= 4 ? $"Player {console.Player}" : null;

    /// <summary>The game is waiting for an amiibo: the Amiibo tile asks for one.</summary>
    public static bool AmiiboWanted(bool connected, ConsoleView console) => connected && console.AmiiboWanted;
}

/// <summary>A step the controller asks the app's menus to take.</summary>
public enum UiCommand
{
    Up,
    Down,
    Left,
    Right,

    /// <summary>A: activate the focused control.</summary>
    Confirm,

    /// <summary>B, on release: Back (the same as an Escape tap).</summary>
    Back,

    /// <summary>L1 / R1: the previous or next page.</summary>
    PreviousPage,
    NextPage,
}

/// <summary>
/// Turns the physical controller's frames into menu steps while the app owns
/// it — the desktops' form of Android's <c>GamepadNavigation</c>, where the
/// system delivers D-pad keys. Neither desktop UI toolkit delivers anything from a
/// gamepad, so the frames the shell already reads (SDL on Linux,
/// Windows.Gaming.Input on Windows) are the input.
///
/// - The D-pad and the left stick move (the stick past three quarters, back
///   under a half, so it cannot chatter at the edge); held, they repeat after
///   <see cref="RepeatDelayMillis"/> every <see cref="RepeatIntervalMillis"/>.
/// - A confirms on press. B goes back on release, so the press that opened
///   something cannot also close it.
/// - L1 / R1 step pages once per press.
///
/// Positional, as the rest of the bridge reads it: A is the bottom face
/// button, B the right one.
/// </summary>
public sealed class GamepadUiNavigator
{
    public const long RepeatDelayMillis = 400;
    public const long RepeatIntervalMillis = 110;

    private const int StickOn = 32;   // |value - 128| beyond 96 of 128: three quarters
    private const int StickOff = 64;  // back inside half of the travel

    private ControllerButtonSet previous = ControllerButtonSet.Empty;
    private UiCommand? heldDirection;
    private long heldSince;
    private long lastRepeat;
    private bool stickX;
    private bool stickY;

    /// <summary>Forget everything held (the app lost the pad, the window lost focus).</summary>
    public void Reset()
    {
        previous = ControllerButtonSet.Empty;
        heldDirection = null;
        stickX = false;
        stickY = false;
    }

    /// <summary>One frame from the controller. Returns the steps to take, in order.</summary>
    public IReadOnlyList<UiCommand> Feed(ControllerButtonSet buttons, AnalogFrame analog, long nowMillis)
    {
        var steps = new List<UiCommand>();
        bool Pressed(ControllerButton button) => buttons.Contains(button) && !previous.Contains(button);
        bool Released(ControllerButton button) => !buttons.Contains(button) && previous.Contains(button);

        if (Pressed(ControllerButton.A))
        {
            steps.Add(UiCommand.Confirm);
        }

        if (Released(ControllerButton.B))
        {
            steps.Add(UiCommand.Back);
        }

        if (Pressed(ControllerButton.L1))
        {
            steps.Add(UiCommand.PreviousPage);
        }

        if (Pressed(ControllerButton.R1))
        {
            steps.Add(UiCommand.NextPage);
        }

        var direction = Direction(analog);
        if (direction != heldDirection)
        {
            heldDirection = direction;
            heldSince = nowMillis;
            lastRepeat = nowMillis;
            if (direction is { } fresh)
            {
                steps.Add(fresh);
            }
        }
        else if (direction is { } held &&
                 nowMillis - heldSince >= RepeatDelayMillis &&
                 nowMillis - lastRepeat >= RepeatIntervalMillis)
        {
            lastRepeat = nowMillis;
            steps.Add(held);
        }

        previous = buttons;
        return steps;
    }

    /// <summary>Repeats need time to pass even when the controller sends no new frame.</summary>
    public UiCommand? Tick(long nowMillis)
    {
        if (heldDirection is not { } held ||
            nowMillis - heldSince < RepeatDelayMillis ||
            nowMillis - lastRepeat < RepeatIntervalMillis)
        {
            return null;
        }

        lastRepeat = nowMillis;
        return held;
    }

    private UiCommand? Direction(AnalogFrame analog)
    {
        if (analog.Dpad is { } dpad)
        {
            if (dpad.Up) return UiCommand.Up;
            if (dpad.Down) return UiCommand.Down;
            if (dpad.Left) return UiCommand.Left;
            if (dpad.Right) return UiCommand.Right;
        }

        var dx = analog.LeftX - 128;
        var dy = analog.LeftY - 128;
        stickX = stickX ? Math.Abs(dx) > StickOff : Math.Abs(dx) > 128 - StickOn;
        stickY = stickY ? Math.Abs(dy) > StickOff : Math.Abs(dy) > 128 - StickOn;
        if (stickY && (!stickX || Math.Abs(dy) >= Math.Abs(dx)))
        {
            return dy < 0 ? UiCommand.Up : UiCommand.Down;
        }

        if (stickX)
        {
            return dx < 0 ? UiCommand.Left : UiCommand.Right;
        }

        return null;
    }
}
