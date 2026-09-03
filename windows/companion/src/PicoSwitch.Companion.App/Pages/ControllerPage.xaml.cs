using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Input;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>Product Controller Link workflow; decisions remain in Services.</summary>
public sealed partial class ControllerPage : Page
{
    private readonly ControllerLinkService controllerLink = AppServices.ControllerLink;
    private readonly WindowsGamepadInputSource gamepad = AppServices.PhysicalGamepad;

    public ControllerPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        controllerLink.View.Changed += OnViewChanged;
        gamepad.SourcesChanged += OnSourcesChanged;
        Render();
        RenderSources();

        // A controller plugged in while this page was closed is the ordinary
        // case, so look again on arrival rather than trusting the last
        // enumeration.
        _ = AppServices.RefreshControllerSourcesAsync();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        controllerLink.View.Changed -= OnViewChanged;
        gamepad.SourcesChanged -= OnSourcesChanged;
    }

    private void OnViewChanged() => AppServices.OnUiThread(Render);

    private void OnSourcesChanged() => AppServices.OnUiThread(RenderSources);

    private async void OnRefreshSources(object sender, RoutedEventArgs e) =>
        await AppServices.RefreshControllerSourcesAsync();

    private async void OnUseSource(object sender, RoutedEventArgs e)
    {
        if (SourceBox.SelectedItem is ControllerSourceRow row)
        {
            await AppServices.SelectControllerSourceAsync(row.Id);
        }
    }

    /// <summary>
    /// Paint which controller on this PC feeds the adapter.
    ///
    /// Every sentence comes from <see cref="ControllerSourceView"/>, which is
    /// unit tested; this only moves strings onto controls and keeps the
    /// chooser's selection in step.
    /// </summary>
    private void RenderSources()
    {
        var view = ControllerSourceView.Of(
            gamepad.Sources, gamepad.SelectedSource, gamepad.UnreadableReason);

        SourceHeadline.Text = view.Headline;
        SourceDetail.Text = view.Detail;
        SourceChooser.Visibility = view.CanChoose ? Visibility.Visible : Visibility.Collapsed;

        // Replaced wholesale: the list is small and changes rarely, and tracking
        // per-row identity would buy nothing but a way for the displayed
        // selection to drift out of step with the resolved one.
        SourceBox.ItemsSource = view.Rows;
        SourceBox.DisplayMemberPath = nameof(ControllerSourceRow.Label);
        SourceBox.SelectedItem =
            view.Rows.FirstOrDefault(r => r.Id == view.SelectedId) ??
            view.Rows.FirstOrDefault();
        SourceApply.IsEnabled = view.Rows.Count > 0;
    }

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
