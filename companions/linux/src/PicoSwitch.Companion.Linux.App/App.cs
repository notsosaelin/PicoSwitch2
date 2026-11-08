using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using PicoSwitch.Companion.Linux.App.Shell;

namespace PicoSwitch.Companion.Linux.App;

public sealed class App : Application
{
    private static MainWindow? window;

    public override void Initialize()
    {
        Name = "PicoSwitch2 Companion";
        Tokens.Install(this);
        AppTheme.ApplyStored(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            window = new MainWindow();
            desktop.MainWindow = window;
        }

        base.OnFrameworkInitializationCompleted();
    }

    /// <summary>A second launch asked for the window; bring it forward.</summary>
    public static void ActivateMainWindow() => Dispatcher.UIThread.Post(() =>
    {
        if (window is null)
        {
            return;
        }

        if (window.WindowState == WindowState.Minimized)
        {
            // Back the way the app always runs.
            window.WindowState = WindowState.FullScreen;
        }

        window.Activate();
    });
}
