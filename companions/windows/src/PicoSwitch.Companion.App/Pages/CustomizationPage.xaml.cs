using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Windows.UI;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Customization: the emulated controller's colours.
///
/// Moved here from the Adapter page, which is about the adapter as a device.
/// What is enabled and what the hint says come from
/// <see cref="AdapterDashboard"/>'s <see cref="AppearanceSection"/>, exactly as
/// before the move; this page only paints it.
/// </summary>
public sealed partial class CustomizationPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    private AppearanceSection? appearance;

    public CustomizationPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;
        Render();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed -= OnStateChanged;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Registry.Changed -= OnStateChanged;
        adapters.Relationship.Changed -= OnStateChanged;
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    private async void OnEditBody(object sender, RoutedEventArgs e) =>
        await EditColorAsync(ColorTarget.Body, appearance?.Body);

    private async void OnEditLeft(object sender, RoutedEventArgs e) =>
        await EditColorAsync(ColorTarget.LeftAccent, appearance?.LeftAccent);

    private async void OnEditRight(object sender, RoutedEventArgs e) =>
        await EditColorAsync(ColorTarget.RightAccent, appearance?.RightAccent);

    private async Task EditColorAsync(ColorTarget target, RgbColor? current)
    {
        var picker = new ColorPicker
        {
            IsAlphaEnabled = false,
            IsColorSliderVisible = true,
            IsHexInputVisible = true,
            Color = current is { } value
                ? Color.FromArgb(255, (byte)value.Red, (byte)value.Green, (byte)value.Blue)
                : Colors.Black,
        };

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Choose a colour",
            Content = picker,
            PrimaryButtonText = "Set",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        var chosen = picker.Color;
        await SafeAsync(() =>
            adapters.SetColorAsync(target, new RgbColor(chosen.R, chosen.G, chosen.B)));
    }

    private async void OnApplyAppearance(object sender, RoutedEventArgs e) =>
        await SafeAsync(async () =>
        {
            await adapters.ApplyAppearanceAsync();
            Report("The console now sees the new colours.", InfoBarSeverity.Success);
        });

    private void Render()
    {
        var registry = adapters.Registry.Value;
        var selected = registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();
        var view = AdapterDashboard.Project(
            adapters.Snapshot.Value,
            adapters.Relationship.Value,
            adapters.Connection.Value,
            selected,
            AppServices.ControllerLinkStreaming);

        var section = view.Appearance;
        appearance = section;
        AppearanceHint.Text = section.ApplyHint;
        Paint(BodySwatch, section.Body);
        Paint(LeftSwatch, section.LeftAccent);
        Paint(RightSwatch, section.RightAccent);

        // A disabled control always says why (see AdapterPage.Gate).
        var enabled = section.Availability.Enabled;
        BodyEdit.IsEnabled = enabled;
        LeftEdit.IsEnabled = enabled;
        RightEdit.IsEnabled = enabled;
        AppearanceApply.IsEnabled = enabled;
        AppearanceReason.Text = section.Availability.DisabledReason ?? string.Empty;
        AppearanceReason.Visibility = section.Availability.DisabledReason is null
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private static void Paint(Border swatch, RgbColor color) =>
        swatch.Background = new SolidColorBrush(
            Color.FromArgb(255, (byte)color.Red, (byte)color.Green, (byte)color.Blue));

    private void Report(string message, InfoBarSeverity severity)
    {
        OperationBar.Severity = severity;
        OperationBar.Message = message;
        OperationBar.IsOpen = true;
    }

    private async Task SafeAsync(Func<Task> operation)
    {
        BusyRing.IsActive = true;
        OperationBar.IsOpen = false;
        try
        {
            await operation();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            adapters.Diagnostics.Error("ui", message);
            Report(message, InfoBarSeverity.Error);
        }
        finally
        {
            BusyRing.IsActive = false;
            Render();
        }
    }
}
