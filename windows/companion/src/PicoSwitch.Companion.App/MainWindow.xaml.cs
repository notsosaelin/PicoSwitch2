using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using PicoSwitch.Companion.App.Pages;
using PicoSwitch.Companion.App.Touch;
using PicoSwitch.Companion.Services;
using Windows.Graphics;

namespace PicoSwitch.Companion.App;

/// <summary>
/// The application shell: a <c>NavigationView</c> rail, a global connection
/// banner, and a content frame.
///
/// Destinations and their order mirror the Android companion's product map
/// (WINDOWS_PASS.md §17.2). Diagnostics is reached from Settings rather than
/// being a rail destination, exactly as it is on Android — it is not a daily
/// destination, and promoting it would say otherwise.
/// </summary>
public sealed partial class MainWindow : Window
{
    /// <summary>
    /// Below this the Amiibo library and the KB/M keyboard map stop being usable.
    /// A hard floor is more honest than a broken layout (WINDOWS_PASS.md §17.3).
    /// </summary>
    private const int MinimumWidth = 640;

    private const int MinimumHeight = 480;

    /// <summary>The shell's title bar: room for the product name beside the caption buttons.</summary>
    private const int ShellCaptionHeight = 48;

    /// <summary>
    /// The Touch Gamepad's: the caption buttons and nothing else.
    ///
    /// 32 epx is the height Windows draws those buttons at, so this is the smallest
    /// strip that still contains them and still gives the window a drag region.
    /// </summary>
    private const int TouchCaptionHeight = 32;

    public MainWindow()
    {
        InitializeComponent();

        // Mica reaches the top edge only when the title bar is extended, and the
        // drag region must then be declared explicitly or the window cannot be
        // moved.
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        Title = "PicoSwitch2 Companion";

        ApplyMinimumSize();

        // Device callbacks arrive on pool threads and must never touch XAML
        // directly; this is the hop they take.
        AppServices.CaptureDispatcher(DispatcherQueue);

        AppServices.Adapters.Connection.Changed += OnConnectionChanged;
        AppServices.Adapters.Relationship.Changed += OnConnectionChanged;
        Closed += (_, _) =>
        {
            AppServices.Adapters.Connection.Changed -= OnConnectionChanged;
            AppServices.Adapters.Relationship.Changed -= OnConnectionChanged;
        };

        Navigate("adapter");
        RenderBanner();
    }

    private void OnConnectionChanged() => AppServices.OnUiThread(RenderBanner);

    /// <summary>
    /// The connection banner is global because connection state affects every
    /// page, and it is ABSENT when the session is ready — a banner that is always
    /// on is a banner nobody reads.
    /// </summary>
    private void RenderBanner()
    {
        var connection = AppServices.Adapters.Connection.Value;
        var relationship = AppServices.Adapters.Relationship.Value;

        if (connection.Connected)
        {
            ConnectionBanner.IsOpen = false;
            return;
        }

        ConnectionBanner.IsOpen = true;
        ConnectionBanner.Severity = relationship.Phase switch
        {
            AdapterRelationshipPhase.RepairRequired => InfoBarSeverity.Warning,
            AdapterRelationshipPhase.Failed => InfoBarSeverity.Error,
            _ => InfoBarSeverity.Informational,
        };
        ConnectionBanner.Title = relationship.Phase switch
        {
            AdapterRelationshipPhase.NoRelationship => "No adapter paired",
            AdapterRelationshipPhase.Idle => "Not connected",
            AdapterRelationshipPhase.Discovering => "Looking for an adapter",
            AdapterRelationshipPhase.Pairing => "Pairing",
            AdapterRelationshipPhase.Connecting => "Connecting",
            AdapterRelationshipPhase.Validating => "Verifying the adapter",
            AdapterRelationshipPhase.Failed => "Could not connect",
            AdapterRelationshipPhase.RepairRequired => "Repair pairing",
            _ => "Adapter",
        };
        ConnectionBanner.Message = relationship.Message ?? connection.Message ??
            "Open Adapter to pair or connect.";
    }

    /// <summary>
    /// Enter the Touch Gamepad.
    ///
    /// ## What "enter" has to mean, and what it must not
    ///
    /// The Android companion returns early from `CompanionApp.kt` when the mode is
    /// active, so its scaffold, navigation rail and content column are never
    /// composed at all. This is the same thing: the title bar and the whole
    /// NavigationView are COLLAPSED, and the surface owns the client area.
    ///
    /// The first Windows attempt instead layered the surface over a still-live
    /// NavigationView and relied on the surface to paint over it. It did not: the
    /// window carries a Mica backdrop, and the surface's only opaque-looking
    /// brushes were translucent Layer fills — so the rail, the Gamepad page's
    /// cards and the status bars showed straight through the controller, and the
    /// desktop showed through at the edges. Painting that overlay a darker colour
    /// would have hidden the symptom and kept the architecture; removing the shell
    /// is the fix.
    ///
    /// The backdrop goes with it. A system backdrop is a window-level material,
    /// and leaving it armed under a gameplay surface means one mis-set brush is
    /// all it takes to see through the controller again.
    ///
    /// Built fresh each time rather than kept alive behind a collapsed panel, so
    /// leaving actually unsubscribes it and drops its visuals. What is worth
    /// remembering across visits — the toolbar dock, the alignment settings, the
    /// profile library — is persisted, so nothing is lost by it.
    /// </summary>
    public void ShowTouchGamepad()
    {
        if (TouchGamepadHost.Children.Count > 0)
        {
            return;
        }

        var view = new TouchGamepadView();
        view.CloseRequested += HideTouchGamepad;
        view.FullScreenToggleRequested += ToggleTouchFullScreen;
        TouchGamepadHost.Children.Add(view);

        Navigation.Visibility = Visibility.Collapsed;
        TouchGamepadHost.Visibility = Visibility.Visible;
        SystemBackdrop = null;
        ApplyTouchTitleBar(active: true);
    }

    public void HideTouchGamepad()
    {
        if (TouchGamepadHost.Children.Count == 0)
        {
            return;
        }

        // Never leave the user in a presenter they cannot get out of: the way back
        // to the shell is also the way back to an ordinary window.
        SetTouchFullScreen(false);

        TouchGamepadHost.Children.Clear();
        TouchGamepadHost.Visibility = Visibility.Collapsed;

        Navigation.Visibility = Visibility.Visible;
        SystemBackdrop = new MicaBackdrop();
        ApplyTouchTitleBar(active: false);
    }

    /// <summary>
    /// The window's own frame, reduced to the minimum while the controller is up.
    ///
    /// The title bar is EXTENDED into the client area, which means the strip named
    /// here is the window's only drag region — collapse it outright and a windowed
    /// Touch Gamepad cannot be moved. So the strip stays and its contents do not:
    /// no product name, no page identity, nothing of the companion, just the bare
    /// band the caption buttons sit in.
    ///
    /// That is the whole of what §7 calls "the minimal window affordance Windows
    /// genuinely requires", and full screen removes even that, because the
    /// FullScreen presenter takes the caption buttons with it and there is nothing
    /// left to drag.
    /// </summary>
    private void ApplyTouchTitleBar(bool active)
    {
        var fullScreen = AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen;

        AppTitleBar.Visibility = active && fullScreen
            ? Visibility.Collapsed
            : Visibility.Visible;
        AppTitleBar.Height = active ? TouchCaptionHeight : ShellCaptionHeight;
        AppTitleTextBlock.Visibility = active ? Visibility.Collapsed : Visibility.Visible;
    }

    private void ToggleTouchFullScreen() => SetTouchFullScreen(
        AppWindow.Presenter.Kind != AppWindowPresenterKind.FullScreen);

    /// <summary>
    /// True immersive full screen, through the presenter rather than by stretching
    /// anything.
    ///
    /// <c>AppWindowPresenterKind.FullScreen</c> is the supported Windows App SDK
    /// answer and it removes the title bar, the border and the caption buttons
    /// outright — which is what the Android surface gets by hiding the system bars.
    /// The surface is TOLD what happened rather than assuming the request
    /// succeeded, because a presenter change can be refused and a layout resolved
    /// into a rectangle the window never took would be wrong everywhere.
    /// </summary>
    private void SetTouchFullScreen(bool value)
    {
        var already = AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen;
        if (already != value)
        {
            AppWindow.SetPresenter(value
                ? AppWindowPresenterKind.FullScreen
                : AppWindowPresenterKind.Overlapped);

            // The minimum is a property of the overlapped presenter, and going back
            // to one gives a fresh instance that has never been told about it.
            if (!value)
            {
                ApplyMinimumSize();
            }
        }

        ApplyTouchTitleBar(active: TouchGamepadHost.Children.Count > 0);

        if (TouchGamepadHost.Children.Count > 0 &&
            TouchGamepadHost.Children[0] is TouchGamepadView surface)
        {
            surface.SetFullScreen(AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen);
        }
    }

    private void OnNavigationSelectionChanged(
        NavigationView sender,
        NavigationViewSelectionChangedEventArgs args)
    {
        if (args.IsSettingsSelected)
        {
            Navigate("settings");
            return;
        }

        if (args.SelectedItem is NavigationViewItem { Tag: string tag })
        {
            Navigate(tag);
        }
    }

    private void Navigate(string tag)
    {
        var target = tag switch
        {
            "adapter" => typeof(AdapterPage),
            "kbm" => typeof(KeyboardMousePage),
            "amiibo" => typeof(AmiiboPage),
            "gamepad" => typeof(ControllerPage),
            "settings" => typeof(SettingsPage),
            "diagnostics" => typeof(DiagnosticsPage),
            _ => typeof(AdapterPage),
        };

        if (ContentFrame.CurrentSourcePageType != target)
        {
            ContentFrame.Navigate(target, null, new EntranceNavigationTransitionInfo());
        }
    }

    /// <summary>
    /// Enforce the minimum window size through the presenter.
    ///
    /// <c>OverlappedPresenter</c> is the only presenter that has a minimum; a
    /// window that has been put into another presenter (full screen, compact
    /// overlay) is left alone rather than forced back.
    /// </summary>
    private void ApplyMinimumSize()
    {
        if (AppWindow.Presenter is not OverlappedPresenter presenter)
        {
            return;
        }

        presenter.PreferredMinimumWidth = MinimumWidth;
        presenter.PreferredMinimumHeight = MinimumHeight;

        if (AppWindow.Size.Width < MinimumWidth || AppWindow.Size.Height < MinimumHeight)
        {
            AppWindow.Resize(new SizeInt32(
                Math.Max(AppWindow.Size.Width, MinimumWidth),
                Math.Max(AppWindow.Size.Height, MinimumHeight)));
        }
    }
}
