using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Gamepad — Controller Link in the shape `WINDOWS_PASS.md` §14.6 prescribes when
/// the §14.5 gate does not pass.
///
/// The gate ran on 2026-08-31
/// (docs/experiments/windows-hogp-bridge-feasibility-2026-08-31.md): B1 and B2
/// passed, B3–B6 were never reached because this PC's radio refuses the
/// connectable advertisement the adapter would have to find, and package identity
/// was tested and ruled out. §31 Phase 6 "If the gate fails" then requires exactly
/// this page — one that names the platform limitation and the missing radio
/// capability — and the release ships without Controller Link.
///
/// **The page decides nothing.** Every sentence comes from
/// <c>ControllerLinkView</c>, which is unit tested without a radio; this file
/// paints it and calls the service, in the same shape as <see cref="AdapterPage"/>.
///
/// The measurement is on a button rather than on load. It asks the radio to
/// advertise, and doing that to every user on every visit — for a feature that
/// does not exist — would be a side effect nobody asked for.
/// </summary>
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

    /// <summary>
    /// The service updates its state from the probe's thread, and no WinRT
    /// callback may touch XAML directly (§21.1).
    /// </summary>
    private void OnViewChanged() => AppServices.OnUiThread(Render);

    private async void OnCheck(object sender, RoutedEventArgs e) =>
        await controllerLink.CheckAsync();

    /// <summary>
    /// Enter the Touch Gamepad, which is a full-window mode rather than a page
    /// (WINDOWS_PASS.md §15.4) — so the shell raises it, not the frame.
    /// </summary>
    private void OnOpenTouchGamepad(object sender, RoutedEventArgs e) =>
        (App.Window as MainWindow)?.ShowTouchGamepad();

    private void Render()
    {
        var view = controllerLink.View.Value;

        Headline.Text = view.Headline;
        Explanation.Text = view.Explanation;

        RadioLine.Text = view.RadioLine ?? string.Empty;
        RadioLine.Visibility = view.RadioLine is { Length: > 0 }
            ? Visibility.Visible
            : Visibility.Collapsed;

        CheckButton.IsEnabled = view.ShowRecheck;

        // "Check this PC" the first time, "Check again" afterwards: the second
        // press is what a user reaches for after swapping a Bluetooth adapter,
        // and the label should say that is a thing worth doing.
        CheckButton.Content = view.Step == Windows.Bluetooth.ControllerLinkStep.Unknown
            ? "Check this PC"
            : "Check again";

        Busy.IsActive = view.Measuring;
        Busy.Visibility = view.Measuring ? Visibility.Visible : Visibility.Collapsed;
    }
}
