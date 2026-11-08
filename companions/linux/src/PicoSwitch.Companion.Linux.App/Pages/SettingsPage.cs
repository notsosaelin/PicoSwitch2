using Avalonia.Controls;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services.Presentation;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// Settings: preferences about this app (theme, navigation placement, face
/// buttons), and the door to Diagnostics. A port of
/// the WinUI SettingsPage.
///
/// Everything about the adapter as a device (remembered adapters, the
/// controllers paired to it) lives on the Adapter page
/// (docs/architecture/companion-shell-layout.md), so this page holds only what
/// is this app's own choice.
/// </summary>
internal sealed class SettingsPage : CompanionPage
{
    private readonly ComboBox themeBox = Ui.CardSelector(new ComboBox());
    private readonly ComboBox faceLayoutBox = Ui.CardSelector(new ComboBox());
    private readonly ComboBox navigationBox = Ui.CardSelector(new ComboBox());
    private readonly TextBlock faceLayoutReason = Ui.Caption();
    private bool suppressThemeChange;
    private bool faceLayoutReady;

    public SettingsPage(MainWindow shell)
        : base(shell)
    {
        themeBox.ItemsSource = new[] { "Use system setting", "Light", "Dark" };
        themeBox.SelectionChanged += (_, _) =>
        {
            if (!suppressThemeChange && themeBox.SelectedIndex >= 0)
            {
                AppTheme.Apply((AppThemeChoice)themeBox.SelectedIndex);
            }
        };

        // Where the slide-out navigation comes in from (companion-shell-layout.md, "Navigation").
        navigationBox.ItemsSource = NavPlacements.Options.Select(option => option.Title()).ToArray();
        navigationBox.SelectedIndex = NavPlacements.Options.ToList().IndexOf(shell.Placement);
        navigationBox.SelectionChanged += (_, _) =>
        {
            if (navigationBox.SelectedIndex >= 0)
            {
                Shell.Placement = NavPlacements.Options[navigationBox.SelectedIndex];
            }
        };

        faceLayoutBox.ItemsSource = FaceLayoutChoices.Options.Select(entry => entry.Label).ToArray();
        faceLayoutBox.SelectionChanged += (_, _) => OnFaceLayoutChanged();

        var version = typeof(SettingsPage).Assembly
            .GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
            .OfType<System.Reflection.AssemblyInformationalVersionAttribute>()
            .FirstOrDefault()?.InformationalVersion ?? "unknown";

        // "1.0.0+<40-hex commit>" reads as noise; show the version and a short commit.
        if (version.IndexOf('+') is var plus and > 0 && version.Length - plus - 1 > 12)
        {
            version = $"{version[..plus]} ({version[(plus + 1)..(plus + 13)]})";
        }

        var diagnostics = Ui.IconButton(Icons.ChevronRight, "Open diagnostics", (_, _) => Shell.Navigate("diagnostics"));
        diagnostics.Background = Avalonia.Media.Brushes.Transparent;
        diagnostics.BorderThickness = new Avalonia.Thickness(0);
        var versionText = Ui.Text(version, secondary: true);
        versionText.TextWrapping = Avalonia.Media.TextWrapping.NoWrap;

        Content = Ui.Page(
            Ui.PageHeader(Ui.Title("Settings"), null),
            Ui.Section(
                (string?)null,
                null,
                null,
                Ui.Setting("Theme", null, themeBox),
                Ui.Setting("Navigation", "Where the menu slides in from. Automatic: the side of a wide window, the bottom of a tall one.", navigationBox),
                Ui.Setting("Face buttons", null, faceLayoutReason, faceLayoutBox),
                Ui.Setting("Diagnostics", "Log, Bluetooth bonds and support bundle", diagnostics),
                Ui.Setting("Version", null, versionText)));
    }

    protected override void OnShown()
    {
        suppressThemeChange = true;
        themeBox.SelectedIndex = (int)AppTheme.Current;
        suppressThemeChange = false;
        RenderFaceLayout();
    }

    private void OnFaceLayoutChanged()
    {
        // Guard population: assigning the selection raises this, and acting on
        // it would overwrite the stored choice.
        if (!faceLayoutReady || faceLayoutBox.SelectedIndex < 0)
        {
            return;
        }

        AppServices.SetFaceLayout(FaceLayoutChoices.Options[faceLayoutBox.SelectedIndex].Layout);
        RenderFaceLayout();
    }

    private void RenderFaceLayout()
    {
        faceLayoutReady = false;
        try
        {
            faceLayoutBox.SelectedIndex = FaceLayoutChoices.IndexOf(AppServices.FaceLayout);
        }
        finally
        {
            faceLayoutReady = true;
        }

        faceLayoutReason.Text = FaceLayoutChoices.Reason(AppServices.FaceLayout, AppServices.ResolvedFaceLayout);
        ToolTip.SetTip(
            faceLayoutReason,
            AppServices.FaceLayout == ControllerFaceLayout.Auto ? AppServices.FaceLayoutReason : null);
    }
}
