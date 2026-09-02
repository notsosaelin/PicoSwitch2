using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>Product Controller Link workflow; decisions remain in Services.</summary>
public sealed partial class ControllerPage : Page
{
    private readonly ControllerLinkService controllerLink = AppServices.ControllerLink;

    public ControllerPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        controllerLink.View.Changed += OnViewChanged;
        Render();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        controllerLink.View.Changed -= OnViewChanged;

    private void OnViewChanged() => AppServices.OnUiThread(Render);

    private async void OnStart(object sender, RoutedEventArgs e) =>
        await controllerLink.StartAsync();

    private async void OnStop(object sender, RoutedEventArgs e) =>
        await controllerLink.StopAsync();

    private void OnOpenTouchGamepad(object sender, RoutedEventArgs e) =>
        (App.Window as MainWindow)?.ShowTouchGamepad();

    private void Render()
    {
        var current = controllerLink.View.Value;
        Headline.Text = current.Headline;
        Explanation.Text = current.Explanation;
        StartButton.IsEnabled = current.CanStart;
        StopButton.IsEnabled = current.CanStop;
        Busy.IsActive = current.Busy;
        Busy.Visibility = current.Busy ? Visibility.Visible : Visibility.Collapsed;
    }
}
