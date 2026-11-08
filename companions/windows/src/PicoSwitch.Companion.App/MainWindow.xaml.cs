using System.Diagnostics;
using System.Numerics;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Shapes;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.App.Pages;
using PicoSwitch.Companion.App.Touch;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using Windows.Graphics;
using Windows.System;
using Windows.UI;
using DispatcherQueuePriority = Microsoft.UI.Dispatching.DispatcherQueuePriority;
using DispatcherQueueTimer = Microsoft.UI.Dispatching.DispatcherQueueTimer;

namespace PicoSwitch.Companion.App;

/// <summary>
/// What the shell asks of a page (companion-shell-layout.md, "Navigation").
/// </summary>
internal interface IShellPage
{
    /// <summary>The page's own Back: close what it has open and return true, or false for the shell's.</summary>
    bool HandleBack();

    /// <summary>A controller's A on the focused element, when the page means more than "click it".</summary>
    bool HandleConfirm(DependencyObject? focused);

    /// <summary>Where focus lands when a controller or keyboard arrives on the page.</summary>
    Control? PreferredFocus { get; }
}

/// <summary>
/// The application shell, as agreed for every platform in
/// docs/architecture/companion-shell-layout.md ("Navigation"): the page, and a
/// slide-out navigation at the edge the player chose. Android's NavShell is the
/// first implementation and this follows it rule for rule; Linux's MainWindow
/// is the same shell in Avalonia.
///
/// - ALWAYS A SLIDE-OUT. Never docked beside the page; nothing is left on screen
///   while it is away (no handle, no arrow, no pin). It comes in by Back
///   (Escape, or B on a controller), by moving focus toward its edge when the
///   page has nothing left that way, or by a press at that edge (touch); it
///   leaves by Back, a tap on the scrim, focus moving back toward the page, or
///   choosing a page. It moves by Translation only, and once it has left it is
///   collapsed, so focus can never land in it by accident.
/// - The connection dot is an indicator only: not focusable, not clickable.
/// - Back: a dialog first, then the page's own Back (<see cref="IShellPage"/>),
///   then show or hide the navigation. Holding Escape for 0.8 s while
///   Controller Link streams releases it (<see cref="BackKeyGesture"/>).
/// - THE CONTROLLER. WinUI 3 delivers nothing from a gamepad to a desktop app,
///   so the shell reads the frames Windows.Gaming.Input already produces
///   (sampled here while Controller Link is not, by its own sampler while it
///   is) and turns them into focus moves and invocations
///   (<see cref="GamepadUiNavigator"/>). The console owns the controller only on
///   the bare game screen (<see cref="PadOwnership"/>); anything of the app's
///   on top takes it, and the console is neutralized at that moment.
///
/// Play is home. Diagnostics is reached from Settings.
/// </summary>
public sealed partial class MainWindow : Window
{
    /// <summary>
    /// Below this the Amiibo library and the KB/M keyboard map stop being usable.
    /// A hard floor is more honest than a broken layout (WINDOWS_PASS.md §17.3).
    /// </summary>
    private const int MinimumWidth = 640;

    private const int MinimumHeight = 480;

    /// <summary>The shell's title bar, shown only while windowed (F11): a drag region beside the caption buttons.</summary>
    private const int ShellCaptionHeight = 48;

    /// <summary>
    /// The Touch Gamepad's: the caption buttons and nothing else.
    ///
    /// 32 epx is the height Windows draws those buttons at, so this is the smallest
    /// strip that still contains them and still gives the window a drag region.
    /// </summary>
    private const int TouchCaptionHeight = 32;

    private const double NavSideWidth = 80;
    private const double NavBarHeight = 64;
    private const double NavItemSize = 52;

    /// <summary>Below this width a top / bottom navigation uses one-word names (never an ellipsis).</summary>
    private const double NavFullNamesMin = 720;

    /// <summary>A press this close to the navigation's edge brings it in (touch has no Escape key).</summary>
    private const double EdgeGrab = 16;

    private static readonly TimeSpan SlideIn = TimeSpan.FromMilliseconds(220);
    private static readonly TimeSpan SlideOut = TimeSpan.FromMilliseconds(120);

    /// <summary>The navigation's pages, in the agreed order, then Settings.</summary>
    private static readonly (string Tag, string Label, string Short, string Glyph)[] Destinations =
    [
        ("play", "Play", "Play", ""),
        ("kbm", "Keyboard & Mouse", "Keys", ""),
        ("amiibo", "Amiibo", "Amiibo", ""),
        ("customize", "Customization", "Style", ""),
        ("adapter", "Adapter", "Adapter", ""),
        ("settings", "Settings", "Settings", ""),
    ];

    private readonly List<(string Tag, Button Button, TextBlock Label, string Full, string Short)> items = [];
    private readonly Ellipse dot = new() { Width = 14, Height = 14 };
    private readonly ProgressRing dotRing = new() { Width = 28, Height = 28, IsActive = false, Visibility = Visibility.Collapsed };
    private readonly Grid dotHost = new() { Width = NavItemSize, Height = NavItemSize, IsTabStop = false };
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private readonly BackKeyGesture back = new();
    private readonly GamepadUiNavigator gamepadNavigator = new();
    private readonly ControllerInputSession pad = AppServices.PadInput;
    private readonly CancellationTokenSource inputPoll = new();
    private readonly DispatcherQueueTimer hideTimer;
    private readonly DispatcherQueueTimer toastTimer;
    private readonly DispatcherQueueTimer backTimer;
    private readonly DispatcherQueueTimer padTimer;

    private NavPlacement chosen;
    private NavPlacement placement;
    private bool shown;
    private bool keysDriving;
    private string current = "play";
    private Control? pageFocus;
    private ControllerButtonSet lastButtons = ControllerButtonSet.Empty;
    private AnalogFrame lastAnalog = AnalogFrame.Neutral;
    private bool appHeldPad;

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

        hideTimer = Timer(SlideOut + TimeSpan.FromMilliseconds(20), FinishHide);
        toastTimer = Timer(TimeSpan.FromSeconds(3), () =>
        {
            toastTimer!.Stop();
            Toast.Visibility = Visibility.Collapsed;
        });
        backTimer = Timer(TimeSpan.FromMilliseconds(50), OnBackTick);
        padTimer = Timer(TimeSpan.FromMilliseconds(16), OnPadTick);
        padTimer.IsRepeating = true;
        backTimer.IsRepeating = true;

        BuildNavigation();

        var adapters = AppServices.Adapters;
        var link = AppServices.ControllerLink;
        adapters.Connection.Changed += OnConnectionChanged;
        adapters.Relationship.Changed += OnConnectionChanged;
        adapters.Registry.Changed += OnConnectionChanged;
        link.View.Changed += OnLinkChanged;
        pad.PhysicalFrameObserved += OnPhysicalFrame;

        // The console's view of this controller (player number, a game asking
        // for an amiibo) rides on the input sources reply, which nothing else
        // re-reads while a session sits idle.
        _ = adapters.RunInputPollAsync(TimeSpan.FromSeconds(5), inputPoll.Token);

        Closed += (_, _) =>
        {
            adapters.Connection.Changed -= OnConnectionChanged;
            adapters.Relationship.Changed -= OnConnectionChanged;
            adapters.Registry.Changed -= OnConnectionChanged;
            link.View.Changed -= OnLinkChanged;
            pad.PhysicalFrameObserved -= OnPhysicalFrame;
            pad.SetAppHoldsPad(false);
            inputPoll.Cancel();
            padTimer.Stop();
            backTimer.Stop();
            AppServices.ShutdownAsync().GetAwaiter().GetResult();
        };

        WindowRoot.SizeChanged += (_, _) => ApplyPlacement();
        WindowRoot.AddHandler(UIElement.PreviewKeyDownEvent, new KeyEventHandler(OnPreviewKeyDown), true);
        WindowRoot.AddHandler(UIElement.PreviewKeyUpEvent, new KeyEventHandler(OnPreviewKeyUp), true);
        WindowRoot.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(OnRootPointerPressed), true);
        Activated += (_, e) =>
        {
            if (e.WindowActivationState == WindowActivationState.Deactivated)
            {
                back.Cancel();
                backTimer.Stop();
            }
        };

        chosen = NavPlacementPreference.Load(AppServices.Documents);
        ApplyPlacement();
        Navigate("play");
        RenderDot();
        SyncPad();
        padTimer.Start();

        // ALWAYS FULL SCREEN, as on Android and Linux: there is no toggle. F11
        // (OnRootKeyDown) still leaves and re-enters it, for a desktop user who
        // wants the window back.
        SetFullScreen(true);
    }

    /// <summary>The placement the player chose (Settings → Navigation).</summary>
    internal NavPlacement Placement
    {
        get => chosen;
        set
        {
            chosen = value;
            NavPlacementPreference.Save(AppServices.Documents, value);
            ApplyPlacement();
        }
    }

    private DispatcherQueueTimer Timer(TimeSpan interval, Action tick)
    {
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = interval;
        timer.IsRepeating = false;
        timer.Tick += (_, _) => tick();
        return timer;
    }

    /* ------------------------------------------------------------ navigation */

    private void BuildNavigation()
    {
        dotHost.Children.Add(dotRing);
        dotHost.Children.Add(dot);
        dot.HorizontalAlignment = HorizontalAlignment.Center;
        dot.VerticalAlignment = VerticalAlignment.Center;
        dotRing.HorizontalAlignment = HorizontalAlignment.Center;
        dotRing.VerticalAlignment = VerticalAlignment.Center;
        AutomationProperties.SetLiveSetting(dotHost, AutomationLiveSetting.Polite);

        foreach (var (tag, label, shortLabel, glyph) in Destinations)
        {
            var text = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center, FontWeight = Microsoft.UI.Text.FontWeights.Medium };
            var button = new Button
            {
                Style = (Style)WindowRoot.Resources["NavItemStyle"],
                IsTabStop = false,
                Content = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    Spacing = 8,
                    Children = { new FontIcon { Glyph = glyph, FontSize = 20 }, text },
                },
            };
            AutomationProperties.SetName(button, label);
            Lift.Apply(button);
            button.Click += (_, _) => Choose(tag);
            items.Add((tag, button, text, label, shortLabel));
        }

        Nav.TranslationTransition = new Vector3Transition { Duration = SlideIn };
        Scrim.OpacityTransition = new ScalarTransition { Duration = SlideIn };
    }

    /// <summary>Open a destination. Settings selects Settings; Diagnostics selects nothing.</summary>
    public void Navigate(string tag)
    {
        var target = tag switch
        {
            "play" => typeof(ControllerPage),
            "adapter" => typeof(AdapterPage),
            "kbm" => typeof(KeyboardMousePage),
            "amiibo" => typeof(AmiiboPage),
            "customize" => typeof(CustomizationPage),
            "settings" => typeof(SettingsPage),
            "diagnostics" => typeof(DiagnosticsPage),
            _ => typeof(ControllerPage),
        };

        if (ContentFrame.CurrentSourcePageType != target)
        {
            // A page whose markup fails at load throws HERE, with only "XAML
            // parsing failed" reaching the caller. Logged with the full exception
            // (which names the element) so it is visible in Diagnostics instead
            // of being a navigation item that silently does nothing.
            try
            {
                // Page changes cross-fade with a slight rise.
                ContentFrame.Navigate(target, null, new EntranceNavigationTransitionInfo());
            }
            catch (Exception error)
            {
                AppServices.Diagnostics.Error("ui", $"Could not open {target.Name}: {error}");
                return;
            }
        }

        current = tag is "play" or "kbm" or "amiibo" or "customize" or "adapter" or "settings" or "diagnostics" ? tag : "play";
        foreach (var item in items)
        {
            var selected = item.Tag == current;
            item.Button.Style = (Style)WindowRoot.Resources[selected ? "NavItemSelectedStyle" : "NavItemStyle"];
            item.Label.FontWeight = selected ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Medium;
        }

        SyncPad();
        if (keysDriving && !shown)
        {
            FocusPageSoon();
        }
    }

    /// <summary>Play's Amiibo tile: the library, landing on <paramref name="target"/> when it is set.</summary>
    public void OpenAmiiboLibrary(string? target)
    {
        Navigate("amiibo");
        if (target is not null && ContentFrame.Content is AmiiboPage page)
        {
            page.FocusOn(target);
        }
    }

    private void Choose(string tag)
    {
        Navigate(tag);
        Dismiss(restoreFocus: false);
        if (keysDriving)
        {
            FocusPageSoon();
        }
    }

    private void Open(bool focusItem)
    {
        if (TouchGamepadHost.Children.Count > 0)
        {
            return;
        }

        if (!shown)
        {
            pageFocus = FocusManager.GetFocusedElement(WindowRoot.XamlRoot) is Control focused && IsInside(focused, ContentFrame)
                ? focused
                : null;
        }

        shown = true;
        SetItemsReachable(true);
        hideTimer.Stop();
        Nav.Visibility = Visibility.Visible;
        Scrim.Visibility = Visibility.Visible;
        Scrim.IsHitTestVisible = true;
        Slide(toShown: true);
        SyncPad();
        if (focusItem)
        {
            var entry = items.FirstOrDefault(item => item.Tag == current).Button ?? items[0].Button;
            DispatcherQueue.TryEnqueue(() => entry.Focus(FocusState.Keyboard));
        }
    }

    private void Dismiss(bool restoreFocus)
    {
        if (!shown)
        {
            return;
        }

        shown = false;
        SetItemsReachable(false);
        Scrim.IsHitTestVisible = false;
        Slide(toShown: false);
        hideTimer.Stop();
        hideTimer.Start();
        SyncPad();

        // Focus never stays on a navigation item that is leaving.
        if (restoreFocus && pageFocus is { IsLoaded: true, Visibility: Visibility.Visible } previous)
        {
            previous.Focus(FocusState.Keyboard);
        }
        else if (keysDriving)
        {
            FocusPageSoon();
        }

        pageFocus = null;
    }

    /// <summary>Items take focus only while the navigation is out, from the moment it starts to leave.</summary>
    private void SetItemsReachable(bool reachable)
    {
        foreach (var item in items)
        {
            item.Button.IsTabStop = reachable;
        }
    }

    /// <summary>The slide-out has finished: take the navigation out of layout and the focus search.</summary>
    private void FinishHide()
    {
        hideTimer.Stop();
        if (!shown)
        {
            Nav.Visibility = Visibility.Collapsed;
            Scrim.Visibility = Visibility.Collapsed;
        }
    }

    private void Slide(bool toShown)
    {
        var duration = toShown ? SlideIn : SlideOut;
        Nav.TranslationTransition = new Vector3Transition { Duration = duration };
        Scrim.OpacityTransition = new ScalarTransition { Duration = duration };
        Nav.Translation = toShown ? new Vector3(0, 0, 32) : Away();
        Scrim.Opacity = toShown ? 1 : 0;
    }

    /// <summary>Where the navigation rests while away: just past its edge, shadow and all.</summary>
    private Vector3 Away()
    {
        const float shadow = 32;
        return placement switch
        {
            NavPlacement.Right => new Vector3((float)NavSideWidth + shadow, 0, 32),
            NavPlacement.Top => new Vector3(0, -((float)NavBarHeight + shadow), 32),
            NavPlacement.Bottom => new Vector3(0, (float)NavBarHeight + shadow, 32),
            _ => new Vector3(-((float)NavSideWidth + shadow), 0, 32),
        };
    }

    /// <summary>Resolve Automatic against the window and lay the navigation out for its edge.</summary>
    private void ApplyPlacement()
    {
        var width = WindowRoot.ActualWidth > 0 ? WindowRoot.ActualWidth : AppWindow.Size.Width;
        var height = WindowRoot.ActualHeight > 0 ? WindowRoot.ActualHeight : AppWindow.Size.Height;
        var resolved = NavPlacements.Resolve(chosen, landscape: width > height);
        var vertical = resolved.IsVertical();
        var fullNames = width >= NavFullNamesMin;
        foreach (var item in items)
        {
            var labelled = !vertical && item.Tag != "settings";
            item.Label.Visibility = labelled ? Visibility.Visible : Visibility.Collapsed;
            item.Label.Text = fullNames ? item.Full : item.Short;
            item.Button.Padding = labelled ? new Thickness(14, 0, 14, 0) : new Thickness(0);
            item.Button.Width = labelled ? double.NaN : NavItemSize;
            ToolTipService.SetToolTip(item.Button, labelled ? null : item.Full);
        }

        if (resolved == placement && Nav.Child is not null)
        {
            return;
        }

        placement = resolved;
        foreach (var item in items)
        {
            (VisualTreeHelper.GetParent(item.Button) as Panel)?.Children.Remove(item.Button);
        }

        (VisualTreeHelper.GetParent(dotHost) as Panel)?.Children.Remove(dotHost);

        var pages = new StackPanel
        {
            Orientation = vertical ? Orientation.Vertical : Orientation.Horizontal,
            Spacing = vertical ? 8 : 4,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        foreach (var item in items.Where(item => item.Tag != "settings"))
        {
            pages.Children.Add(item.Button);
        }

        var settings = items.First(item => item.Tag == "settings").Button;
        var bar = new Grid();
        if (vertical)
        {
            bar.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            bar.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            bar.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Grid.SetRow(pages, 1);
            Grid.SetRow(settings, 2);
            Grid.SetColumn(pages, 0);
            Grid.SetColumn(settings, 0);
        }
        else
        {
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            bar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(pages, 1);
            Grid.SetColumn(settings, 2);
            Grid.SetRow(pages, 0);
            Grid.SetRow(settings, 0);
        }

        bar.Children.Add(dotHost);
        bar.Children.Add(pages);
        bar.Children.Add(settings);

        Nav.Child = bar;
        Nav.Padding = vertical ? new Thickness(0, 12, 0, 12) : new Thickness(16, 0, 16, 0);
        Nav.Width = vertical ? NavSideWidth : double.NaN;
        Nav.Height = vertical ? double.NaN : NavBarHeight;
        Nav.HorizontalAlignment = placement switch
        {
            NavPlacement.Left => HorizontalAlignment.Left,
            NavPlacement.Right => HorizontalAlignment.Right,
            _ => HorizontalAlignment.Stretch,
        };
        Nav.VerticalAlignment = placement switch
        {
            NavPlacement.Top => VerticalAlignment.Top,
            NavPlacement.Bottom => VerticalAlignment.Bottom,
            _ => VerticalAlignment.Stretch,
        };
        Nav.BorderThickness = placement switch
        {
            NavPlacement.Left => new Thickness(0, 0, 1, 0),
            NavPlacement.Right => new Thickness(1, 0, 0, 0),
            NavPlacement.Top => new Thickness(0, 0, 0, 1),
            _ => new Thickness(0, 1, 0, 0),
        };

        Nav.TranslationTransition = null;
        Nav.Translation = shown ? new Vector3(0, 0, 32) : Away();
    }

    private void OnScrimTapped(object sender, TappedRoutedEventArgs e)
    {
        e.Handled = true;
        Dismiss(restoreFocus: false);
    }

    /* -------------------------------------------------------------- the dot */

    private void OnConnectionChanged() => AppServices.OnUiThread(RenderDot);

    /// <summary>
    /// The app's one connection indicator: a dot, with a ring while connecting.
    /// Never a control: not a tab stop, not clickable. Its words (the tooltip and
    /// the accessible name) are the shared <see cref="AdapterStatus"/>.
    /// </summary>
    private void RenderDot()
    {
        var adapters = AppServices.Adapters;
        var connection = adapters.Connection.Value;
        var relationship = adapters.Relationship.Value;
        var registry = adapters.Registry.Value;
        var record = registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();

        var name = record?.DisplayName ?? connection.DeviceName ?? "No adapter";
        var (tone, words) = AdapterStatus.Of(connection.Connected, relationship.Phase);
        dot.Fill = new SolidColorBrush(StatusColors.Of(tone));
        var connecting = !connection.Connected && AdapterStatus.InProgress(relationship.Phase);
        dotRing.IsActive = connecting;
        dotRing.Visibility = connecting ? Visibility.Visible : Visibility.Collapsed;

        var detail = relationship.Message ?? connection.Message;
        AutomationProperties.SetName(dotHost, $"{name}, {words}");
        ToolTipService.SetToolTip(dotHost, detail is null ? $"{name}: {words}" : $"{name}: {words}. {detail}");
    }

    /* ------------------------------------------------------------------ keys */

    private VirtualKey TowardNav => placement switch
    {
        NavPlacement.Right => VirtualKey.Right,
        NavPlacement.Top => VirtualKey.Up,
        NavPlacement.Bottom => VirtualKey.Down,
        _ => VirtualKey.Left,
    };

    private VirtualKey TowardPage => placement switch
    {
        NavPlacement.Right => VirtualKey.Left,
        NavPlacement.Top => VirtualKey.Down,
        NavPlacement.Bottom => VirtualKey.Up,
        _ => VirtualKey.Right,
    };

    private static bool IsArrow(VirtualKey key) =>
        key is VirtualKey.Left or VirtualKey.Right or VirtualKey.Up or VirtualKey.Down;

    private bool DialogOpen() =>
        WindowRoot.XamlRoot is { } root &&
        VisualTreeHelper.GetOpenPopupsForXamlRoot(root).Any(popup => popup.Child is ContentDialog);

    private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key is not (VirtualKey.Shift or VirtualKey.Control or VirtualKey.Menu))
        {
            keysDriving = true;
        }

        // The Touch Gamepad is a mode with its own keys; a dialog closes on its own Escape.
        if (TouchGamepadHost.Children.Count > 0 || DialogOpen())
        {
            return;
        }

        if (e.Key == VirtualKey.Escape)
        {
            back.Down(clock.ElapsedMilliseconds, repeat: e.KeyStatus.WasKeyDown);
            backTimer.Start();
            e.Handled = true;
            return;
        }

        if (!shown)
        {
            // Nothing focused on the page: the first arrow lands on its first
            // stop, never on whatever a search from the window's corner finds.
            if (IsArrow(e.Key) && !FocusInside(ContentFrame))
            {
                if (!FocusPage() && e.Key == TowardNav)
                {
                    Open(focusItem: true);
                }

                e.Handled = true;
            }

            return;
        }

        e.Handled = MoveInNavigation(e.Key, e.Key == VirtualKey.Tab &&
            Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(global::Windows.UI.Core.CoreVirtualKeyStates.Down));
    }

    /// <summary>
    /// Inside the navigation: along it moves among the items, toward the page
    /// sends it away, and toward the screen's edge (or past either end) goes
    /// nowhere rather than escaping to whatever is nearest.
    /// </summary>
    private bool MoveInNavigation(VirtualKey key, bool shiftTab)
    {
        var vertical = placement.IsVertical();
        var previousKey = vertical ? VirtualKey.Up : VirtualKey.Left;
        var nextKey = vertical ? VirtualKey.Down : VirtualKey.Right;
        if (key == previousKey || key == nextKey || key == VirtualKey.Tab)
        {
            var step = key == previousKey || shiftTab ? -1 : 1;
            var index = items.FindIndex(item => item.Button.FocusState != FocusState.Unfocused);
            var next = index < 0
                ? items.FirstOrDefault(item => item.Tag == current).Button ?? items[0].Button
                : items[Math.Clamp(index + step, 0, items.Count - 1)].Button;
            next.Focus(FocusState.Keyboard);
            return true;
        }

        if (key == TowardPage)
        {
            Dismiss(restoreFocus: true);
            return true;
        }

        return key == TowardNav;
    }

    private void OnPreviewKeyUp(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Escape || !back.Pressed)
        {
            return;
        }

        backTimer.Stop();
        if (back.Up() == BackKeyGesture.Result.Tap)
        {
            Back();
        }

        e.Handled = true;
    }

    /// <summary>
    /// F11 anywhere in the app, and an arrow toward the navigation that nothing
    /// on the page took. Bubbling, so the Touch Gamepad, which handles its own
    /// F11 and marks it handled, never toggles twice, and so the page's own
    /// directional navigation (XYFocusKeyboardNavigation) goes first.
    /// </summary>
    private void OnRootKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.F11)
        {
            ToggleFullScreen();
            e.Handled = true;
            return;
        }

        if (!e.Handled && !shown && e.Key == TowardNav && TouchGamepadHost.Children.Count == 0 && !DialogOpen())
        {
            Open(focusItem: true);
            e.Handled = true;
        }
    }

    private void OnBackTick()
    {
        if (!back.Pressed)
        {
            backTimer.Stop();
            return;
        }

        if (back.Tick(clock.ElapsedMilliseconds, AppServices.ControllerLinkStreaming) == BackKeyGesture.Result.Hold)
        {
            backTimer.Stop();
            _ = ReleaseControllerLinkAsync();
        }
    }

    /// <summary>Holding Back while streaming: give the controller back to this PC.</summary>
    private async Task ReleaseControllerLinkAsync()
    {
        try
        {
            await AppServices.ControllerLink.StopAsync();
            ShowToast("Controller Link released");
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            AppServices.Diagnostics.Error("ui", message);
            ShowToast(message);
        }
    }

    /// <summary>Back: a dialog, then the page's own Back, then show or hide the navigation.</summary>
    private void Back()
    {
        if (TouchGamepadHost.Children.Count > 0)
        {
            return;
        }

        if (WindowRoot.XamlRoot is { } root &&
            VisualTreeHelper.GetOpenPopupsForXamlRoot(root).Select(popup => popup.Child).OfType<ContentDialog>().FirstOrDefault() is { } dialog)
        {
            dialog.Hide();
            return;
        }

        if (shown)
        {
            Dismiss(restoreFocus: true);
            return;
        }

        if (ContentFrame.Content is IShellPage page && page.HandleBack())
        {
            return;
        }

        Open(focusItem: keysDriving);
    }

    private void OnRootPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        keysDriving = false;
        if (shown || TouchGamepadHost.Children.Count > 0)
        {
            return;
        }

        // The touch screen's way in: a press at the navigation's edge.
        var at = e.GetCurrentPoint(ShellLayer).Position;
        var atEdge = placement switch
        {
            NavPlacement.Right => at.X >= ShellLayer.ActualWidth - EdgeGrab,
            NavPlacement.Top => at.Y <= EdgeGrab,
            NavPlacement.Bottom => at.Y >= ShellLayer.ActualHeight - EdgeGrab,
            _ => at.X <= EdgeGrab,
        };
        if (atEdge && at.X >= 0 && at.Y >= 0)
        {
            Open(focusItem: false);
            e.Handled = true;
        }
    }

    private static bool IsInside(DependencyObject? element, DependencyObject ancestor)
    {
        for (var node = element; node is not null; node = VisualTreeHelper.GetParent(node))
        {
            if (ReferenceEquals(node, ancestor))
            {
                return true;
            }
        }

        return false;
    }

    private bool FocusInside(DependencyObject ancestor) =>
        WindowRoot.XamlRoot is { } root && IsInside(FocusManager.GetFocusedElement(root) as DependencyObject, ancestor);

    private void FocusPageSoon() => DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () => FocusPage());

    /// <summary>The page's own first stop: its preferred control, else the first focusable one.</summary>
    private bool FocusPage()
    {
        if (shown || ContentFrame.Content is not Page page)
        {
            return false;
        }

        if ((page as IShellPage)?.PreferredFocus is { IsEnabled: true, Visibility: Visibility.Visible } preferred &&
            preferred.Focus(FocusState.Keyboard))
        {
            return true;
        }

        return FocusManager.FindFirstFocusableElement(page) is Control first && first.Focus(FocusState.Keyboard);
    }

    /* ------------------------------------------------------------- controller */

    /// <summary>Raised on the sampling thread: the UI timer's, or Controller Link's while it streams.</summary>
    private void OnPhysicalFrame(ControllerButtonSet buttons, AnalogFrame analog) =>
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.High, () => OnPadFrame(buttons, analog));

    private void OnPadFrame(ControllerButtonSet buttons, AnalogFrame analog)
    {
        lastButtons = buttons;
        lastAnalog = analog;
        if (!AppOwnsPad())
        {
            return;
        }

        foreach (var command in gamepadNavigator.Feed(buttons, analog, clock.ElapsedMilliseconds))
        {
            OnPadCommand(command);
        }
    }

    /// <summary>
    /// Windows.Gaming.Input is polled. While Controller Link streams its own
    /// sampler reads the controller at encode time; otherwise this timer does,
    /// so the menus have frames to read. Repeats need time to pass either way.
    /// </summary>
    private void OnPadTick()
    {
        if (!AppServices.ControllerLinkStreaming && TouchGamepadHost.Children.Count == 0)
        {
            pad.SamplePhysical();
        }

        if (AppOwnsPad() && gamepadNavigator.Tick(clock.ElapsedMilliseconds) is { } command)
        {
            OnPadCommand(command);
        }
    }

    /// <summary>The app's menus read the controller: not streaming, or something of the app's on top.</summary>
    private bool AppOwnsPad() => appHeldPad && TouchGamepadHost.Children.Count == 0;

    /// <summary>
    /// A controller step. Directions move focus the way the arrow keys do
    /// (XY focus), A invokes the focused control through its automation
    /// pattern (what a click or Enter would do), B is Back, L1 / R1 step pages.
    /// </summary>
    private void OnPadCommand(UiCommand command)
    {
        keysDriving = true;
        var root = WindowRoot.XamlRoot;
        if (root is null)
        {
            return;
        }

        var dialog = VisualTreeHelper.GetOpenPopupsForXamlRoot(root).Select(popup => popup.Child).OfType<ContentDialog>().FirstOrDefault();
        var focused = FocusManager.GetFocusedElement(root) as DependencyObject;
        switch (command)
        {
            case UiCommand.Back:
                Back();
                return;
            case UiCommand.PreviousPage or UiCommand.NextPage:
                if (dialog is null)
                {
                    Choose(ShellPages.Step(current, command == UiCommand.NextPage ? 1 : -1));
                }

                return;
            case UiCommand.Confirm:
                if (dialog is null && ContentFrame.Content is IShellPage page && page.HandleConfirm(focused))
                {
                    return;
                }

                Invoke(focused);
                return;
        }

        var key = command switch
        {
            UiCommand.Up => VirtualKey.Up,
            UiCommand.Down => VirtualKey.Down,
            UiCommand.Left => VirtualKey.Left,
            _ => VirtualKey.Right,
        };

        if (dialog is not null)
        {
            var options = new FindNextElementOptions { SearchRoot = dialog };
            if (!IsInside(focused, dialog))
            {
                (FocusManager.FindFirstFocusableElement(dialog) as Control)?.Focus(FocusState.Keyboard);
                return;
            }

            _ = FocusManager.TryMoveFocus(Direction(key), options);
            return;
        }

        if (shown)
        {
            MoveInNavigation(key, shiftTab: false);
            return;
        }

        if (!IsInside(focused, ContentFrame))
        {
            if (!FocusPage() && key == TowardNav)
            {
                Open(focusItem: true);
            }

            return;
        }

        var moved = FocusManager.TryMoveFocus(Direction(key), new FindNextElementOptions { SearchRoot = ContentFrame.XamlRoot.Content });
        if (!moved && key == TowardNav)
        {
            Open(focusItem: true);
        }
    }

    private static FocusNavigationDirection Direction(VirtualKey key) => key switch
    {
        VirtualKey.Up => FocusNavigationDirection.Up,
        VirtualKey.Down => FocusNavigationDirection.Down,
        VirtualKey.Left => FocusNavigationDirection.Left,
        _ => FocusNavigationDirection.Right,
    };

    /// <summary>What a click, Enter or Space would do, through the control's automation pattern.</summary>
    private static void Invoke(DependencyObject? element)
    {
        if (element is not UIElement target)
        {
            return;
        }

        var peer = FrameworkElementAutomationPeer.FromElement(target) ?? FrameworkElementAutomationPeer.CreatePeerForElement(target);
        switch (peer)
        {
            case not null when peer.GetPattern(PatternInterface.Invoke) is IInvokeProvider invoke:
                invoke.Invoke();
                break;
            case not null when peer.GetPattern(PatternInterface.Toggle) is IToggleProvider toggle:
                toggle.Toggle();
                break;
            case not null when peer.GetPattern(PatternInterface.ExpandCollapse) is IExpandCollapseProvider expand:
                if (expand.ExpandCollapseState == ExpandCollapseState.Collapsed)
                {
                    expand.Expand();
                }
                else
                {
                    expand.Collapse();
                }

                break;
            case not null when peer.GetPattern(PatternInterface.SelectionItem) is ISelectionItemProvider item:
                item.Select();
                break;
        }
    }

    /// <summary>
    /// Who owns the physical controller (<see cref="PadOwnership"/>), recomputed
    /// whenever any of its inputs changes: Controller Link, the page, the
    /// navigation, the Touch Gamepad.
    /// </summary>
    private void SyncPad()
    {
        var overlay = TouchGamepadHost.Children.Count > 0 || current == "diagnostics";
        var console = PadOwnership.ConsoleOwnsPad(AppServices.ControllerLinkStreaming, current, overlay, shown);
        var held = !console;
        if (held == appHeldPad)
        {
            return;
        }

        appHeldPad = held;
        pad.SetAppHoldsPad(held);
        if (AppServices.ControllerLinkStreaming)
        {
            AppServices.Diagnostics.Info("controller", held ? "pad.owner: app (console neutralized)" : "pad.owner: console");
        }

        // Whatever is held at the hand-off is not a fresh press for the menus.
        gamepadNavigator.Reset();
        _ = gamepadNavigator.Feed(lastButtons, lastAnalog, clock.ElapsedMilliseconds);
    }

    private void OnLinkChanged() => AppServices.OnUiThread(SyncPad);

    private void ShowToast(string message)
    {
        ToastText.Text = message;
        Toast.Visibility = Visibility.Visible;
        toastTimer.Stop();
        toastTimer.Start();
    }

    /* --------------------------------------------------------- touch gamepad */

    /// <summary>
    /// Enter the Touch Gamepad.
    ///
    /// ## What "enter" has to mean, and what it must not
    ///
    /// The Android companion returns early from `CompanionApp.kt` when the mode is
    /// active, so its scaffold, navigation rail and content column are never
    /// composed at all. This is the same thing: the page and the navigation are
    /// COLLAPSED (the shell layer that holds them both), and the surface owns the
    /// client area.
    ///
    /// The first Windows attempt instead layered the surface over a still-live
    /// NavigationView (the shell before the dock) and relied on the surface to paint over it. It did not: the
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

        Dismiss(restoreFocus: false);
        FinishHide();
        var view = new TouchGamepadView();
        view.CloseRequested += HideTouchGamepad;
        view.FullScreenToggleRequested += ToggleFullScreen;
        TouchGamepadHost.Children.Add(view);

        ShellLayer.Visibility = Visibility.Collapsed;
        TouchGamepadHost.Visibility = Visibility.Visible;
        SyncPad();
        SystemBackdrop = null;
        ApplyTouchTitleBar(active: true);
    }

    public void HideTouchGamepad()
    {
        if (TouchGamepadHost.Children.Count == 0)
        {
            return;
        }

        // Full screen is the APP's state, not the controller's, so leaving the
        // Touch Gamepad keeps it.

        TouchGamepadHost.Children.Clear();
        TouchGamepadHost.Visibility = Visibility.Collapsed;

        ShellLayer.Visibility = Visibility.Visible;
        SyncPad();
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

        // Full screen has no caption at all, in the shell as in the Touch
        // Gamepad: the strip exists only as the windowed drag region.
        AppTitleBar.Visibility = fullScreen ? Visibility.Collapsed : Visibility.Visible;
        AppTitleBar.Height = active ? TouchCaptionHeight : ShellCaptionHeight;
        AppTitleTextBlock.Visibility = active ? Visibility.Collapsed : Visibility.Visible;
        if (AppTitleBar.XamlRoot is { } xamlRoot)
        {
            CaptionInset.Width = new GridLength(AppWindow.TitleBar.RightInset / xamlRoot.RasterizationScale);
        }

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

    private void ToggleFullScreen() => SetFullScreen(
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
    private void SetFullScreen(bool value)
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
