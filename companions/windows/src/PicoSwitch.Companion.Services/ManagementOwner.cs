using PicoSwitch.Companion.Services.Diagnostics;

namespace PicoSwitch.Companion.Services;

/// <summary>
/// The one management relationship this process owns.
///
/// ## Why this is application-scoped
///
/// The adapter relationship is a device-level resource: one adapter, one LE
/// management link, one owner. On Android it was previously constructed inside a
/// ViewModel, which scopes it to an Activity's ViewModelStore, so every
/// additional Activity created a second transport with its own GATT connection
/// and its own background poller.
///
/// Confirmed on hardware 2026-08-23. Five live `MainActivity` records were
/// stacked in one task, with two of their transports polling the adapter
/// concurrently. The user-visible symptom was that pressing Disconnect appeared
/// to do nothing: the UI correctly reported ITS OWN instance as disconnected
/// while a different instance kept the real session alive, so the adapter still
/// reported a connected management client and kept answering commands.
///
/// ## What is different on Windows
///
/// WinUI 3 has no Activity stack, so half that hazard is gone. The other half is
/// not: a second PROCESS is entirely possible — the user double-clicks the
/// shortcut again, or launches from the tray — and two processes would hold two
/// GATT sessions to an adapter that admits exactly one management client. A
/// singleton inside the process cannot prevent that, which is why
/// <c>AppInstance.FindOrRegisterForKey</c> redirects a second launch before the
/// XAML runtime starts (see <c>App.xaml.cs</c>). The two mechanisms are both
/// required and neither substitutes for the other.
///
/// Deliberately NOT a second ownership mechanism layered over the transport's own
/// generation handling: the transport still owns retirement of its own
/// generations. This only ensures there is one transport to own them.
/// </summary>
public static class ManagementOwner
{
    private static readonly Lock Gate = new();

    private static AdapterRepository? repository;
    private static DiagnosticLog? diagnosticLog;

    /// <summary>The diagnostics sink the live transport reports through, if one exists.</summary>
    public static DiagnosticLog? Diagnostics
    {
        get
        {
            lock (Gate)
            {
                return diagnosticLog;
            }
        }
    }

    /// <summary>True once a transport exists. Used by tests to assert single creation.</summary>
    public static bool HasRepository
    {
        get
        {
            lock (Gate)
            {
                return repository is not null;
            }
        }
    }

    /// <summary>
    /// The process-wide management repository, created on first use.
    ///
    /// <paramref name="create"/> runs AT MOST ONCE for the life of the process.
    /// That "at most once" is the entire invariant, so it is expressed here where
    /// a test can observe it without a Bluetooth stack.
    ///
    /// <paramref name="diagnostics"/> is adopted from the first caller so
    /// diagnostic output keeps flowing to the log the UI is observing. Later
    /// callers get the existing repository; passing a different log does not build
    /// a second transport.
    /// </summary>
    public static AdapterRepository Get(DiagnosticLog? diagnostics, Func<AdapterRepository> create)
    {
        lock (Gate)
        {
            if (repository is { } existing)
            {
                return existing;
            }

            diagnosticLog = diagnostics;
            var created = create();
            repository = created;
            return created;
        }
    }

    /// <summary>
    /// Retire the live management session because the screen that was using it
    /// went away.
    ///
    /// This DISCONNECTS; it deliberately does not dispose the transport. Disposal
    /// retires the transport's internal lifecycle permanently, which would make
    /// the singleton unusable for the rest of the process — the exact failure mode
    /// a per-ViewModel transport never had to consider because it was thrown away
    /// with its owner.
    ///
    /// Fire-and-forget on purpose, and on an application-lived path rather than
    /// the caller's: the request outlives the ViewModel that made it, and the work
    /// must not be cancelled halfway through closing a real BLE connection.
    /// </summary>
    public static void ReleaseSession()
    {
        AdapterRepository? active;
        lock (Gate)
        {
            active = repository;
        }

        if (active is null)
        {
            return;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                await active.DisconnectAsync().ConfigureAwait(false);
            }
            catch
            {
                // A teardown failure has nowhere useful to go: the session is being
                // abandoned either way, and throwing here would take down an
                // unobserved task.
            }
        });
    }

    /// <summary>Tests only: drop the singleton so each case starts from a clean process.</summary>
    public static void ResetForTest()
    {
        lock (Gate)
        {
            repository = null;
            diagnosticLog = null;
        }
    }
}
