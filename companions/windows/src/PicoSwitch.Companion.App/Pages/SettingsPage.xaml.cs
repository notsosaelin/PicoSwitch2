using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Settings: preferences about this app (theme, navigation placement, face
/// buttons), and the door to Diagnostics.
///
/// Everything about the adapter as a device — remembered adapters and the
/// controllers paired to it — lives on the Adapter page
/// (docs/architecture/companion-shell-layout.md), so this page holds only what
/// is this app's own choice.
/// </summary>
public sealed partial class SettingsPage : Page
{
    private bool suppressThemeChange;
    private bool faceLayoutReady;
    private bool navigationReady;

    public SettingsPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        suppressThemeChange = true;
        ThemeBox.SelectedIndex = (int)AppTheme.Current;
        suppressThemeChange = false;

        RenderFaceLayout();

        navigationReady = false;
        NavigationBox.ItemsSource ??= NavPlacements.Options.Select(option => option.Title()).ToArray();
        NavigationBox.SelectedIndex = App.Window is MainWindow shell
            ? NavPlacements.Options.ToList().IndexOf(shell.Placement)
            : 0;
        navigationReady = true;
    }

    private void OnNavigationChanged(object sender, SelectionChangedEventArgs e)
    {
        if (navigationReady && NavigationBox.SelectedIndex >= 0 && App.Window is MainWindow shell)
        {
            shell.Placement = NavPlacements.Options[NavigationBox.SelectedIndex];
        }
    }

    private void OnFaceLayoutChanged(object sender, SelectionChangedEventArgs e)
    {
        // Guard population: assigning the selection raises this, and acting on
        // it would overwrite the stored choice.
        if (!faceLayoutReady || FaceLayoutBox.SelectedIndex < 0)
        {
            return;
        }

        AppServices.SetFaceLayout(FaceLayoutChoices.Options[FaceLayoutBox.SelectedIndex].Layout);
        RenderFaceLayout();
    }

    private void RenderFaceLayout()
    {
        faceLayoutReady = false;
        try
        {
            FaceLayoutBox.ItemsSource ??= FaceLayoutChoices.Options.Select(entry => entry.Label).ToArray();
            FaceLayoutBox.SelectedIndex = FaceLayoutChoices.IndexOf(AppServices.FaceLayout);
        }
        finally
        {
            faceLayoutReady = true;
        }

        FaceLayoutReason.Text = FaceLayoutChoices.Reason(AppServices.FaceLayout, AppServices.ResolvedFaceLayout);
        ToolTipService.SetToolTip(
            FaceLayoutReason,
            AppServices.FaceLayout == ControllerFaceLayout.Auto ? AppServices.FaceLayoutReason : null);
    }

    private void OnThemeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressThemeChange || ThemeBox.SelectedIndex < 0)
        {
            return;
        }

        AppTheme.Apply((AppThemeChoice)ThemeBox.SelectedIndex);
    }

    private void OnOpenDiagnostics(object sender, RoutedEventArgs e) =>
        (App.Window as MainWindow)?.Navigate("diagnostics");
}
