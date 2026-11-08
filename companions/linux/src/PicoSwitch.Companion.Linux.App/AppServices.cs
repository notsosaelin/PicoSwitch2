using Avalonia.Threading;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.Audio;
using PicoSwitch.Companion.Linux.Input;
using PicoSwitch.Companion.Linux.Storage;
using PicoSwitch.Companion.Platform.Storage;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Services.Presentation;

namespace PicoSwitch.Companion.Linux.App;

/// <summary>
/// The composition root: the Linux counterpart of the WinUI app's AppServices.
///
/// Kept deliberately parallel to it, member for member, so a reviewer can read
/// the two side by side. What differs is only what the platform supplies:
/// BlueZ instead of WinRT (through <see cref="LinuxCompanionPlatform"/>), SDL3
/// instead of Windows.Gaming.Input, and SDL also as the motion backend — the
/// Windows source has no gyro, so its Controller Link carries none.
///
/// Composed here: adapter pairing and management, peers, input sources,
/// Controller Link, companion audio (headset and microphone), the KB/M profile
/// library, the Amiibo library and the Touch Gamepad.
/// </summary>
public static class AppServices
{
    private static readonly Lock Gate = new();

    private static AdapterConnectionService? adapters;
    private static ControllerLinkService? controllerLink;
    private static DiagnosticLog? diagnostics;
    private static ControllerInputSession? controllerInput;
    private static SdlGamepadInputSource? physicalGamepad;
    private static ControllerLinkManagement? controllerLinkManagement;
    private static KbmLibraryRepository? kbmLibrary;
    private static AmiiboLibrary? amiiboLibrary;
    private static LinuxAmiiboKeyStore? amiiboKeys;
    private static AmiiboCatalog? amiiboCatalog;
    private static TouchGamepadService? touchGamepad;
    private static CompanionAudioService? companionAudio;

    public static DiagnosticLog Diagnostics
    {
        get
        {
            lock (Gate)
            {
                return diagnostics ??= CreateLog();
            }
        }
    }

    /// <summary>
    /// The in-memory log, mirrored line by line to stderr. A Linux app's stderr
    /// is where a terminal, `journalctl --user` or a redirect can read it, which
    /// lets a failure be diagnosed over SSH without the Diagnostics page. Added
    /// after the first Ally pairing run, where the only evidence was a one-line
    /// toast (2026-11-06).
    /// </summary>
    private static DiagnosticLog CreateLog()
    {
        var log = new DiagnosticLog();
        log.Recorded += entry =>
        {
            try
            {
                Console.Error.WriteLine(entry.ToString());
            }
            catch (IOException)
            {
                // A closed stderr must never break the app.
            }
        };
        return log;
    }

    /// <summary>$XDG_DATA_HOME/PicoSwitch2 (normally ~/.local/share/PicoSwitch2).</summary>
    public static DocumentStore Documents => Platform.Documents;

    /// <summary>
    /// Set by tooling, never by users: points the document store at a scratch
    /// directory (see the screenshot harness).
    /// </summary>
    public const string DataDirectoryVariable = "PICOSWITCH2_DATA_DIR";

    /// <summary>The host services the shared layers compose: BlueZ and SDL, here.</summary>
    private static LinuxCompanionPlatform Platform { get; } =
        new(Environment.GetEnvironmentVariable(DataDirectoryVariable) is { Length: > 0 } root ? root : null);

    /// <summary>
    /// The user's local KB/M profile library. Deliberately not reached through
    /// <see cref="Adapters"/>: it belongs to the user and works with nothing paired.
    /// </summary>
    public static KbmLibraryRepository KbmLibrary
    {
        get
        {
            lock (Gate)
            {
                return kbmLibrary ??= new KbmLibraryRepository(new KbmProfileLibraryStore(Documents));
            }
        }
    }

    /// <summary>The user's local Amiibo backups; several may be the only copy of a save.</summary>
    public static AmiiboLibrary AmiiboLibrary
    {
        get
        {
            lock (Gate)
            {
                return amiiboLibrary ??= new AmiiboLibrary(Path.Combine(Documents.Directory, "amiibo"));
            }
        }
    }

    /// <summary>The retail key set, in its own owner-only directory away from the backups.</summary>
    public static LinuxAmiiboKeyStore AmiiboKeys
    {
        get
        {
            lock (Gate)
            {
                return amiiboKeys ??= new LinuxAmiiboKeyStore(Path.Combine(Documents.Directory, "amiibo-private"));
            }
        }
    }

    /// <summary>AmiiboAPI enrichment: public catalog data, fetched once, nothing about the user sent.</summary>
    public static AmiiboCatalog AmiiboCatalog
    {
        get
        {
            lock (Gate)
            {
                return amiiboCatalog ??= new AmiiboCatalog(Path.Combine(Documents.Directory, "amiibo"));
            }
        }
    }

    /// <summary>Which library view the user prefers. Same document and vocabulary as Windows.</summary>
    public static AmiiboViewMode AmiiboViewMode
    {
        get => Enum.TryParse<AmiiboViewMode>(Documents.Read("amiibo-view"), out var mode) ? mode : AmiiboViewMode.Grid;
        set => _ = Documents.Write("amiibo-view", value.ToString());
    }

    /// <summary>
    /// Whether catalog artwork may be fetched. Artwork is the one request made
    /// per FIGURE, so it is the seam for an "online artwork" setting.
    /// </summary>
    public static bool OnlineArtworkEnabled => Documents.Read("amiibo-artwork") != "off";

    public static AdapterConnectionService Adapters
    {
        get
        {
            lock (Gate)
            {
                return adapters ??= AdapterConnectionService.CreateDefault(
                    diagnostics ??= CreateLog(), Platform);
            }
        }
    }

    /// <summary>
    /// The on-screen controller: its layouts, its profile library and its editor.
    /// </summary>
    /// <remarks>
    /// The same composition as the WinUI app's, and for the same reasons. It is
    /// not reached through <see cref="Adapters"/> and not gated on Controller
    /// Link: a layout belongs to the user, and the surface has to open, draw and
    /// be editable with nothing paired. The confirmed personality still comes
    /// from the adapter, through the surface (<see cref="TouchProfileSelector"/>).
    /// Layouts live in the same document store as everything else, so they share
    /// the format and the folder with Windows' and can be exported between them.
    /// </remarks>
    public static TouchGamepadService TouchGamepad
    {
        get
        {
            lock (Gate)
            {
                return touchGamepad ??= new TouchGamepadService(
                    new TouchProfileStore(Documents),
                    new TouchOverrideStore(Documents),
                    Diagnostics,
                    controllerInput: ControllerInput);
            }
        }
    }

    private static ControllerInputSession ControllerInput
    {
        get
        {
            lock (Gate)
            {
                if (controllerInput is { } existing)
                {
                    return existing;
                }

                var input = new ControllerInputSession();
                FaceLayoutPreference.Restore(Documents, input);
                controllerInput = input;
                return input;
            }
        }
    }

    /// <summary>
    /// The controller on this machine that feeds Controller Link — on a Steam
    /// Deck, normally its built-in controls.
    ///
    /// SDL is event-driven: every input change raises <c>Frame</c>, so unlike
    /// the Windows source there is no sampler to attach — there is no polling
    /// period for the publisher to wait out.
    /// </summary>
    public static SdlGamepadInputSource PhysicalGamepad
    {
        get
        {
            lock (Gate)
            {
                if (physicalGamepad is { } existing)
                {
                    return existing;
                }

                // Through the property, never `??= new` again: a second
                // construction site would skip the stored face layout.
                var input = ControllerInput;
                var log = diagnostics ??= CreateLog();
                var source = new SdlGamepadInputSource((tag, message) => log.Info(tag, message));
                source.Frame += input.ApplyPhysicalFrame;
                source.Removed += input.RemovePhysicalSource;
                source.BatteryChanged += input.ApplyBattery;
                physicalGamepad = source;

                _ = source.RefreshAsync(AdapterPersonality());
                return source;
            }
        }
    }

    /// <summary>
    /// What the connected adapter is emulating, or null. Used only to keep the
    /// adapter's own USB echo out of automatic controller selection.
    /// </summary>
    private static string? AdapterPersonality()
    {
        try
        {
            var current = adapters?.Snapshot.Value.Personality.Current;
            return current is null
                ? null
                : global::PicoSwitch.Management.Personalities.WireName(current.Value);
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>Re-enumerate, always carrying the adapter personality (see the Windows root).</summary>
    public static Task RefreshControllerSourcesAsync() =>
        PhysicalGamepad.RefreshAsync(AdapterPersonality());

    public static Task SelectControllerSourceAsync(string? sourceId) =>
        PhysicalGamepad.SelectAsync(sourceId, AdapterPersonality());

    public static Task TapConsoleButtonAsync(ControllerButton button) =>
        ControllerInput.TapAsync(button);

    /// <summary>Play's held Home / Capture / C (see <see cref="ControllerInputSession.SetConsoleButton"/>).</summary>
    public static void SetConsoleButton(ControllerButton button, bool pressed) =>
        ControllerInput.SetConsoleButton(button, pressed);

    /// <summary>
    /// The physical controller as the shell's menus read it, and who owns it
    /// (companion-shell-layout.md, "Who owns the handheld's controller").
    /// Avalonia delivers nothing from a gamepad, so the shell drives its own
    /// focus from these frames (<see cref="GamepadUiNavigator"/>). Touching
    /// <see cref="PhysicalGamepad"/> first makes sure SDL is feeding them.
    /// </summary>
    public static ControllerInputSession PadInput
    {
        get
        {
            _ = PhysicalGamepad;
            return ControllerInput;
        }
    }

    public static bool ControllerLinkStreaming =>
        ControllerLink.View.Value.Phase == ControllerLinkPhase.Streaming;

    public static ControllerFaceLayout FaceLayout => ControllerInput.FaceLayout;

    public static ControllerFaceLayout ResolvedFaceLayout => ControllerInput.ResolvedFaceLayout;

    public static string FaceLayoutReason => ControllerInput.FaceLayoutReason;

    public static void SetFaceLayout(ControllerFaceLayout layout) =>
        FaceLayoutPreference.Set(Documents, ControllerInput, layout);

    /// <summary>
    /// Production Controller Link over the trusted management link, with SDL as
    /// both the rumble sink and the motion source.
    /// </summary>
    public static ControllerLinkService ControllerLink
    {
        get
        {
            lock (Gate)
            {
                var management = controllerLinkManagement ??=
                    new ControllerLinkManagement(Adapters).Subscribe();
                var gamepad = PhysicalGamepad;
                return controllerLink ??= new ControllerLinkService(
                    management,
                    ControllerInput,
                    gamepad,
                    diagnostics ??= CreateLog(),
                    motion: gamepad);
            }
        }
    }

    /// <summary>
    /// Companion audio (EXPERIMENTAL): the console's audio on this device, and
    /// this device's microphone as the headset's, over the same trusted
    /// management link Controller Link uses. SDL3 plays and captures, as it
    /// already drives the gamepads. The same service and rules as Windows and
    /// Android: the headset belongs to the controller this device is driving,
    /// so it follows Controller Link.
    /// </summary>
    public static CompanionAudioService CompanionAudio
    {
        get
        {
            lock (Gate)
            {
                if (companionAudio is not null)
                {
                    return companionAudio;
                }

                var link = ControllerLink;
                var streaming = new StateValue<bool>(ControllerLinkStreaming);
                link.View.Changed += () => streaming.Set(ControllerLinkStreaming);
                return companionAudio = new CompanionAudioService(
                    new CompanionAudioManagement(Adapters).Subscribe(),
                    new SdlCompanionAudioSinkFactory(),
                    diagnostics ??= CreateLog(),
                    controllerLink: streaming,
                    micCaptures: new SdlCompanionMicCaptureFactory());
            }
        }
    }

    /// <summary>
    /// Bounded teardown, run by Program after the window has closed.
    ///
    /// Unlike Windows, the management link is released EXPLICITLY. BlueZ keeps
    /// an LE connection up after the D-Bus client that asked for it has gone,
    /// and the adapter has room for one management session — a link left behind
    /// would keep a phone or another PC from reaching it until supervision
    /// timed out.
    /// </summary>
    public static async Task ShutdownAsync()
    {
        ControllerLinkService? link;
        SdlGamepadInputSource? gamepad;
        AdapterConnectionService? connection;
        CompanionAudioService? audio;
        lock (Gate)
        {
            audio = companionAudio;
            link = controllerLink;
            gamepad = physicalGamepad;
            connection = adapters;
        }

        // First, while management is up: the adapter withdraws the headset jack
        // so the console's audio goes back to the TV at once.
        if (audio is not null)
        {
            await Quietly(() => audio.DisposeAsync().AsTask()).ConfigureAwait(false);
        }

        if (link is not null)
        {
            await Quietly(() => link.DisposeAsync().AsTask()).ConfigureAwait(false);
        }

        if (connection is not null && connection.Connection.Value.Connected)
        {
            await Quietly(connection.DisconnectAsync).ConfigureAwait(false);
        }

        if (gamepad is not null)
        {
            await Quietly(() => gamepad.DisposeAsync().AsTask()).ConfigureAwait(false);
        }
    }

    private static async Task Quietly(Func<Task> step)
    {
        try
        {
            await step().ConfigureAwait(false);
        }
        catch (Exception error)
        {
            Diagnostics.Warn("app", $"shutdown step failed: {error.Message}");
        }
    }

    /// <summary>
    /// Run on the UI thread. BlueZ signals and SDL events arrive on their own
    /// threads, and no control may be touched from them.
    /// </summary>
    public static void OnUiThread(Action action)
    {
        if (Dispatcher.UIThread.CheckAccess())
        {
            action();
            return;
        }

        Dispatcher.UIThread.Post(action);
    }
}
