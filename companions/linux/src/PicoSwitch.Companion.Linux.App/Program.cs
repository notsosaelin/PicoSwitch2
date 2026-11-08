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
            .WithInterFont()
            .LogToTrace();
}
