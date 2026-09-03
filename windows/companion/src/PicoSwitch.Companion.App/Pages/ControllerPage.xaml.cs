using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Windows.Input;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>Product Controller Link workflow; decisions remain in Services.</summary>
public sealed partial class ControllerPage : Page
{
    private readonly ControllerLinkService controllerLink = AppServices.ControllerLink;
    private readonly WindowsGamepadInputSource gamepad = AppServices.PhysicalGamepad;
    private bool faceLayoutReady;
    private DispatcherTimer? adviceTimer;
    private string? shownAdvice;

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
        RenderFaceLayout();

        // A controller plugged in while this page was closed is the ordinary
        // case, so look again on arrival rather than trusting the last
        // enumeration.
        _ = AppServices.RefreshControllerSourcesAsync();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        controllerLink.View.Changed -= OnViewChanged;
        gamepad.SourcesChanged -= OnSourcesChanged;

        // A DispatcherTimer holds a strong reference through its Tick handler,
        // so leaving it running keeps this page alive after navigation.
        adviceTimer?.Stop();
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

    private void OnFaceLayoutChanged(object sender, SelectionChangedEventArgs e)
    {
        // Guard the initial population: assigning ItemsSource raises this, and
        // acting on it would overwrite the stored choice with the first row.
        if (!faceLayoutReady || FaceLayoutBox.SelectedItem is not ControllerFaceLayout layout)
        {
            return;
        }

        AppServices.SetFaceLayout(layout);
        RenderFaceLayout();
    }

    /// <summary>
    /// Show the connection advice, and take it away again.
    ///
    /// It describes a BETTER setup, not a problem the player currently has, so it
    /// should not sit on screen forever like an unresolved fault. Thirty seconds
    /// is long enough to read and act on, short enough that it stops being
    /// furniture.
    /// </summary>
    /// <remarks>
    /// The timer is restarted rather than stacked: RenderSources runs on every
    /// enumeration change, and a fresh timer per call would leave several racing
    /// to close the same bar. Re-showing the SAME advice does not restart the
    /// countdown either — plugging a second controller in should not put an
    /// already-expired message back on screen.
    /// </remarks>
    private void ShowAdvice(string? advice)
    {
        if (advice is null)
        {
            adviceTimer?.Stop();
            shownAdvice = null;
            SourceAdvice.IsOpen = false;
            return;
        }

        if (advice == shownAdvice)
        {
            return;
        }

        shownAdvice = advice;
        SourceAdvice.Message = advice;
        SourceAdvice.IsOpen = true;

        adviceTimer ??= CreateAdviceTimer();
        adviceTimer.Stop();
        adviceTimer.Start();
    }

    private DispatcherTimer CreateAdviceTimer()
    {
        var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(30) };
        timer.Tick += (_, _) =>
        {
            timer.Stop();
            SourceAdvice.IsOpen = false;
        };
        return timer;
    }

    /// <summary>
    /// Paint the printed-legend choice.
    ///
    /// Separate from <see cref="RenderSources"/> because the two change on
    /// different events: the source list moves when hardware comes and goes, this
    /// only when the user picks, and rebuilding the combo on every enumeration
    /// would fight whatever they had open.
    /// </summary>
    private void RenderFaceLayout()
    {
        faceLayoutReady = false;
        try
        {
            FaceLayoutBox.ItemsSource ??= new[]
            {
                ControllerFaceLayout.Auto,
                ControllerFaceLayout.Nintendo,
                ControllerFaceLayout.Xbox,
            };
            FaceLayoutBox.SelectedItem = AppServices.FaceLayout;
        }
        finally
        {
            faceLayoutReady = true;
        }

        FaceLayoutReason.Text = AppServices.FaceLayout == ControllerFaceLayout.Auto
            ? $"Auto chose {AppServices.ResolvedFaceLayout}: {AppServices.FaceLayoutReason}."
            : $"{AppServices.FaceLayout} labels, chosen manually.";
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

        ShowAdvice(view.Advice);
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
