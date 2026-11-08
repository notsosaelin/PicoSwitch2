using System.Diagnostics;
using Avalonia;
using Avalonia.Animation;
using Avalonia.Animation.Easings;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform;
using Avalonia.Threading;
using Avalonia.VisualTree;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.App.Pages;
using PicoSwitch.Companion.Linux.App.Touch;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;

namespace PicoSwitch.Companion.Linux.App.Shell;

/// <summary>
/// The shell: the page, and a slide-out navigation at the edge the player
/// chose (docs/architecture/companion-shell-layout.md, "Navigation", the
/// contract all three platforms implement; Android's NavShell is the first
/// implementation and this one follows it rule for rule).
///
/// - ALWAYS A SLIDE-OUT. The navigation is never docked beside the page and
///   nothing is left on screen while it is away (no handle, no arrow, no pin).
///   It comes in by Back (Escape, or B on a controller), by moving focus
///   toward its edge when nothing is left in that direction on the page, or
///   by a press at that edge (the touch screen's way in); it leaves by Back, a
///   click on the scrim, focus moving back toward the page, or choosing a page.
///   It moves only by translation (220 ms in, 120 ms out) and the page never
///   moves, so nothing is re-measured while it slides.
/// - A CLOSED NAVIGATION IS NOT IN THE FOCUS SEARCH: once its slide-out ends it
///   is not visible at all, so a directional move can never land in it by
///   accident (Android's "unpops itself" bug, 2026-11-08).
/// - The connection dot is an indicator only: not focusable, not clickable.
/// - Back: a dialog first, then the page's own Back (<see cref="CompanionPage.HandleBack"/>),
///   then show or hide the navigation. Holding Escape for 0.8 s while
///   Controller Link streams releases it (<see cref="BackKeyGesture"/>).
/// - THE CONTROLLER. Avalonia delivers nothing from a gamepad, so the shell
///   reads the physical frames SDL already produces and turns them into the
///   same key events a keyboard sends (<see cref="GamepadUiNavigator"/>):
///   D-pad and stick are the arrows, A is Enter, B is Escape, L1 / R1 step
///   pages. The console owns the controller only on the bare game screen
///   (<see cref="PadOwnership"/>); anything of the app's on top takes it, and
///   the console is neutralized at that moment.
/// - Always full screen: the window opens full screen with no toggle; F11
///   leaves and re-enters it so a desktop session is never trapped.
///
/// NO DEVICE IS SPECIAL. Placement Automatic is measured (wider than tall:
/// Left), labels shorten on a narrow bar, and nothing here knows a model name.
/// </summary>
internal sealed class MainWindow : Window
{
    /// <summary>A press this close to the navigation's edge brings it in (touch has no Escape key).</summary>
    private const double EdgeGrab = 16;

    private static readonly TimeSpan SlideIn = TimeSpan.FromMilliseconds(220);
    private static readonly TimeSpan SlideOut = TimeSpan.FromMilliseconds(120);

    public static readonly (string Tag, string Label, string Short, string Icon)[] Destinations =
    [
        ("play", "Play", "Play", Icons.Gamepad),
        ("kbm", "Keyboard & Mouse", "Keys", Icons.Keyboard),
        ("amiibo", "Amiibo", "Amiibo", Icons.Amiibo),
        ("customize", "Customization", "Style", Icons.Palette),
        ("adapter", "Adapter", "Adapter", Icons.Bluetooth),
    ];

    /// <summary>Every page, for tooling that walks them (the screenshot harness).</summary>
    public static readonly string[] AllPages = ["play", "kbm", "amiibo", "customize", "adapter", "settings", "diagnostics"];

    private readonly ContentControl content = new();
    private readonly Border scrim = new() { Opacity = 0, IsVisible = false };
    private readonly Border nav;
    private readonly TranslateTransform navShift = new();
    private readonly List<NavItem> items = [];
    private readonly NavItem settingsItem;
    private readonly AdapterDot dot = new();
    private readonly Border toast;
    private readonly TextBlock toastText = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap };
    private readonly Panel shellLayer = new();
    private readonly Panel touchHost = new() { IsVisible = false };
    private readonly DispatcherTimer hideTimer;
    private readonly DispatcherTimer toastTimer;
    private readonly DispatcherTimer backTimer;
    private readonly DispatcherTimer padTimer;
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private readonly BackKeyGesture back = new();
    private readonly GamepadUiNavigator gamepadNavigator = new();
    private readonly ControllerInputSession pad = AppServices.PadInput;
    private readonly CancellationTokenSource inputPoll = new();

    private NavPlacement chosen;
    private NavPlacement placement;
    private bool shown;
    private bool keysDriving;
    private string current = "play";
    private IInputElement? pageFocus;
    private ControllerButtonSet lastButtons = ControllerButtonSet.Empty;
    private AnalogFrame lastAnalog = AnalogFrame.Neutral;
    private bool appHeldPad;
    private WindowState restoreState = WindowState.Normal;
    private bool wantFullScreen = true;

    public MainWindow()
    {
        Title = "PicoSwitch2 Companion";
        MinWidth = 560;
        MinHeight = 420;
        Icon = LoadIcon();
        this.Bind(BackgroundProperty, Tokens.Window);
        SizeToScreen();

        foreach (var (tag, label, shortLabel, icon) in Destinations)
        {
            items.Add(new NavItem(tag, label, shortLabel, icon, () => Choose(tag)));
        }

        settingsItem = new NavItem("settings", "Settings", "Settings", Icons.Settings, () => Choose("settings"), iconOnly: true);
        items.Add(settingsItem);
        SetItemsReachable(false);

        nav = new Border
        {
            RenderTransform = navShift,
            IsVisible = false,
            BoxShadow = BoxShadows.Parse("0 0 24 0 #40000000"),
        }.Bind(Border.BackgroundProperty, Tokens.Rail);

        scrim.Bind(Border.BackgroundProperty, Tokens.Scrim);
        scrim.Transitions = [new DoubleTransition { Property = OpacityProperty, Duration = SlideIn }];
        scrim.PointerPressed += (_, e) =>
        {
            e.Handled = true;
            Dismiss(restoreFocus: false);
        };

        toast = new Border
        {
            IsVisible = false,
            IsHitTestVisible = false,
            MaxWidth = 480,
            Margin = new Thickness(24),
            Padding = new Thickness(18, 12),
            CornerRadius = new CornerRadius(12),
            BorderThickness = new Thickness(1),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Top,
            Child = toastText,
        }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);
        AutomationProperties.SetLiveSetting(toast, AutomationLiveSetting.Polite);

        shellLayer.Children.AddRange([content, scrim, nav, toast]);

        Dialogs = new DialogHost();
        Dialogs.PropertyChanged += (_, e) =>
        {
            if (e.Property == IsVisibleProperty)
            {
                SyncPad();
            }
        };

        var root = new Panel();
        root.Children.Add(shellLayer);
        root.Children.Add(touchHost);
        root.Children.Add(Dialogs);
        Content = root;

        // Directional focus for the keyboard and the gamepad alike: the gamepad's
        // steps arrive as the same arrow keys (OnPadCommand).
        XYFocus.SetNavigationModes(root, XYFocusNavigationModes.Enabled);

        root.SizeChanged += (_, _) => ApplyPlacement();
        hideTimer = new DispatcherTimer(SlideOut + TimeSpan.FromMilliseconds(20), DispatcherPriority.Normal, (_, _) => FinishHide());
        hideTimer.Stop();
        toastTimer = new DispatcherTimer(TimeSpan.FromSeconds(3), DispatcherPriority.Normal, (_, _) =>
        {
            toastTimer!.Stop();
            toast.IsVisible = false;
        });
        toastTimer.Stop();
        backTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(50), DispatcherPriority.Input, (_, _) => OnBackTick());
        backTimer.Stop();
        padTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(40), DispatcherPriority.Input, (_, _) => OnPadTick());

        AddHandler(KeyDownEvent, OnKeyDownTunnel, RoutingStrategies.Tunnel);
        AddHandler(KeyUpEvent, OnKeyUpTunnel, RoutingStrategies.Tunnel);
        // After Avalonia's own directional navigation, which runs on the bubble
        // and leaves an arrow unhandled when nothing lies that way.
        AddHandler(KeyDownEvent, OnKeyDownBubble, RoutingStrategies.Bubble);
        AddHandler(PointerPressedEvent, OnPointerPressedTunnel, RoutingStrategies.Tunnel);
        Deactivated += (_, _) =>
        {
            back.Cancel();
            backTimer.Stop();
        };

        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty)
            {
                (touchHost.Children.FirstOrDefault() as TouchGamepadView)?.SetFullScreen(WindowState == WindowState.FullScreen);
            }
        };

        var adapters = AppServices.Adapters;
        adapters.Connection.Changed += OnConnectionChanged;
        adapters.Relationship.Changed += OnConnectionChanged;
        adapters.Registry.Changed += OnConnectionChanged;
        var link = AppServices.ControllerLink;
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
        };

        chosen = NavPlacementPreference.Load(AppServices.Documents);
        ApplyPlacement();
        Navigate("play");
        dot.Render();
        SyncPad();

        // Opens full screen. SizeToScreen above still decided the restored
        // size, which is what F11 returns to.
        restoreState = WindowState;
        WindowState = WindowState.FullScreen;

        // Full screen is a request a window manager may ignore; gamescope
        // (SteamOS / Bazzite Game Mode) does, and shows the window at the size
        // it asked for, scaled and letterboxed, instead of filling the panel
        // (owner, 2026-11-08). So once mapped, and whenever the screens change,
        // the window also SIZES itself to the whole screen it is on.
        Opened += (_, _) =>
        {
            FillScreen();
            // gamescope settles its output after the first map; ask again then.
            DispatcherTimer.RunOnce(FillScreen, TimeSpan.FromMilliseconds(500));
        };
        Screens.Changed += (_, _) => Dispatcher.UIThread.Post(FillScreen);
    }

    /// <summary>
    /// SteamOS / Bazzite Game Mode: gamescope sets these for what it launches.
    /// Read from the environment, never a device name.
    /// </summary>
    private static bool UnderGamescope =>
        Environment.GetEnvironmentVariable("GAMESCOPE_WAYLAND_DISPLAY") is { Length: > 0 } ||
        string.Equals(Environment.GetEnvironmentVariable("XDG_CURRENT_DESKTOP"), "gamescope", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Full screen by size as well as by state: the screen's whole bounds, at
    /// its origin, so the window covers the panel at its native pixels whether
    /// or not the window manager honours the full-screen state. Left alone
    /// after F11 took the window out of full screen.
    /// </summary>
    private void FillScreen()
    {
        if (!wantFullScreen || !FillsScreen || (Screens.ScreenFromWindow(this) ?? Screens.Primary) is not { } screen)
        {
            return;
        }

        var bounds = screen.Bounds;
        Position = bounds.Position;
        Width = bounds.Width / screen.Scaling;
        Height = bounds.Height / screen.Scaling;
        if (WindowState != WindowState.FullScreen)
        {
            WindowState = WindowState.FullScreen;
        }

        AppServices.Diagnostics.Info(
            "window",
            $"full screen: screen {bounds.Width}x{bounds.Height} px at {screen.Scaling:0.##}x, " +
            $"client {ClientSize.Width:0}x{ClientSize.Height:0}{(UnderGamescope ? ", gamescope" : string.Empty)}");
    }

    public DialogHost Dialogs { get; }

    /// <summary>Off only for tooling that renders at a chosen size (the screenshot harness).</summary>
    public bool FillsScreen { get; init; } = true;

    /// <summary>Off only for tooling that captures a settled frame (the screenshot harness).</summary>
    public bool Animate { get; init; } = true;

    /// <summary>The placement the player chose (Settings → Navigation).</summary>
    public NavPlacement Placement
    {
        get => chosen;
        set
        {
            chosen = value;
            NavPlacementPreference.Save(AppServices.Documents, value);
            ApplyPlacement();
        }
    }

    /// <summary>Whether the navigation is out (tooling, and the pad-ownership rule).</summary>
    public bool NavigationShown => shown;

    /// <summary>The page in front (tooling).</summary>
    public string CurrentPage => current;

    /// <summary>Open a destination. Settings selects Settings; Diagnostics selects nothing.</summary>
    public void Navigate(string tag)
    {
        Func<Control> create = tag switch
        {
            "kbm" => () => new KeyboardMousePage(this),
            "amiibo" => () => new AmiiboPage(this),
            "customize" => () => new CustomizationPage(this),
            "adapter" => () => new AdapterPage(this),
            "settings" => () => new SettingsPage(this),
            "diagnostics" => () => new DiagnosticsPage(this),
            _ => () => new PlayPage(this),
        };

        current = AllPages.Contains(tag) ? tag : "play";

        // Built fresh per visit, as WinUI's Frame does, so leaving a page
        // really drops its subscriptions.
        var page = create();
        if (!Animate)
        {
            // Tooling captures a settled frame: no fade, including the one the
            // constructor's first page started before this was set.
            if (content.Content?.GetType() != page.GetType())
            {
                content.Content = page;
            }

            if (content.Content is Control settled)
            {
                settled.Transitions = null;
                settled.Opacity = 1;
                settled.RenderTransform = null;
            }
        }
        else if (content.Content?.GetType() != page.GetType())
        {
            // Page changes cross-fade with a slight rise.
            page.Opacity = 0;
            page.RenderTransform = new TranslateTransform(0, 8);
            page.Transitions =
            [
                new DoubleTransition { Property = OpacityProperty, Duration = SlideIn, Easing = new CubicEaseOut() },
                new TransformOperationsTransition { Property = RenderTransformProperty, Duration = SlideIn, Easing = new CubicEaseOut() },
            ];
            content.Content = page;
            Dispatcher.UIThread.Post(() =>
            {
                page.Opacity = 1;
                page.RenderTransform = Avalonia.Media.Transformation.TransformOperations.Identity;
            }, DispatcherPriority.Render);
        }

        foreach (var item in items)
        {
            item.Selected = item.Destination == current;
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
        if (target is not null && content.Content is AmiiboPage page)
        {
            page.FocusOn(target);
        }
    }

    /// <summary>Show or hide the navigation without animating (the screenshot harness).</summary>
    public void SetNavigationShown(bool value)
    {
        if (value)
        {
            Open(focusItem: false);
            navShift.Transitions = null;
            navShift.X = 0;
            navShift.Y = 0;
            scrim.Transitions = null;
            scrim.Opacity = 1;
        }
        else
        {
            Dismiss(restoreFocus: false);
            FinishHide();
        }
    }

    /* ------------------------------------------------------------ navigation */

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
        if (touchHost.IsVisible)
        {
            return;
        }

        if (!shown)
        {
            var focused = FocusManager?.GetFocusedElement();
            pageFocus = focused is Visual visual && content.IsVisualAncestorOf(visual) ? focused : null;
        }

        shown = true;
        SetItemsReachable(true);
        hideTimer.Stop();
        nav.IsVisible = true;
        scrim.IsVisible = true;
        scrim.IsHitTestVisible = true;
        Slide(toShown: true);
        SyncPad();
        if (focusItem)
        {
            var entry = items.FirstOrDefault(item => item.Destination == current) ?? items[0];
            Dispatcher.UIThread.Post(() => entry.Focus(NavigationMethod.Directional), DispatcherPriority.Input);
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
        scrim.IsHitTestVisible = false;
        Slide(toShown: false);
        hideTimer.Stop();
        hideTimer.Start();
        SyncPad();

        // Focus never stays on a navigation item that is leaving: back to
        // where it was on the page, or the page's own first stop.
        if (restoreFocus && pageFocus is InputElement { IsEffectivelyVisible: true } previous && previous.GetVisualRoot() is not null)
        {
            previous.Focus(NavigationMethod.Directional);
        }
        else if (keysDriving)
        {
            FocusPageSoon();
        }
        else if (items.Any(item => item.IsFocused))
        {
            FocusManager?.ClearFocus();
        }

        pageFocus = null;
    }

    /// <summary>
    /// Items take focus only while the navigation is out, from the moment it
    /// starts to leave: a directional move during the 120 ms slide-out must not
    /// land in it.
    /// </summary>
    private void SetItemsReachable(bool reachable)
    {
        foreach (var item in items)
        {
            item.Focusable = reachable;
        }
    }

    /// <summary>The slide-out has finished: take the navigation out of layout and the focus search.</summary>
    private void FinishHide()
    {
        hideTimer.Stop();
        if (!shown)
        {
            nav.IsVisible = false;
            scrim.IsVisible = false;
        }
    }

    private void Slide(bool toShown)
    {
        var (x, y) = Away();
        var duration = toShown ? SlideIn : SlideOut;
        var easing = toShown ? (Easing)new CubicEaseOut() : new CubicEaseIn();
        if (toShown && navShift.X == 0 && navShift.Y == 0 && nav.IsVisible && !shown)
        {
            // Not reachable today; kept so a re-entrant open never starts on screen.
            navShift.X = x;
            navShift.Y = y;
        }

        navShift.Transitions =
        [
            new DoubleTransition { Property = TranslateTransform.XProperty, Duration = duration, Easing = easing },
            new DoubleTransition { Property = TranslateTransform.YProperty, Duration = duration, Easing = easing },
        ];
        scrim.Transitions = [new DoubleTransition { Property = OpacityProperty, Duration = duration }];
        navShift.X = toShown ? 0 : x;
        navShift.Y = toShown ? 0 : y;
        scrim.Opacity = toShown ? 1 : 0;
    }

    /// <summary>Where the navigation rests while away: just past its edge, shadow and all.</summary>
    private (double X, double Y) Away()
    {
        const double shadow = 28;
        return placement switch
        {
            NavPlacement.Right => (Sizes.NavSideWidth + shadow, 0),
            NavPlacement.Top => (0, -(Sizes.NavBarHeight + shadow)),
            NavPlacement.Bottom => (0, Sizes.NavBarHeight + shadow),
            _ => (-(Sizes.NavSideWidth + shadow), 0),
        };
    }

    /// <summary>Resolve Automatic against the window and lay the navigation out for its edge.</summary>
    private void ApplyPlacement()
    {
        var size = Bounds.Size.Width > 0 ? Bounds.Size : new Size(Width, Height);
        var resolved = NavPlacements.Resolve(chosen, landscape: size.Width > size.Height);
        var fullNames = size.Width >= Sizes.NavFullNamesMin;
        var rebuild = resolved != placement || nav.Child is null;
        placement = resolved;
        var vertical = placement.IsVertical();
        foreach (var item in items)
        {
            item.ShowLabel = !vertical && item != settingsItem;
            item.FullName = fullNames;
        }

        if (!rebuild)
        {
            return;
        }

        foreach (var item in items)
        {
            (item.Parent as Panel)?.Children.Remove(item);
        }

        (dot.Parent as Panel)?.Children.Remove(dot);

        var pages = new StackPanel
        {
            Orientation = vertical ? Orientation.Vertical : Orientation.Horizontal,
            Spacing = vertical ? 8 : 4,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        pages.Children.AddRange(items.Where(item => item != settingsItem));

        var bar = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(dot, vertical ? Dock.Top : Dock.Left);
        DockPanel.SetDock(settingsItem, vertical ? Dock.Bottom : Dock.Right);
        bar.Children.Add(dot);
        bar.Children.Add(settingsItem);
        bar.Children.Add(pages);

        nav.Child = bar;
        nav.Padding = vertical ? new Thickness(0, 12) : new Thickness(16, 0);
        nav.Width = vertical ? Sizes.NavSideWidth : double.NaN;
        nav.Height = vertical ? double.NaN : Sizes.NavBarHeight;
        nav.HorizontalAlignment = placement switch
        {
            NavPlacement.Left => HorizontalAlignment.Left,
            NavPlacement.Right => HorizontalAlignment.Right,
            _ => HorizontalAlignment.Stretch,
        };
        nav.VerticalAlignment = placement switch
        {
            NavPlacement.Top => VerticalAlignment.Top,
            NavPlacement.Bottom => VerticalAlignment.Bottom,
            _ => VerticalAlignment.Stretch,
        };

        if (!shown)
        {
            navShift.Transitions = null;
            var (x, y) = Away();
            navShift.X = x;
            navShift.Y = y;
        }
        else
        {
            navShift.Transitions = null;
            navShift.X = 0;
            navShift.Y = 0;
        }
    }

    /* ------------------------------------------------------------------- keys */

    private Key TowardNav => placement switch
    {
        NavPlacement.Right => Key.Right,
        NavPlacement.Top => Key.Up,
        NavPlacement.Bottom => Key.Down,
        _ => Key.Left,
    };

    private Key TowardPage => placement switch
    {
        NavPlacement.Right => Key.Left,
        NavPlacement.Top => Key.Down,
        NavPlacement.Bottom => Key.Up,
        _ => Key.Right,
    };

    private void OnKeyDownTunnel(object? sender, KeyEventArgs e)
    {
        if (e.Key is not (Key.LeftShift or Key.RightShift or Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt))
        {
            keysDriving = true;
        }

        if (e.Key == Key.F11)
        {
            ToggleFullScreen();
            e.Handled = true;
            return;
        }

        // The Touch Gamepad is a mode with its own keys; a dialog closes on its own Escape.
        if (touchHost.IsVisible || Dialogs.IsVisible)
        {
            return;
        }

        if (e.Key == Key.Escape)
        {
            back.Down(clock.ElapsedMilliseconds, repeat: back.Pressed);
            backTimer.Start();
            e.Handled = true;
            return;
        }

        if (!shown)
        {
            // Nothing focused (after a click, after a pane closed): the first
            // arrow lands on the page's own first stop, never on whatever
            // Avalonia's search from the window's corner finds first. A page
            // with nothing to focus (everything waits for an adapter) still
            // lets the arrow toward the navigation bring it in.
            if (IsArrow(e.Key) &&
                (FocusManager?.GetFocusedElement() is not Visual focused || !content.IsVisualAncestorOf(focused)))
            {
                if (!FocusPage() && e.Key == TowardNav)
                {
                    Open(focusItem: true);
                }

                e.Handled = true;
            }

            return;
        }

        // Inside the navigation: along it moves among the items, toward the
        // page sends it away, and toward the screen's edge (or past either
        // end) goes nowhere rather than escaping to whatever is nearest.
        var vertical = placement.IsVertical();
        var previousKey = vertical ? Key.Up : Key.Left;
        var nextKey = vertical ? Key.Down : Key.Right;
        if (e.Key == previousKey || e.Key == nextKey || e.Key == Key.Tab)
        {
            var step = e.Key == previousKey || (e.Key == Key.Tab && e.KeyModifiers.HasFlag(KeyModifiers.Shift)) ? -1 : 1;
            var index = items.FindIndex(item => item.IsFocused);
            var next = index < 0
                ? items.FirstOrDefault(item => item.Destination == current) ?? items[0]
                : items[Math.Clamp(index + step, 0, items.Count - 1)];
            next.Focus(NavigationMethod.Directional);
            e.Handled = true;
        }
        else if (e.Key == TowardPage)
        {
            Dismiss(restoreFocus: true);
            e.Handled = true;
        }
        else if (e.Key == TowardNav)
        {
            e.Handled = true;
        }
    }

    private void OnKeyDownBubble(object? sender, KeyEventArgs e)
    {
        if (e.Handled || shown || touchHost.IsVisible || Dialogs.IsVisible || !IsArrow(e.Key))
        {
            return;
        }

        // Nothing left that way inside the page: the navigation comes in.
        if (e.Key == TowardNav)
        {
            Open(focusItem: true);
            e.Handled = true;
        }
    }

    private void OnKeyUpTunnel(object? sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape || !back.Pressed)
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

    /// <summary>Holding Back while streaming: give the controller back to this computer.</summary>
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
        if (touchHost.IsVisible)
        {
            return;
        }

        if (Dialogs.IsVisible)
        {
            Dialogs.Close(-1);
            return;
        }

        if (shown)
        {
            Dismiss(restoreFocus: true);
            return;
        }

        if (content.Content is CompanionPage page && page.HandleBack())
        {
            return;
        }

        Open(focusItem: keysDriving);
    }

    private void OnPointerPressedTunnel(object? sender, PointerPressedEventArgs e)
    {
        keysDriving = false;
        if (shown || touchHost.IsVisible || Dialogs.IsVisible)
        {
            return;
        }

        // The touch screen's way in: a press at the navigation's edge.
        var at = e.GetPosition(this);
        var size = Bounds.Size;
        var atEdge = placement switch
        {
            NavPlacement.Right => at.X >= size.Width - EdgeGrab,
            NavPlacement.Top => at.Y <= EdgeGrab,
            NavPlacement.Bottom => at.Y >= size.Height - EdgeGrab,
            _ => at.X <= EdgeGrab,
        };
        if (atEdge)
        {
            Open(focusItem: false);
            e.Handled = true;
        }
    }

    private static bool IsArrow(Key key) => key is Key.Left or Key.Right or Key.Up or Key.Down;

    private void FocusPageSoon() => Dispatcher.UIThread.Post(() => FocusPage(), DispatcherPriority.Loaded);

    /// <summary>The page's own first stop: its preferred control, else the first focusable one.</summary>
    private bool FocusPage()
    {
        if (shown || content.Content is not Control page)
        {
            return false;
        }

        var target = (page as CompanionPage)?.PreferredFocus is { IsEffectivelyVisible: true, IsEffectivelyEnabled: true } preferred
            ? preferred
            : page.GetVisualDescendants().OfType<InputElement>().FirstOrDefault(element =>
                element.Focusable && element.IsTabStop && element.IsEffectivelyVisible && element.IsEffectivelyEnabled);
        if (target is not null)
        {
            return target.Focus(NavigationMethod.Directional);
        }

        if (FocusManager?.GetFocusedElement() is Visual stale && nav.IsVisualAncestorOf(stale))
        {
            // Never leave focus on a navigation that has gone.
            FocusManager.ClearFocus();
        }

        return false;
    }

    /* -------------------------------------------------------------- controller */

    private void OnPhysicalFrame(ControllerButtonSet buttons, AnalogFrame analog) =>
        Dispatcher.UIThread.Post(() => OnPadFrame(buttons, analog), DispatcherPriority.Input);

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

    private void OnPadTick()
    {
        if (AppOwnsPad() && gamepadNavigator.Tick(clock.ElapsedMilliseconds) is { } command)
        {
            OnPadCommand(command);
        }
    }

    /// <summary>The app's menus read the controller: not streaming, or something of the app's on top.</summary>
    private bool AppOwnsPad() => appHeldPad && !touchHost.IsVisible && IsActive;

    /// <summary>
    /// A controller step, delivered as the key a keyboard would send, so every
    /// control that already answers the arrows, Enter and Escape answers the
    /// controller the same way (lists, the carousel, dialogs, menus).
    /// </summary>
    private void OnPadCommand(UiCommand command)
    {
        keysDriving = true;
        switch (command)
        {
            case UiCommand.PreviousPage or UiCommand.NextPage when !Dialogs.IsVisible:
                Choose(ShellPages.Step(current, command == UiCommand.NextPage ? 1 : -1));
                return;
            case UiCommand.PreviousPage or UiCommand.NextPage:
                return;
        }

        var key = command switch
        {
            UiCommand.Up => Key.Up,
            UiCommand.Down => Key.Down,
            UiCommand.Left => Key.Left,
            UiCommand.Right => Key.Right,
            UiCommand.Confirm => Key.Enter,
            _ => Key.Escape,
        };

        var target = FocusManager?.GetFocusedElement() as InputElement ?? this;
        if (Dialogs.IsVisible)
        {
            // A dialog is modal for the controller too: Back closes it, and
            // focus left behind it is brought inside before anything moves.
            if (command == UiCommand.Back)
            {
                Dialogs.Close(-1);
                return;
            }

            if (!Dialogs.IsVisualAncestorOf(target))
            {
                (KeyboardNavigationHandler.GetNext(Dialogs, NavigationDirection.Next) as InputElement)?.Focus(NavigationMethod.Directional);
                return;
            }
        }

        foreach (var routed in new[] { KeyDownEvent, KeyUpEvent })
        {
            target.RaiseEvent(new KeyEventArgs
            {
                RoutedEvent = routed,
                Key = key,
                Source = target,
                KeyDeviceType = KeyDeviceType.Gamepad,
            });
        }
    }

    /// <summary>
    /// Who owns the physical controller (<see cref="PadOwnership"/>), recomputed
    /// whenever any of its inputs changes: Controller Link, the page, the
    /// navigation, a dialog.
    /// </summary>
    private void SyncPad()
    {
        var overlay = Dialogs.IsVisible || touchHost.IsVisible || current == "diagnostics";
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

    /* ---------------------------------------------------------- touch gamepad */

    /// <summary>
    /// Enter the Touch Gamepad: a mode that REPLACES the shell, not a page in
    /// it (docs/architecture/companion-shell-layout.md). The page and the
    /// navigation are hidden outright rather than painted over, the same
    /// structure as Android's early return in CompanionApp.kt and Windows'
    /// MainWindow.ShowTouchGamepad, whose first version layered the surface over
    /// a live shell and let it show through.
    ///
    /// Built fresh each time so leaving really unsubscribes it; what is worth
    /// keeping across visits (toolbar dock, alignment, the profile library) is
    /// persisted by the surface and the shared service.
    /// </summary>
    public void ShowTouchGamepad()
    {
        if (touchHost.Children.Count > 0)
        {
            return;
        }

        Dismiss(restoreFocus: false);
        FinishHide();
        var view = new TouchGamepadView(this, WindowState == WindowState.FullScreen);
        view.CloseRequested += HideTouchGamepad;
        view.FullScreenToggleRequested += ToggleFullScreen;
        touchHost.Children.Add(view);
        touchHost.IsVisible = true;
        shellLayer.IsVisible = false;
        SyncPad();
    }

    /// <summary>Leave the Touch Gamepad. Full screen is the app's state and is left as the user chose.</summary>
    public void HideTouchGamepad()
    {
        if (touchHost.Children.Count == 0)
        {
            return;
        }

        touchHost.Children.Clear();
        touchHost.IsVisible = false;
        shellLayer.IsVisible = true;
        SyncPad();
    }

    /* ------------------------------------------------------------ full screen */

    private void ToggleFullScreen()
    {
        if (WindowState == WindowState.FullScreen)
        {
            wantFullScreen = false;
            WindowState = restoreState == WindowState.FullScreen ? WindowState.Normal : restoreState;
        }
        else
        {
            restoreState = WindowState;
            wantFullScreen = true;
            WindowState = WindowState.FullScreen;
            FillScreen();
        }
    }

    /* ------------------------------------------------------------------- misc */

    private void ShowToast(string message)
    {
        toastText.Text = message;
        toast.IsVisible = true;
        toastTimer.Stop();
        toastTimer.Start();
    }

    private void OnLinkChanged() => AppServices.OnUiThread(SyncPad);

    private void OnConnectionChanged() => AppServices.OnUiThread(dot.Render);

    /// <summary>
    /// The first size, from the screen the window will open on.
    ///
    /// Proportional rather than fixed: a fixed 1280x800 was taller than a
    /// 1920x1080 panel at 1.5x (1280x720 of logical space) and absurdly small on
    /// a 4K monitor. A screen with too little room for a comfortable window gets
    /// a maximised one instead, which is what a handheld user wants anyway.
    /// </summary>
    private void SizeToScreen()
    {
        Width = 1200;
        Height = 760;
        if (Screens.Primary is not { } screen)
        {
            return;
        }

        if (UnderGamescope)
        {
            // No restored window exists in Game Mode: start at the panel's size.
            Width = screen.Bounds.Width / screen.Scaling;
            Height = screen.Bounds.Height / screen.Scaling;
            Position = screen.Bounds.Position;
            return;
        }

        var area = screen.WorkingArea.Size.ToSize(screen.Scaling);
        if (area.Width < 1280 || area.Height < 800)
        {
            WindowState = WindowState.Maximized;
            Width = Math.Max(MinWidth, area.Width);
            Height = Math.Max(MinHeight, area.Height);
            return;
        }

        Width = Math.Clamp(area.Width * 0.72, 1100, 1600);
        Height = Math.Clamp(area.Height * 0.82, 740, 1100);
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
    }

    private static WindowIcon? LoadIcon()
    {
        try
        {
            using var stream = AssetLoader.Open(new Uri("avares://PicoSwitch2Companion/Assets/picoswitch2.png"));
            return new WindowIcon(stream);
        }
        catch (Exception)
        {
            // An icon is not worth failing to start over.
            return null;
        }
    }
}

/// <summary>
/// One destination: icon only at the side, icon and name at the top or bottom.
/// Selected by a filled pill and the accent icon, not colour alone (the name
/// turns semibold). Lifts under a controller like Play's tiles (the "lift" style).
/// </summary>
internal sealed class NavItem : Button
{
    private readonly TextBlock label;
    private readonly PathIcon glyph;
    private readonly string fullName;
    private readonly string shortName;
    private readonly bool iconOnly;

    public NavItem(string tag, string text, string shortText, string icon, Action open, bool iconOnly = false)
    {
        Destination = tag;
        fullName = text;
        shortName = shortText;
        this.iconOnly = iconOnly;
        Height = Sizes.NavItemSize;
        MinWidth = Sizes.NavItemSize;
        Padding = new Thickness(14, 0);
        HorizontalAlignment = HorizontalAlignment.Center;
        VerticalAlignment = VerticalAlignment.Center;
        HorizontalContentAlignment = HorizontalAlignment.Center;
        VerticalContentAlignment = VerticalAlignment.Center;
        Background = Brushes.Transparent;
        BorderThickness = new Thickness(2);
        BorderBrush = Brushes.Transparent;
        CornerRadius = new CornerRadius(Sizes.NavItemSize / 2);
        Classes.Add("lift");
        AutomationProperties.SetName(this, text);

        glyph = Ui.Icon(icon, 22);
        label = new TextBlock { Text = text, FontSize = 14, FontWeight = FontWeight.Medium, VerticalAlignment = VerticalAlignment.Center };
        Content = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Children = { glyph, label },
        };

        Click += (_, _) => open();
    }

    protected override Type StyleKeyOverride => typeof(Button);

    public string Destination { get; }

    public bool ShowLabel
    {
        set
        {
            var visible = value && !iconOnly;
            label.IsVisible = visible;
            Padding = visible ? new Thickness(14, 0) : new Thickness(0);
            Width = visible ? double.NaN : Sizes.NavItemSize;
            ToolTip.SetTip(this, visible ? null : fullName);
        }
    }

    /// <summary>Full names, or one word when the bar is too narrow (never an ellipsis).</summary>
    public bool FullName
    {
        set => label.Text = value ? fullName : shortName;
    }

    public bool Selected
    {
        set
        {
            label.FontWeight = value ? FontWeight.SemiBold : FontWeight.Medium;
            if (value)
            {
                this.Bind(BackgroundProperty, Tokens.AccentSoft);
                glyph.Bind(PathIcon.ForegroundProperty, Tokens.AccentText);
            }
            else
            {
                Background = Brushes.Transparent;
                glyph.ClearValue(PathIcon.ForegroundProperty);
            }
        }
    }
}

/// <summary>
/// The app's one connection indicator: a dot, never a control. Not focusable,
/// so the D-pad never stops on it; the Adapter page is the item beside it. Its
/// words (the tooltip and the accessible name) are the shared <see cref="AdapterStatus"/>.
/// </summary>
internal sealed class AdapterDot : Panel
{
    private readonly Ellipse ring = new() { Width = 26, Height = 26, StrokeThickness = 2, IsVisible = false };
    private readonly Ellipse dot = new() { Width = 14, Height = 14 };

    public AdapterDot()
    {
        Width = Sizes.NavItemSize;
        Height = Sizes.NavItemSize;
        Focusable = false;
        Background = Brushes.Transparent;
        ring.HorizontalAlignment = HorizontalAlignment.Center;
        ring.VerticalAlignment = VerticalAlignment.Center;
        dot.HorizontalAlignment = HorizontalAlignment.Center;
        dot.VerticalAlignment = VerticalAlignment.Center;
        Children.Add(ring);
        Children.Add(dot);
        AutomationProperties.SetLiveSetting(this, AutomationLiveSetting.Polite);
    }

    public void Render()
    {
        var adapters = AppServices.Adapters;
        var connection = adapters.Connection.Value;
        var relationship = adapters.Relationship.Value;
        var registry = adapters.Registry.Value;
        var record = registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();

        var name = record?.DisplayName ?? connection.DeviceName ?? "No adapter";
        var (tone, words) = AdapterStatus.Of(connection.Connected, relationship.Phase);
        var brush = Ui.ToneBrush(tone);
        dot.Fill = brush;

        // A ring while a connection is in progress: the spinner's place on Android.
        ring.Stroke = brush;
        ring.IsVisible = !connection.Connected && AdapterStatus.InProgress(relationship.Phase);

        var detail = relationship.Message ?? connection.Message;
        AutomationProperties.SetName(this, $"{name}, {words}");
        ToolTip.SetTip(this, detail is null ? $"{name}: {words}" : $"{name}: {words}. {detail}");
    }
}
