using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;
using PicoSwitch.Companion.Windows.ControllerLink;

namespace PicoSwitch.Companion.App;

/// <summary>
/// The application entry point, and the enforcement of "one process, one active
/// management session".
///
/// ## Why single-instance is mandatory, not polish
///
/// The Android companion carries a hardware-evidenced defect from 2026-08-23:
/// five stacked <c>MainActivity</c> records each owned their own BLE management
/// transport, so pressing Disconnect appeared to do nothing — the UI reported its
/// own instance as disconnected while another kept the real session alive. The fix there was a process-wide
/// <c>ManagementOwner</c> singleton plus <c>singleTask</c> launch mode.
///
/// WinUI 3 has no Activity stack, so half that hazard is gone. The other half is
/// not: a second PROCESS is entirely possible — the user double-clicks the
/// shortcut again, or launches from the tray — and two processes would hold two
/// GATT sessions to an adapter that admits exactly one management client. A
/// singleton inside the process cannot prevent that; only redirecting activation
/// can. See WINDOWS_PASS.md §12.1.
///
/// This is why <see cref="Main"/> is hand-written and
/// <c>DISABLE_XAML_GENERATED_MAIN</c> is set: the redirect has to happen BEFORE
/// <c>Application.Start</c> creates a XAML runtime, or the second process pays
/// for a window it is about to throw away.
/// </summary>
public partial class App : Application
{
    private const string InstanceKey = "PicoSwitch.Companion";

    /// <summary>The single window, so the theme can be applied to its content root.</summary>
    public static Window? Window { get; private set; }

    public App() => InitializeComponent();

    [STAThread]
    public static int Main(string[] args)
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();

        if (args.Contains("--controller-link-host-check", StringComparer.Ordinal))
        {
            return RunControllerLinkHostCheckAsync().GetAwaiter().GetResult();
        }

        if (RedirectedToRunningInstance())
        {
            return 0;
        }

        // The parameter is named rather than discarded: `_` would shadow the
        // discard, and `_ = new App()` would then assign to the callback
        // parameter instead of constructing the application.
        Start(callbackParams =>
        {
            _ = callbackParams;

            // WinUI dispatches on this thread; without an installed context every
            // `await` in the app would resume on a thread pool thread and touch UI
            // objects from off the UI thread.
            var context = new DispatcherQueueSynchronizationContext(
                DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);

            // Constructed for its side effect: the base Application constructor
            // registers it as Application.Current, which is what raises OnLaunched.
            new App();
        });

        return 0;
    }

    private static async Task<int> RunControllerLinkHostCheckAsync()
    {
        var resultPath = Path.Combine(
            global::Windows.Storage.ApplicationData.Current.LocalFolder.Path,
            "controller-link-main-check.log");
        try
        {
            await using var host = await ControllerLinkHostConnection.OpenAsync();
            await host.StartAsync();
            await host.StopAsync();
            await File.WriteAllTextAsync(
                resultPath,
                "Controller Link host handshake: PASS\r\n" +
                "Controller Link AppContainer advertising: STARTED\r\n" +
                "Controller Link AppContainer advertising: STOPPED\r\n");
            return 0;
        }
        catch (Exception error)
        {
            await File.WriteAllTextAsync(resultPath, $"Controller Link host check failed: {error}");
            return 2;
        }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Window = new MainWindow();
        AppTheme.Restore();
        Window.Activate();
    }

    /// <summary>
    /// True when this process handed its activation to an already-running
    /// instance and should exit.
    ///
    /// <c>AppInstance.FindOrRegisterForKey</c> is the Windows App SDK's
    /// cross-process key registration: the first process to claim the key becomes
    /// the owner, and every later one gets that owner back and redirects to it.
    /// </summary>
    private static bool RedirectedToRunningInstance()
    {
        var instance = AppInstance.FindOrRegisterForKey(InstanceKey);
        if (instance.IsCurrent)
        {
            return false;
        }

        // Blocking is correct here: this process exists only to hand over its
        // activation, and returning before the redirect completes would race the
        // owner's window activation.
        instance.RedirectActivationToAsync(AppInstance.GetCurrent().GetActivatedEventArgs())
            .AsTask()
            .GetAwaiter()
            .GetResult();
        return true;
    }
}
