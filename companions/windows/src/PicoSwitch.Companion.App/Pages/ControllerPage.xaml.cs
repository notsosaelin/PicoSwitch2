using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Platform.Input;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Companion.Windows.Input;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Play: the home screen (docs/architecture/companion-shell-layout.md, "Play"),
/// laid out like a console home screen rather than a settings form. Android's
/// PlayScreen is the first implementation and Linux's PlayPage the same page
/// in Avalonia; this follows them tile for tile.
///
/// - Left column: the HEADSET strip on top (96 tall), the CONTROLLER card
///   below taking the rest: which controller this PC reads (Change /
///   Refresh), a one-phrase state with the console's Player N beside it, one
///   line only when something blocks starting, and at the bottom edge one row
///   of equal actions (Home, Capture, C held while streaming; otherwise Touch
///   Gamepad and Wake or the adapter's next step) over Start / Stop.
/// - Right column: the AMIIBO the adapter holds, full height, as art and a
///   name (<see cref="PlayAmiiboTile"/>); "GAME WANTS AN AMIIBO" while a game
///   has an NFC scan running.
/// - Every action is the one action button (52 tall, radius 14).
/// - The adapter's state is the navigation's dot; Play has no adapter card.
/// - Side by side when the columns fit; narrower or shorter windows stack and
///   scroll.
///
/// Every word comes from a shared projection, so this page, Linux's and (in
/// meaning) Android's cannot disagree. The desktop headset keeps its separate
/// microphone toggle: the microphone is a wish the adapter may refuse.
/// </summary>
public sealed partial class ControllerPage : Page, IShellPage
{
    private const double PageMaxWidth = 1240;
    private const double HubMinWidth = 600;
    private const double HubMinHeight = 280;
    private const double HeadsetStripHeight = 96;
    private const double StackedHeadsetHeight = 120;
    private const double StackedAmiiboHeight = 300;

    private readonly ControllerLinkService controllerLink = AppServices.ControllerLink;
    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly WindowsGamepadInputSource gamepad = AppServices.PhysicalGamepad;
    private readonly CompanionAudioService companionAudio = AppServices.CompanionAudio;

    private readonly Grid hub = new() { ColumnSpacing = 16 };
    private readonly Grid left = new() { RowSpacing = 16 };
    private readonly StackPanel stack = new() { Spacing = 12 };
    private bool? hubShown;

    private readonly Border heroCard = new();
    private readonly TextBlock controllerName = new() { FontSize = 16, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock controllerState = new() { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Button sourceButton = new() { Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0) };
    private readonly TextBlock headline = new() { FontSize = 28, FontWeight = Microsoft.UI.Text.FontWeights.Bold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    private readonly Border playerPill = new() { Margin = new Thickness(12, 0, 0, 0), Visibility = Visibility.Collapsed };
    private readonly TextBlock playerText = new() { FontSize = 13, FontWeight = Microsoft.UI.Text.FontWeights.Bold };
    private readonly TextBlock blocker = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly InfoBar sourceAdvice = new() { IsClosable = false, Severity = InfoBarSeverity.Informational };
    private readonly InfoBar playNotice = new() { IsClosable = true };
    private readonly Grid consoleRow = EqualRow(3);
    private readonly Grid quickRow = EqualRow(2);
    private Button touchButton = null!;
    private Button nextButton = null!;
    private Button startStop = null!;

    private readonly Border headsetTile = new();
    private Button headsetButton = null!;
    private readonly Border headsetIcon = new() { Width = 56, Height = 56, CornerRadius = new CornerRadius(28) };
    private readonly TextBlock headsetTitle = new() { FontSize = 20, FontWeight = Microsoft.UI.Text.FontWeights.Bold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    private readonly Border liveBadge = new() { Margin = new Thickness(8, 0, 0, 0), Visibility = Visibility.Collapsed };
    private readonly TextBlock headsetLine = new() { FontSize = 13, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly ProgressRing headsetBusy = new() { Width = 28, Height = 28, IsActive = false, Visibility = Visibility.Collapsed, Margin = new Thickness(0, 0, 8, 0) };
    private readonly ToggleButton micToggle = new() { Width = 52, Height = 52, Padding = new Thickness(0), CornerRadius = new CornerRadius(14), Margin = new Thickness(0, 0, 22, 0), VerticalAlignment = VerticalAlignment.Center };

    private readonly Border amiiboTile = new();
    private readonly Border wantedBadge = new() { HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 0, 0, 8), Visibility = Visibility.Collapsed };
    private readonly Image amiiboArt = new() { Stretch = Stretch.Uniform };
    private readonly Border amiiboPlaceholder = new() { Width = 96, Height = 96, CornerRadius = new CornerRadius(48) };
    private readonly TextBlock amiiboTitle = new() { FontSize = 20, FontWeight = Microsoft.UI.Text.FontWeights.Bold, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Center };
    private readonly TextBlock amiiboSubtitle = new() { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Center };
    private Button amiiboOpen = null!;
    private Button amiiboAction = null!;
    private PlayAmiiboTile? amiibo;
    private string? shownArt;

    private DispatcherTimer? adviceTimer;
    private string? shownAdvice;
    private readonly HashSet<ControllerButton> held = [];

    public ControllerPage()
    {
        InitializeComponent();
        Build();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        Scroller.SizeChanged += (_, e) => Arrange(e.NewSize.Width, e.NewSize.Height);
    }

    /// <summary>A controller or keyboard arriving on Play lands on Start / Stop, so A is always "play".</summary>
    public Control? PreferredFocus => startStop;

    public bool HandleBack() => false;

    public bool HandleConfirm(DependencyObject? focused) => false;

    /* --------------------------------------------------------------- build */

    private Style Look(string key) => (Style)Resources[key];

    private void Build()
    {
        /* ---- controller card */
        var controllerIcon = new Border
        {
            Width = 40,
            Height = 40,
            CornerRadius = new CornerRadius(20),
            Style = Look("Circle"),
            Child = new FontIcon { Glyph = "", FontSize = 20, Foreground = (Brush)Application.Current.Resources["AccentTextFillColorPrimaryBrush"] },
        };
        controllerState.Style = Look("Secondary");
        var who = new StackPanel { Margin = new Thickness(12, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center, Children = { controllerName, controllerState } };
        AutomationProperties.SetName(sourceButton, "Change or refresh the controller");
        var header = new Grid { ColumnDefinitions = { new() { Width = GridLength.Auto }, new() { Width = new GridLength(1, GridUnitType.Star) }, new() { Width = GridLength.Auto } } };
        Grid.SetColumn(who, 1);
        Grid.SetColumn(sourceButton, 2);
        header.Children.Add(controllerIcon);
        header.Children.Add(who);
        header.Children.Add(sourceButton);

        playerPill.Style = Look("Pill");
        playerPill.Child = playerText;
        var headlineRow = new StackPanel { Orientation = Orientation.Horizontal, Children = { headline, playerPill } };
        blocker.Style = Look("Secondary");

        // Hold, not click: the console tells a tap from a hold (Capture records
        // on a long hold; Home opens the power menu). Pressed THROUGH Controller
        // Link, so they exist only while it streams.
        AddConsoleButton(0, "Home", "", ControllerButton.Home);
        AddConsoleButton(1, "Capture", "", ControllerButton.Capture);
        AddConsoleButton(2, "C", "", ControllerButton.C);

        touchButton = Action("Touch Gamepad", "", "Action");
        touchButton.Click += (_, _) => (App.Window as MainWindow)?.ShowTouchGamepad();
        nextButton = Action("Wake", "", "Action");
        nextButton.Click += async (_, _) => await OnNextAsync();
        Grid.SetColumn(nextButton, 1);
        quickRow.Children.Add(touchButton);
        quickRow.Children.Add(nextButton);

        startStop = Action("Start playing", "", "ActionAccent");
        startStop.Click += async (_, _) => await OnStartStopAsync();

        var top = new StackPanel { Spacing = 8, Children = { header, headlineRow, blocker, sourceAdvice, playNotice } };
        var bottom = new StackPanel { Spacing = 8, VerticalAlignment = VerticalAlignment.Bottom, Children = { consoleRow, quickRow, startStop } };
        var heroBody = new Grid { RowDefinitions = { new() { Height = GridLength.Auto }, new() { Height = new GridLength(1, GridUnitType.Star) } } };
        Grid.SetRow(bottom, 1);
        heroBody.Children.Add(top);
        heroBody.Children.Add(bottom);
        heroCard.Style = Look("PlayHero");
        heroCard.Child = heroBody;

        /* ---- headset */
        headsetIcon.Style = Look("Circle");
        headsetIcon.Child = new FontIcon { Glyph = "", FontSize = 26 };
        liveBadge.Style = Look("Badge");
        liveBadge.Child = new TextBlock { Text = "LIVE", Style = Look("BadgeText") };
        headsetLine.Style = Look("Secondary");
        var titleRow = new StackPanel { Orientation = Orientation.Horizontal, Children = { headsetTitle, liveBadge } };
        var headsetText = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(14, 0, 8, 0), Children = { titleRow, headsetLine } };
        var headsetContent = new Grid { ColumnDefinitions = { new() { Width = GridLength.Auto }, new() { Width = new GridLength(1, GridUnitType.Star) }, new() { Width = GridLength.Auto } } };
        Grid.SetColumn(headsetText, 1);
        Grid.SetColumn(headsetBusy, 2);
        headsetContent.Children.Add(headsetIcon);
        headsetContent.Children.Add(headsetText);
        headsetContent.Children.Add(headsetBusy);
        headsetButton = TileButton(headsetContent);
        headsetButton.Click += async (_, _) => await OnHeadsetToggleAsync();
        ToolTipService.SetToolTip(headsetButton, "Play console audio on this PC (experimental)");

        micToggle.Content = new FontIcon { Glyph = "", FontSize = 18 };
        Lift.Apply(micToggle);
        micToggle.Click += async (_, _) => await OnMicToggleAsync();

        var headsetRow = new Grid { ColumnDefinitions = { new() { Width = new GridLength(1, GridUnitType.Star) }, new() { Width = GridLength.Auto } } };
        Grid.SetColumn(micToggle, 1);
        headsetRow.Children.Add(headsetButton);
        headsetRow.Children.Add(micToggle);
        headsetTile.Style = Look("PlayTile");
        headsetTile.Child = headsetRow;

        /* ---- amiibo */
        wantedBadge.Style = Look("Badge");
        wantedBadge.HorizontalAlignment = HorizontalAlignment.Center;
        wantedBadge.Child = new TextBlock { Text = "GAME WANTS AN AMIIBO", Style = Look("BadgeText") };
        amiiboPlaceholder.Style = Look("Circle");
        amiiboPlaceholder.Child = new FontIcon { Glyph = "", FontSize = 44 };
        amiiboSubtitle.Style = Look("Secondary");
        var art = new Grid { Children = { amiiboPlaceholder, amiiboArt } };
        var names = new StackPanel { Spacing = 2, Margin = new Thickness(0, 8, 0, 0), Children = { amiiboTitle, amiiboSubtitle } };
        var openContent = new Grid
        {
            RowDefinitions = { new() { Height = GridLength.Auto }, new() { Height = new GridLength(1, GridUnitType.Star) }, new() { Height = GridLength.Auto } },
        };
        Grid.SetRow(art, 1);
        Grid.SetRow(names, 2);
        openContent.Children.Add(wantedBadge);
        openContent.Children.Add(art);
        openContent.Children.Add(names);
        amiiboOpen = TileButton(openContent);
        amiiboOpen.Padding = new Thickness(16, 16, 16, 8);
        amiiboOpen.Click += (_, _) => (App.Window as MainWindow)?.OpenAmiiboLibrary(amiibo?.LibraryTarget);

        amiiboAction = Action("Library", "", "Action");
        amiiboAction.Margin = new Thickness(16, 4, 16, 16);
        amiiboAction.Click += async (_, _) => await OnAmiiboActionAsync();
        var amiiboBody = new Grid { RowDefinitions = { new() { Height = new GridLength(1, GridUnitType.Star) }, new() { Height = GridLength.Auto } } };
        Grid.SetRow(amiiboAction, 1);
        amiiboBody.Children.Add(amiiboOpen);
        amiiboBody.Children.Add(amiiboAction);
        amiiboTile.Style = Look("PlayTile");
        amiiboTile.Child = amiiboBody;

        hub.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.35, GridUnitType.Star) });
        hub.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        left.RowDefinitions.Add(new RowDefinition { Height = new GridLength(HeadsetStripHeight) });
        left.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
    }

    private Button Action(string label, string glyph, string style)
    {
        var button = new Button { Style = Look(style) };
        SetAction(button, label, glyph);
        Lift.Apply(button);
        return button;
    }

    /// <summary>A label too long for its share of the row shrinks to fit; it never ends in an ellipsis.</summary>
    private static void SetAction(Button button, string label, string glyph)
    {
        button.Content = new Viewbox
        {
            StretchDirection = StretchDirection.DownOnly,
            Child = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
                Children =
                {
                    new FontIcon { Glyph = glyph, FontSize = 18 },
                    new TextBlock { Text = label, FontSize = 15, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center },
                },
            },
        };
        AutomationProperties.SetName(button, label);
    }

    private Button TileButton(UIElement content)
    {
        var button = new Button { Style = Look("TileButton"), Content = content };

        // The tile's own colour shows through in every state; WinUI would paint
        // a disabled or pressed button grey over it.
        foreach (var key in new[] { "ButtonBackgroundDisabled", "ButtonBackgroundPressed", "ButtonBorderBrushDisabled", "ButtonBackgroundPointerOver" })
        {
            button.Resources[key] = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        }

        Lift.Apply(button);
        return button;
    }

    private static Grid EqualRow(int columns)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        for (var i = 0; i < columns; i++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }

        return grid;
    }

    /* ----------------------------------------------------------- lifecycle */

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        controllerLink.View.Changed += OnViewChanged;
        gamepad.SourcesChanged += OnSourcesChanged;
        adapters.Connection.Changed += OnAdapterChanged;
        adapters.Relationship.Changed += OnAdapterChanged;
        adapters.Registry.Changed += OnAdapterChanged;
        adapters.Snapshot.Changed += OnAdapterChanged;
        companionAudio.View.Changed += OnHeadsetChanged;
        AppServices.AmiiboLibrary.Items.Changed += OnAdapterChanged;
        Render();
        RenderSources();
        RenderHeadset();

        // A controller plugged in while this page was closed is the ordinary
        // case, so look again on arrival; the catalog names the figure.
        _ = AppServices.RefreshControllerSourcesAsync();
        _ = AppServices.AmiiboCatalog.EnsureLoadedAsync().ContinueWith(_ => AppServices.OnUiThread(RenderAmiibo), TaskScheduler.Default);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        controllerLink.View.Changed -= OnViewChanged;
        gamepad.SourcesChanged -= OnSourcesChanged;
        adapters.Connection.Changed -= OnAdapterChanged;
        adapters.Relationship.Changed -= OnAdapterChanged;
        adapters.Registry.Changed -= OnAdapterChanged;
        adapters.Snapshot.Changed -= OnAdapterChanged;
        companionAudio.View.Changed -= OnHeadsetChanged;
        AppServices.AmiiboLibrary.Items.Changed -= OnAdapterChanged;

        // A DispatcherTimer holds a strong reference through its Tick handler,
        // so leaving it running keeps this page alive after navigation.
        adviceTimer?.Stop();
        ReleaseHeld();
    }

    private void OnViewChanged() => AppServices.OnUiThread(Render);

    private void OnSourcesChanged() => AppServices.OnUiThread(RenderSources);

    private void OnAdapterChanged() => AppServices.OnUiThread(Render);

    private void OnHeadsetChanged() => AppServices.OnUiThread(RenderHeadset);

    /* -------------------------------------------------------------- layout */

    /// <summary>Side by side when the columns fit (measured on the page, never a device), else stacked.</summary>
    private void Arrange(double width, double height)
    {
        var isShort = height < 560;
        var wantHub = width >= HubMinWidth && height >= HubMinHeight;
        var gap = isShort ? 8 : 16;
        hub.ColumnSpacing = gap;
        left.RowSpacing = gap;
        PageFill.Padding = wantHub
            ? isShort ? new Thickness(16, 4, 16, 8) : new Thickness(32, 12, 32, 24)
            : new Thickness(16, 12, 16, 24);

        // The hub fills the viewport exactly, so it never scrolls. The width is
        // set, not left to MaxWidth: a stretched Border inside a ScrollViewer
        // is not centred by MaxWidth alone, and the hub drew off the right edge
        // of a 2560-wide full-screen window.
        PageFill.Width = Math.Min(width, PageMaxWidth);
        PageFill.Height = wantHub ? height : double.NaN;
        Scroller.VerticalScrollMode = wantHub ? ScrollMode.Disabled : ScrollMode.Auto;
        if (hubShown == wantHub)
        {
            return;
        }

        hubShown = wantHub;
        Detach(headsetTile);
        Detach(heroCard);
        Detach(amiiboTile);
        PageFill.Child = null;

        if (wantHub)
        {
            headsetTile.Height = double.NaN;
            heroCard.MinHeight = 0;
            amiiboTile.Height = double.NaN;
            Grid.SetRow(heroCard, 1);
            left.Children.Add(headsetTile);
            left.Children.Add(heroCard);
            Grid.SetColumn(amiiboTile, 1);
            hub.Children.Add(left);
            hub.Children.Add(amiiboTile);
            PageFill.Child = hub;
        }
        else
        {
            headsetTile.Height = StackedHeadsetHeight;
            heroCard.MinHeight = 300;
            amiiboTile.Height = StackedAmiiboHeight;
            stack.Children.Add(heroCard);
            stack.Children.Add(headsetTile);
            stack.Children.Add(amiiboTile);
            PageFill.Child = stack;
        }
    }

    private static void Detach(FrameworkElement element)
    {
        if (element.Parent is Panel panel)
        {
            panel.Children.Remove(element);
        }
    }

    /* ---------------------------------------------------------------- link */

    private void Render()
    {
        RenderLink();
        RenderAmiibo();
    }

    private void RenderLink()
    {
        var link = controllerLink.View.Value;
        var connected = adapters.Connection.Value.Connected;
        var streaming = link.Phase == ControllerLinkPhase.Streaming;

        headline.Text = PlayHeadline.Of(link);
        var blocking = PlayHeadline.Blocker(link);
        blocker.Text = blocking;
        blocker.Visibility = blocking.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        ToolTipService.SetToolTip(headline, $"{link.Headline}. {link.Explanation}");

        var player = ConsoleCues.PlayerLabel(connected, adapters.Snapshot.Value.Input.Console);
        playerPill.Visibility = player is null ? Visibility.Collapsed : Visibility.Visible;
        playerText.Text = player ?? string.Empty;

        // One primary action at a time, the same size as every other.
        var stop = link.CanStop;
        startStop.Style = Look(stop ? "ActionDanger" : "ActionAccent");
        SetAction(startStop, stop ? "Stop" : link.Busy ? "Connecting…" : "Start playing", stop ? "" : "");
        startStop.IsEnabled = stop || link.CanStart;

        consoleRow.Visibility = streaming ? Visibility.Visible : Visibility.Collapsed;
        quickRow.Visibility = streaming ? Visibility.Collapsed : Visibility.Visible;
        if (!streaming)
        {
            ReleaseHeld();
        }

        touchButton.IsEnabled = adapters.Registry.Value.Records.Count > 0;
        var phase = adapters.Relationship.Value.Phase;
        if (connected)
        {
            SetAction(nextButton, "Wake", "");
            AutomationProperties.SetName(nextButton, "Wake the console");
            nextButton.IsEnabled = adapters.Snapshot.Value.Capabilities.Wake != CapabilityState.Unsupported;
        }
        else
        {
            SetAction(nextButton, AdapterStatus.NextStep(false, phase), "");
            nextButton.IsEnabled = !AdapterStatus.InProgress(phase);
        }

        // The card is lit while it plays.
        heroCard.Style = Look(streaming ? "PlayHeroLit" : "PlayHero");
    }

    private async Task OnStartStopAsync()
    {
        try
        {
            if (controllerLink.View.Value.CanStop)
            {
                await controllerLink.StopAsync();
            }
            else
            {
                await controllerLink.StartAsync();
            }
        }
        catch (Exception error)
        {
            Fail(error);
        }
    }

    /// <summary>Wake, or the adapter's next step when it is not connected.</summary>
    private async Task OnNextAsync()
    {
        if (adapters.Connection.Value.Connected)
        {
            await OnWakeAsync();
            return;
        }

        var phase = adapters.Relationship.Value.Phase;
        var registry = adapters.Registry.Value;
        var record = registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();
        if (!AdapterStatus.NextStepConnects(false, phase, record is not null) || record is null)
        {
            (App.Window as MainWindow)?.Navigate("adapter");
            return;
        }

        try
        {
            await adapters.ConnectAsync(record.Id);
        }
        catch (Exception error)
        {
            // The relationship carries the failure; the dot and this card repaint from it.
            AppServices.Diagnostics.Error("ui", ManagementErrorText.Summarize(error));
        }
    }

    private async Task OnWakeAsync()
    {
        nextButton.IsEnabled = false;
        playNotice.IsOpen = false;
        try
        {
            var status = await adapters.WakeConsoleAsync();

            // Unknown means the adapter could not tell us -- older firmware has no
            // `wake status`. Reporting that as failure would be a lie about the
            // console, so it is reported as exactly what it is.
            Notify(
                status.Result switch
                {
                    WakeResult.ConsoleAwake => "The console is awake.",
                    WakeResult.Advertised => "Wake sent. The console should wake shortly.",
                    WakeResult.NoIdentity =>
                        "The adapter has no saved console identity, so it cannot wake one. " +
                        "Connect it to the console once first.",
                    WakeResult.RadioBusy => "The adapter's radio was busy. Try again in a moment.",
                    WakeResult.Unknown =>
                        "Wake was sent, but this adapter's firmware cannot report the result.",
                    _ => "The adapter is still trying to wake the console.",
                },
                status.Result is WakeResult.ConsoleAwake or WakeResult.Advertised
                    ? InfoBarSeverity.Success
                    : InfoBarSeverity.Informational);
        }
        catch (Exception error)
        {
            Fail(error);
        }
        finally
        {
            RenderLink();
        }
    }

    /* ----------------------------------------------------- console buttons */

    private void AddConsoleButton(int column, string label, string glyph, ControllerButton button)
    {
        var action = Action(label, glyph, "Action");
        AutomationProperties.SetName(action, $"Hold {label} on the console");
        Grid.SetColumn(action, column);
        var byPointer = false;
        action.AddHandler(PointerPressedEvent, new PointerEventHandler((_, _) =>
        {
            byPointer = true;
            Hold(button, true);
        }), true);
        action.AddHandler(PointerReleasedEvent, new PointerEventHandler((_, _) => Hold(button, false)), true);
        action.AddHandler(PointerCaptureLostEvent, new PointerEventHandler((_, _) => Hold(button, false)), true);
        action.AddHandler(PointerCanceledEvent, new PointerEventHandler((_, _) => Hold(button, false)), true);

        // A key (Enter, Space) has no hold to pass through: it taps. The click
        // that ends a pointer hold is not a second press.
        action.Click += async (_, _) =>
        {
            if (byPointer)
            {
                byPointer = false;
                return;
            }

            await AppServices.TapConsoleButtonAsync(button);
        };
        consoleRow.Children.Add(action);
    }

    private void Hold(ControllerButton button, bool pressed)
    {
        if (pressed ? held.Add(button) : held.Remove(button))
        {
            AppServices.SetConsoleButton(button, pressed);
        }
    }

    /// <summary>Never leave a console button down: the page went away, or streaming stopped.</summary>
    private void ReleaseHeld()
    {
        foreach (var button in held.ToArray())
        {
            Hold(button, false);
        }
    }

    /* ---------------------------------------------------------- controller */

    private void RenderSources()
    {
        var view = ControllerSourceView.Of(gamepad.Sources, gamepad.SelectedSource, gamepad.UnreadableReason);
        var selected = gamepad.SelectedSource;
        var (name, tone, words) = ControllerCardStatus.Of(selected?.Name, selected?.CanDrive ?? false, view.Rows.Count);
        controllerName.Text = name;
        controllerState.Text = tone == StatusTone.Good ? string.Empty : words;
        controllerState.Visibility = tone == StatusTone.Good ? Visibility.Collapsed : Visibility.Visible;
        ToolTipService.SetToolTip(controllerName, string.IsNullOrWhiteSpace(view.Detail)
            ? view.Headline
            : $"{view.Headline}. {view.Detail}");
        ShowAdvice(view.Advice);

        // A choice opens as a menu, so the header stays one line.
        var choose = (view.CanChoose && view.Rows.Count > 1) || (selected is null && view.Rows.Count > 0);
        sourceButton.Content = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            Children =
            {
                new FontIcon { Glyph = choose ? "" : "", FontSize = 14 },
                new TextBlock { Text = choose ? "Change" : "Refresh", VerticalAlignment = VerticalAlignment.Center },
            },
        };
        sourceButton.Flyout = null;
        sourceButton.Click -= OnRefreshSources;
        if (choose)
        {
            var menu = new MenuFlyout { Placement = FlyoutPlacementMode.BottomEdgeAlignedRight };
            foreach (var row in view.Rows)
            {
                var item = new RadioMenuFlyoutItem { Text = row.Label, GroupName = "source", IsChecked = row.Id == view.SelectedId };
                var id = row.Id;
                item.Click += async (_, _) => await AppServices.SelectControllerSourceAsync(id);
                menu.Items.Add(item);
            }

            menu.Items.Add(new MenuFlyoutSeparator());
            var refresh = new MenuFlyoutItem { Text = "Refresh", Icon = new FontIcon { Glyph = "" } };
            refresh.Click += async (_, _) => await AppServices.RefreshControllerSourcesAsync();
            menu.Items.Add(refresh);
            sourceButton.Flyout = menu;
        }
        else
        {
            sourceButton.Click += OnRefreshSources;
        }
    }

    private async void OnRefreshSources(object sender, RoutedEventArgs e) =>
        await AppServices.RefreshControllerSourcesAsync();

    /// <summary>
    /// Connection advice describes a BETTER setup, not a fault, so it goes away
    /// after thirty seconds, and re-showing the same advice does not restart it.
    /// </summary>
    private void ShowAdvice(string? advice)
    {
        if (advice is null)
        {
            adviceTimer?.Stop();
            shownAdvice = null;
            sourceAdvice.IsOpen = false;
            return;
        }

        if (advice == shownAdvice)
        {
            return;
        }

        shownAdvice = advice;
        sourceAdvice.Message = advice;
        sourceAdvice.IsOpen = true;
        if (adviceTimer is null)
        {
            adviceTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(30) };
            adviceTimer.Tick += (_, _) =>
            {
                adviceTimer.Stop();
                sourceAdvice.IsOpen = false;
            };
        }

        adviceTimer.Stop();
        adviceTimer.Start();
    }

    /* ------------------------------------------------------------- headset */

    private async Task OnHeadsetToggleAsync()
    {
        playNotice.IsOpen = false;
        try
        {
            await companionAudio.ToggleAsync();
        }
        catch (Exception error)
        {
            Fail(error);
        }

        RenderHeadset();
    }

    private async Task OnMicToggleAsync()
    {
        try
        {
            await companionAudio.SetMicrophoneAsync(!companionAudio.MicrophoneEnabled);
        }
        catch (Exception error)
        {
            Fail(error);
        }

        RenderHeadset();
    }

    private void RenderHeadset()
    {
        var view = companionAudio.View.Value;
        var strip = HeadsetStrip.Of(view);
        headsetTitle.Text = strip.Title;
        liveBadge.Visibility = strip.Live ? Visibility.Visible : Visibility.Collapsed;
        headsetLine.Text = strip.Line;
        headsetLine.Visibility = strip.Line.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        headsetBusy.IsActive = strip.Busy;
        headsetBusy.Visibility = strip.Busy ? Visibility.Visible : Visibility.Collapsed;
        headsetButton.IsEnabled = strip.Enabled;
        AutomationProperties.SetName(headsetButton, $"Headset. {(strip.Live ? "Live" : "Off")}. {strip.Line}");
        headsetIcon.Style = Look(strip.Live ? "CircleLit" : "Circle");
        headsetTile.Style = Look(strip.Live ? "PlayTileLit" : "PlayTile");

        // A ToggleButton flips itself on click; the service's state is the truth.
        micToggle.IsChecked = companionAudio.MicrophoneEnabled;
        micToggle.IsEnabled = HeadsetLine.MicCanToggle(view);
        var mic = HeadsetLine.Mic(view, companionAudio.MicrophoneEnabled);
        AutomationProperties.SetName(micToggle, $"Microphone: {mic}");
        ToolTipService.SetToolTip(micToggle,
            $"{mic}. Send this PC's microphone to the console as the headset's (experimental). " +
            "Use headphones: console audio on speakers reaches the microphone.");

        if (HeadsetLine.Notice(view) is { } notice)
        {
            Notify(notice.Text, notice.Tone switch
            {
                NoticeTone.Informational => InfoBarSeverity.Informational,
                NoticeTone.Warning => InfoBarSeverity.Warning,
                _ => InfoBarSeverity.Error,
            });
        }
    }

    /* -------------------------------------------------------------- amiibo */

    private void RenderAmiibo()
    {
        var snapshot = adapters.Snapshot.Value;
        var view = AmiiboView.From(
            snapshot,
            adapters.Connection.Value.Connected,
            AppServices.AmiiboLibrary.Items.Value,
            AppServices.AmiiboKeys.Exists,
            selectedId: null);
        var tile = amiibo = PlayAmiiboTile.Of(
            view,
            AppServices.AmiiboCatalog.Find,
            snapshot.Input.Console,
            pro2: snapshot.Personality.Current == Personality.Pro2);

        wantedBadge.Visibility = tile.Wanted ? Visibility.Visible : Visibility.Collapsed;
        amiiboTile.Style = Look(tile.Wanted ? "PlayTileLit" : "PlayTile");
        amiiboTitle.Text = tile.Title;
        amiiboSubtitle.Text = tile.Subtitle;
        amiiboSubtitle.Visibility = tile.Subtitle.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        AutomationProperties.SetName(amiiboOpen, tile.AccessibleName);

        amiiboAction.Style = Look(tile.ActionPrimary ? "ActionAccent" : "Action");
        SetAction(amiiboAction, tile.ActionLabel, tile.Action == PlayAmiiboAction.Library ? "" : "");
        amiiboAction.IsEnabled = tile.ActionEnabled;

        var url = tile.Resident ? tile.ImageUrl : string.Empty;
        if (url == shownArt)
        {
            return;
        }

        // Decoration only: a BitmapImage stays blank on failure and the
        // placeholder shows through.
        shownArt = url;
        amiiboArt.Source = url.Length > 0 && AppServices.OnlineArtworkEnabled && Uri.TryCreate(url, UriKind.Absolute, out var uri) &&
            (uri.Scheme == Uri.UriSchemeHttps || uri.Scheme == Uri.UriSchemeHttp)
            ? new BitmapImage(uri) { DecodePixelWidth = 320 }
            : null;
        amiiboPlaceholder.Visibility = amiiboArt.Source is null ? Visibility.Visible : Visibility.Collapsed;
    }

    private async Task OnAmiiboActionAsync()
    {
        if (amiibo is not { } tile)
        {
            return;
        }

        if (tile.Action == PlayAmiiboAction.Library)
        {
            (App.Window as MainWindow)?.OpenAmiiboLibrary(tile.LibraryTarget);
            return;
        }

        try
        {
            await adapters.SetAmiiboPresentedAsync(tile.Action == PlayAmiiboAction.Present);
        }
        catch (Exception error)
        {
            Fail(error);
        }
    }

    /* -------------------------------------------------------------- pieces */

    private void Fail(Exception error)
    {
        var message = ManagementErrorText.Summarize(error);
        AppServices.Diagnostics.Error("ui", message);
        Notify(message, InfoBarSeverity.Error);
    }

    private void Notify(string message, InfoBarSeverity severity)
    {
        playNotice.Severity = severity;
        playNotice.Message = message;
        playNotice.IsOpen = true;
    }
}
