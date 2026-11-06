using Microsoft.UI.Xaml;

namespace PicoSwitch.Companion.App;

public enum AppThemeChoice
{
    System,
    Light,
    Dark,
}

/// <summary>
/// The window theme, and where the choice is remembered.
///
/// Stored in the same app-private documents directory as everything else rather
/// than in the registry: the app already owns a settled place for state that
/// survives a restart, and adding a second one would mean two things to migrate
/// and two things to clear.
///
/// System is the default and is a real choice, not "no choice" — following the OS
/// is what most people want, and it must survive a restart the same way an
/// explicit Light or Dark does.
/// </summary>
public static class AppTheme
{
    private const string FileName = "theme.txt";

    private static AppThemeChoice current = AppThemeChoice.System;

    public static AppThemeChoice Current => current;

    /// <summary>Load the saved choice and apply it. Safe before any window exists.</summary>
    public static void Restore()
    {
        try
        {
            if (AppServices.Documents.Read(FileName) is { } saved &&
                Enum.TryParse<AppThemeChoice>(saved.Trim(), ignoreCase: true, out var choice))
            {
                current = choice;
            }
        }
        catch (Exception)
        {
            // An unreadable preference is not worth failing a launch over; the
            // system default is always a correct answer.
        }

        ApplyToWindow();
    }

    public static void Apply(AppThemeChoice choice)
    {
        current = choice;
        AppServices.Documents.Write(FileName, choice.ToString());
        ApplyToWindow();
    }

    private static void ApplyToWindow()
    {
        if (App.Window?.Content is FrameworkElement root)
        {
            root.RequestedTheme = current switch
            {
                AppThemeChoice.Light => ElementTheme.Light,
                AppThemeChoice.Dark => ElementTheme.Dark,
                _ => ElementTheme.Default,
            };
        }
    }
}
