using Avalonia;

namespace PicoSwitch.Companion.Linux.App;

internal static class Program
{
    /// <summary>Shared with the Windows app's AppInstance key, for one vocabulary.</summary>
    private const string InstanceKey = "PicoSwitch.Companion";

    /// <summary>
    /// How long exit may spend releasing the adapter before giving up.
    ///
    /// Bounded because a radio that has gone away can leave a D-Bus call hanging,
    /// and an app that will not close is worse than one that leaves a link for
    /// BlueZ's own supervision timeout to reap.
    /// </summary>
    private static readonly TimeSpan ShutdownBudget = TimeSpan.FromSeconds(4);

    [STAThread]
    public static int Main(string[] args)
    {
        // Before Avalonia starts: a second process must not pay for a window it
        // is about to throw away, and must never open a second BlueZ session.
        using var instance = OperatingSystem.IsLinux()
            ? SingleInstance.TryAcquire(InstanceKey, () => App.ActivateMainWindow())
            : null;
        if (OperatingSystem.IsLinux() && instance is null)
        {
            return 0;
        }

        // Before the platform starts: Avalonia reads its scale once, at startup.
        // gamescope publishes none, so Game Mode would draw at 1x.
        if (PicoSwitch.Companion.Linux.Display.GamescopeScale.ApplyIfUnderGamescope() is { } scaleNote)
        {
            Console.Error.WriteLine(scaleNote);
        }

        try
        {
            return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        }
        finally
        {
            // Off the UI thread, after the window is gone: nothing left to
            // deadlock against.
            AppServices.ShutdownAsync().Wait(ShutdownBudget);
        }
    }

    /// <summary>Also the entry point the Avalonia designer looks for.</summary>
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            // Under gamescope (Steam Game Mode) every popup -- a ComboBox's
            // list, a flyout, a menu -- is drawn INSIDE the main window. As its
            // own X11 window it took focus, gamescope handed focus straight
            // back, and the main window's deactivation closed the popup about
            // 15 ms after it opened: no dropdown could be used (ROG Xbox Ally X,
            // Bazzite, 2026-11-08; the log shows "[pad] window deactivated" /
            // "activated" pairs on each tap). A desktop keeps native popups.
            .With(new X11PlatformOptions
            {
                OverlayPopups = PicoSwitch.Companion.Linux.Display.GamescopeScale.UnderGamescope(
                    Environment.GetEnvironmentVariable),
            })
            .WithInterFont()
            .LogToTrace();
}
