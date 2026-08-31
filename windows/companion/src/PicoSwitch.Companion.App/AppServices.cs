using Microsoft.UI.Dispatching;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Storage;
using PicoSwitch.Companion.Services.Diagnostics;

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
    private static KbmLibraryRepository? kbmLibrary;
    private static AmiiboLibrary? amiiboLibrary;
    private static WindowsAmiiboKeyStore? amiiboKeys;
    private static AmiiboCatalog? amiiboCatalog;
    private static DiagnosticLog? diagnostics;
    private static DispatcherQueue? dispatcher;
    private static WindowsDocumentStore? documents;

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
