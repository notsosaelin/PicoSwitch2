using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Windowing;
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
    /// Built fresh each time rather than kept alive behind a collapsed panel, so
    /// leaving the surface actually unsubscribes it and drops its visuals. The
    /// things worth remembering across visits - the toolbar dock, the alignment
    /// settings, the profile library - are persisted, so nothing is lost by it.
    /// </summary>
    public void ShowTouchGamepad()
    {
        if (TouchGamepadHost.Children.Count > 0)
        {
            return;
        }

        var view = new TouchGamepadView();
        view.CloseRequested += HideTouchGamepad;
        TouchGamepadHost.Children.Add(view);
        TouchGamepadHost.Visibility = Visibility.Visible;
    }

    public void HideTouchGamepad()
    {
        TouchGamepadHost.Children.Clear();
        TouchGamepadHost.Visibility = Visibility.Collapsed;
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
