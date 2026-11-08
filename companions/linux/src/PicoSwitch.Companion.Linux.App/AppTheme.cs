using Avalonia;
using Avalonia.Styling;

namespace PicoSwitch.Companion.Linux.App;

/// <summary>Same vocabulary and document as the Windows app's AppThemeChoice.</summary>
public enum AppThemeChoice
{
    System,
    Light,
    Dark,
}

/// <summary>The user's theme, persisted in the companion document store.</summary>
public static class AppTheme
{
    private const string FileName = "theme.txt";

    public static AppThemeChoice Current =>
        AppServices.Documents.Read(FileName) is { } saved &&
        Enum.TryParse<AppThemeChoice>(saved.Trim(), ignoreCase: true, out var choice)
            ? choice
            : AppThemeChoice.System;

    public static void ApplyStored(Application application) =>
        application.RequestedThemeVariant = Variant(Current);

    public static void Apply(AppThemeChoice choice)
    {
        _ = AppServices.Documents.Write(FileName, choice.ToString());
        if (Application.Current is { } application)
        {
            application.RequestedThemeVariant = Variant(choice);
        }
    }

    private static ThemeVariant Variant(AppThemeChoice choice) => choice switch
    {
        AppThemeChoice.Light => ThemeVariant.Light,
        AppThemeChoice.Dark => ThemeVariant.Dark,
        _ => ThemeVariant.Default,
    };
}
