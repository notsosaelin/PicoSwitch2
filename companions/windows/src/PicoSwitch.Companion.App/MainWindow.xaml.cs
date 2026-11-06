using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using PicoSwitch.Companion.App.Pages;
using PicoSwitch.Companion.App.Touch;
using PicoSwitch.Companion.Services;
using Windows.Graphics;
using Windows.UI;

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
        ApplyWindowChrome();

        // The caption glyphs are the only chrome that has to follow the theme, and
        // nothing else in this window changes when it flips.
        if (Content is FrameworkElement rootElement)
        {
            rootElement.ActualThemeChanged += (_, _) => ApplyWindowChrome();
        }

        // Device callbacks arrive on pool threads and must never touch XAML
        // directly; this is the hop they take.
        AppServices.CaptureDispatcher(DispatcherQueue);

        AppServices.Adapters.Connection.Changed += OnConnectionChanged;
        AppServices.Adapters.Relationship.Changed += OnConnectionChanged;
        Closed += (_, _) =>
        {
            AppServices.Adapters.Connection.Changed -= OnConnectionChanged;
            AppServices.Adapters.Relationship.Changed -= OnConnectionChanged;
            AppServices.ShutdownAsync().GetAwaiter().GetResult();
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

        // THE WHOLE WINDOW IS PAINTED, not just the strip.
        //
        // The title bar is extended into the client area, so the top of this
        // window is ordinary content. In the shell that is exactly right: nothing
        // is painted and the Mica backdrop shows through. The Touch Gamepad drops
        // that backdrop -- an opaque controller over a translucent material is not
        // a thing -- and anything still unpainted then falls through to the
        // window's DEFAULT brush, which is white in the light theme. That was the
        // white band across the top of the controller.
        //
        // On the ROOT rather than on AppTitleBar, which was the first fix and was
        // not enough. A fixed 32-epx strip covers the reserved caption band at one
        // display scale and not at another, and the report was monitor-specific
        // for exactly that reason: invisible on a 4K panel at 200%, a white line
        // on a 1920x1200 at 125%. Painting the root removes the whole class --
        // there is no band left whose height has to be guessed, and any future row
        // added to this Grid inherits the ground for free.
        WindowRoot.Background = active ? TouchGround() : null;
        AppTitleBar.Background = active ? TouchGround() : null;
        ApplyWindowChrome();
    }

    /// <summary>The controller's ground, from the one place it is defined.</summary>
    private static Brush? TouchGround() =>
        Application.Current.Resources.TryGetValue("TouchSurfaceGroundBrush", out var value)
            ? value as Brush
            : null;

    // DWM window attributes. The Windows App SDK exposes the caption BUTTONS
    // through AppWindow.TitleBar but not the frame border or the caption band
    // itself, and those are the two surfaces that default to a light colour.
    private const int DwmBorderColor = 34;
    private const int DwmCaptionColor = 35;

    /// <summary>Let the frame border draw nothing at all.</summary>
    private const uint DwmColorNone = 0xFFFFFFFEu;

    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(
        IntPtr hwnd, int attribute, ref uint value, int size);

    /// <summary>
    /// The window's non-client surfaces, which XAML does not reach.
    /// </summary>
    /// <remarks>
    /// THE WHITE BAR. The title bar is extended into the client area, so it is
    /// natural to assume XAML owns every pixel. It does not: the 1px frame BORDER
    /// and the caption BAND behind the minimise/maximise/close buttons are drawn
    /// by DWM, from its own colours, and DWM's defaults are light. Nothing in this
    /// app had ever set them, which is why the line was there from the beginning
    /// and in every part of the app rather than only on one screen.
    ///
    /// It reads as monitor-specific because that is when the frame is actually
    /// VISIBLE: maximized, the border sits outside the monitor and is clipped;
    /// under the FullScreen presenter there is no frame at all; and at a high
    /// display scale one physical pixel of it is easy to miss. A window merely
    /// sized to fill a 125% panel shows all of it.
    ///
    /// The border is set to NONE rather than to a colour, so there is nothing to
    /// keep in step with the theme. The caption band follows the app: transparent
    /// buttons over the Mica backdrop in the shell, and the controller's own
    /// ground while the Touch Gamepad is up, where there is no backdrop to show
    /// through.
    ///
    /// Called on every transition that can change any of it, and once at startup,
    /// because DWM keeps these per window and nothing restores them.
    /// </remarks>
    private void ApplyWindowChrome()
    {
        var handle = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var none = DwmColorNone;
        DwmSetWindowAttribute(handle, DwmBorderColor, ref none, sizeof(uint));

        // THE CAPTION BAND IS ALWAYS SET, never left at the system default.
        //
        // This is the white bar. Maximized, Windows pushes the frame outside the
        // monitor and the app's own painted content begins a couple of rows below
        // the top of the screen; that gap is DWM's caption, and DWM's default is
        // light. Leaving it alone in the shell -- which the first attempt did --
        // fixed the Touch Gamepad and left every other page with the same line.
        //
        // COLORREF is 0x00BBGGRR, not ARGB.
        var touch = TouchGamepadHost.Children.Count > 0;
        var dark = touch ||
            (Content as FrameworkElement)?.ActualTheme != ElementTheme.Light;
        var caption = touch
            ? 0x000C0A0Au   // the controller's ground, #0A0A0C
            : dark
                ? 0x00222020u   // the shell's Mica-tinted top in the dark theme
                : 0x00F3F3F3u;  // and in the light one
        DwmSetWindowAttribute(handle, DwmCaptionColor, ref caption, sizeof(uint));

        ApplyCaptionButtonColours(touch);
    }

    /// <summary>
    /// The system-drawn caption buttons, over the controller's ground.
    /// </summary>
    /// <remarks>
    /// These are painted by the SYSTEM, not by XAML, so the strip's own
    /// background does not reach them: leaving them alone puts a light plate
    /// behind minimise/maximise/close at the right-hand end of an otherwise black
    /// band. Restored to the system defaults on the way out by assigning the
    /// theme's own values back, rather than by remembering what they were --
    /// nothing else changes them, so there is nothing to remember.
    /// </remarks>
    private void ApplyCaptionButtonColours(bool touch)
    {
        var bar = AppWindow.TitleBar;

        // TRANSPARENT IN BOTH MODES. Left unset, the buttons carry their own
        // opaque plate in the system's colour, which is a light rectangle at the
        // right-hand end of the title bar whatever is drawn behind it. The band
        // behind them is already painted -- Mica in the shell, the controller's
        // ground in the Touch Gamepad -- so the buttons only need to not cover it.
        var clear = Color.FromArgb(0, 0, 0, 0);
        bar.ButtonBackgroundColor = clear;
        bar.ButtonInactiveBackgroundColor = clear;

        // The glyphs still have to be legible on whichever of those it is. The
        // shell follows the app theme; the controller's ground is dark in both
        // themes, so it does not.
        var dark = touch ||
            (Content as FrameworkElement)?.ActualTheme != ElementTheme.Light;
        var ink = dark
            ? Color.FromArgb(0xFF, 0xE2, 0xE2, 0xE9)
            : Color.FromArgb(0xFF, 0x19, 0x1B, 0x20);
        var wash = dark ? (byte)0xFF : (byte)0x00;

        bar.ButtonForegroundColor = ink;
        bar.ButtonInactiveForegroundColor = Color.FromArgb(0x9A, ink.R, ink.G, ink.B);
        bar.ButtonHoverBackgroundColor = Color.FromArgb(0x2A, wash, wash, wash);
        bar.ButtonHoverForegroundColor = ink;
        bar.ButtonPressedBackgroundColor = Color.FromArgb(0x14, wash, wash, wash);
        bar.ButtonPressedForegroundColor = ink;
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
