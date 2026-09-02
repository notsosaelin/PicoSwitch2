using Microsoft.UI.Dispatching;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Companion.Windows.Input;

namespace PicoSwitch.Companion.App;

/// <summary>
/// The composition root.
///
/// One place that knows how the layers are assembled, so no page has to. It is
/// built once, on first use, and the management repository behind it comes from
/// <see cref="ManagementOwner"/> — which is what makes "one process, one active
/// management session" structural rather than a consequence of navigation.
/// </summary>
public static class AppServices
{
    private static readonly Lock Gate = new();

    private static AdapterConnectionService? adapters;
    private static ControllerLinkService? controllerLink;
    private static KbmLibraryRepository? kbmLibrary;
    private static AmiiboLibrary? amiiboLibrary;
    private static WindowsAmiiboKeyStore? amiiboKeys;
    private static AmiiboCatalog? amiiboCatalog;
    private static DiagnosticLog? diagnostics;
    private static DispatcherQueue? dispatcher;
    private static WindowsDocumentStore? documents;
    private static TouchGamepadService? touchGamepad;
    private static ControllerInputSession? controllerInput;
    private static WindowsGamepadInputSource? physicalGamepad;
    private static ControllerLinkManagement? controllerLinkManagement;

    public static DiagnosticLog Diagnostics
    {
        get
        {
            lock (Gate)
            {
                return diagnostics ??= new DiagnosticLog();
            }
        }
    }

    /// <summary>
    /// The app-private documents directory, shared with the services layer.
    ///
    /// One settled place for state that survives a restart. A second store (the
    /// registry, say) would mean two things to migrate and two things to clear.
    /// </summary>
    public static WindowsDocumentStore Documents
    {
        get
        {
            lock (Gate)
            {
                return documents ??= new WindowsDocumentStore();
            }
        }
    }

    /// <summary>
    /// The user's local KB/M profile library.
    /// </summary>
    /// <remarks>
    /// Deliberately NOT reached through <see cref="Adapters"/>: the library
    /// belongs to the user, not to a device, and it must be usable with nothing
    /// paired. Hanging it off the connection service is precisely how creating a
    /// profile came to require an adapter.
    /// </remarks>
    public static KbmLibraryRepository KbmLibrary
    {
        get
        {
            lock (Gate)
            {
                return kbmLibrary ??=
                    new KbmLibraryRepository(new KbmProfileLibraryStore(Documents));
            }
        }
    }

    /// <summary>
    /// The user's local Amiibo backups.
    /// </summary>
    /// <remarks>
    /// Like <see cref="KbmLibrary"/>, deliberately not reached through
    /// <see cref="Adapters"/>. These are the user's tag backups; importing,
    /// renaming, exporting and inspecting them needs no adapter, and several of
    /// them are the only surviving copy of a save state.
    /// </remarks>
    public static AmiiboLibrary AmiiboLibrary
    {
        get
        {
            lock (Gate)
            {
                return amiiboLibrary ??=
                    new AmiiboLibrary(Path.Combine(Documents.Directory, "amiibo"));
            }
        }
    }

    /// <summary>
    /// The user's amiibo retail key set, protected at rest by DPAPI.
    /// </summary>
    /// <remarks>
    /// Kept in a directory of its own, away from the tag backups, so that
    /// exporting or sharing a library folder cannot sweep it along.
    /// </remarks>
    public static WindowsAmiiboKeyStore AmiiboKeys
    {
        get
        {
            lock (Gate)
            {
                return amiiboKeys ??= new WindowsAmiiboKeyStore(
                    Path.Combine(Documents.Directory, "amiibo-private"));
            }
        }
    }

    /// <summary>
    /// AmiiboAPI enrichment: what a figure is actually called.
    /// </summary>
    /// <remarks>
    /// Cached beside the library rather than in the private key directory: this
    /// is public catalog data about every amiibo that exists, not anything of
    /// the user's, and nothing about which figures they own reaches the network.
    /// </remarks>
    public static AmiiboCatalog AmiiboCatalog
    {
        get
        {
            lock (Gate)
            {
                return amiiboCatalog ??=
                    new AmiiboCatalog(Path.Combine(Documents.Directory, "amiibo"));
            }
        }
    }

    /// <summary>
    /// Which library view the user prefers. Persisted.
    /// </summary>
    /// <remarks>
    /// A stable preference, not a per-visit choice: re-picking Grid or List on
    /// every launch is friction. Kept in the same document store as everything
    /// else, and a damaged value simply falls back to Grid.
    /// </remarks>
    public static AmiiboViewMode AmiiboViewMode
    {
        get => Enum.TryParse<AmiiboViewMode>(Documents.Read("amiibo-view"), out var mode)
            ? mode
            : AmiiboViewMode.Grid;
        set => _ = Documents.Write("amiibo-view", value.ToString());
    }

    /// <summary>
    /// Whether catalog artwork may be fetched.
    /// </summary>
    /// <remarks>
    /// The seam for a future "load online artwork" setting. Artwork is the one
    /// thing that contacts a host per FIGURE rather than once for the whole
    /// catalog, so it reveals which figures are in the library to whoever serves
    /// the images. Routing every artwork request through one flag means that
    /// setting becomes a one-line change rather than a page rewrite.
    ///
    /// Defaults on, matching the Android companion's long-standing behaviour.
    /// </remarks>
    public static bool OnlineArtworkEnabled =>
        Documents.Read("amiibo-artwork") != "off";

    public static AdapterConnectionService Adapters
    {
        get
        {
            lock (Gate)
            {
                if (adapters is { } existing)
                {
                    return existing;
                }

                var log = diagnostics ??= new DiagnosticLog();
                adapters = AdapterConnectionService.CreateDefault(log);
                return adapters;
            }
        }
    }

    private static ControllerInputSession ControllerInput
    {
        get
        {
            lock (Gate)
            {
                return controllerInput ??= new ControllerInputSession();
            }
        }
    }

    private static WindowsGamepadInputSource PhysicalGamepad
    {
        get
        {
            lock (Gate)
            {
                if (physicalGamepad is { } existing)
                {
                    return existing;
                }

                var input = controllerInput ??= new ControllerInputSession();
                var source = new WindowsGamepadInputSource();
                source.Frame += input.ApplyPhysicalFrame;
                source.Removed += input.RemovePhysicalSource;
                physicalGamepad = source;

                // Enumerate once on creation so the page has something to show
                // before the user touches anything. The adapter's personality
                // keeps its own USB output out of automatic selection; it is
                // null until an adapter connects, which is the honest answer
                // at this point.
                _ = source.RefreshAsync(AdapterPersonality());
                return source;
            }
        }
    }

    /// <summary>
    /// What the connected adapter is emulating, or null when none is connected.
    /// Used only to keep the adapter's own USB echo out of automatic controller
    /// selection — never to hide it, since a genuine controller of the same
    /// model carries the same identity.
    /// </summary>
    private static string? AdapterPersonality()
    {
        try
        {
            // The wire name is what the catalog keys on, so the two ends agree
            // on "pro2"/"gc" rather than on a display string.
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

    /// <summary>Production Controller Link over the trusted management link.</summary>
    public static ControllerLinkService ControllerLink
    {
        get
        {
            lock (Gate)
            {
                var management = controllerLinkManagement ??=
                    new ControllerLinkManagement(Adapters).Subscribe();
                return controllerLink ??= new ControllerLinkService(
                    management,
                    ControllerInput,
                    PhysicalGamepad,
                    diagnostics ??= new DiagnosticLog());
            }
        }
    }

    /// <summary>
    /// The on-screen controller: its layouts, its profile library and its editor.
    /// </summary>
    /// <remarks>
    /// Deliberately NOT reached through <see cref="Adapters"/>, and deliberately not
    /// gated on Controller Link. A layout belongs to the user, the whole subsystem is
    /// local (WINDOWS_PASS.md §15.8), and the surface has to open, draw and be editable
    /// with nothing paired — which is exactly the Phase 6a exit criterion.
    ///
    /// The confirmed personality still comes from the adapter, through
    /// <see cref="TouchProfileSelector"/>; what does not come from the adapter is
    /// PERMISSION to edit.
    /// </remarks>
    public static TouchGamepadService TouchGamepad
    {
        get
        {
            lock (Gate)
            {
                return touchGamepad ??= new TouchGamepadService(
                    new WindowsTouchProfileStore(Documents),
                    new WindowsTouchOverrideStore(Documents),
                    diagnostics ??= new DiagnosticLog(),
                    controllerInput: ControllerInput);
            }
        }
    }

    /// <summary>Best-effort bounded teardown for the main window close path.</summary>
    public static async Task ShutdownAsync()
    {
        ControllerLinkService? link;
        WindowsGamepadInputSource? gamepad;
        lock (Gate)
        {
            link = controllerLink;
            gamepad = physicalGamepad;
        }

        if (link is not null)
        {
            await link.DisposeAsync().ConfigureAwait(false);
        }

        if (gamepad is not null)
        {
            await gamepad.DisposeAsync().ConfigureAwait(false);
        }
    }

    public static string? ControllerLinkHostLog()
    {
        try
        {
            var path = Path.Combine(
                global::Windows.Storage.ApplicationData.Current.LocalFolder.Path,
                "controller-link-host.log");
            return File.Exists(path) ? File.ReadAllText(path) : null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    /// <summary>
    /// Remember the UI thread's dispatcher so device callbacks can hop onto it.
    ///
    /// WinRT delivers watcher, pairing and characteristic events on pool threads,
    /// and **no WinRT event handler may touch XAML directly** (WINDOWS_PASS.md
    /// §21.1). Every observable this app exposes is updated from those threads, so
    /// every subscriber marshals through here before mutating a bound property.
    /// </summary>
    public static void CaptureDispatcher(DispatcherQueue queue)
    {
        lock (Gate)
        {
            dispatcher = queue;
        }
    }

    public static void OnUiThread(Action action)
    {
        DispatcherQueue? queue;
        lock (Gate)
        {
            queue = dispatcher;
        }

        if (queue is null || queue.HasThreadAccess)
        {
            action();
            return;
        }

        queue.TryEnqueue(() => action());
    }
}
