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
