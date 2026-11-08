using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Linux.Input;
using PicoSwitch.Companion.Platform.Input;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// Play: the home screen (docs/architecture/companion-shell-layout.md, "Play"),
/// laid out like a console home screen rather than a settings form. Android's
/// PlayScreen is the first implementation; this follows it tile for tile.
///
/// - Left column, four rows (owner, 2026-11-08: the controller card was mostly
///   empty space, and the console buttons REPLACED Touch Gamepad while
///   streaming, so there was no way into it once Controller Link started):
///   1. the HEADSET strip;
///   2. the CONTROLLER row: input → output at a glance (the controller this
///      computer reads → what the console sees, with its Player N), the
///      one-phrase state, and one quick-settings menu (which controller,
///      which emulated controller); a line only when something blocks;
///   3. the CONSOLE BUTTONS: Home, Capture, C, always in place, held while
///      streaming and disabled otherwise, so nothing moves when play starts;
///   4. the ACTIONS: Start / Stop (wider), Touch Gamepad, Wake or the
///      adapter's next step. Touch Gamepad is there in every state.
/// - Right column: the AMIIBO the adapter holds, full height, as art and a
///   name (<see cref="PlayAmiiboTile"/>); "GAME WANTS AN AMIIBO" while a game
///   has an NFC scan running.
/// - Every action is the one action button (52 tall, radius 14).
/// - The adapter's state is the navigation's dot; Play has no adapter card.
/// - Side by side when the columns fit; narrower or shorter windows stack and
///   scroll. The controller row takes the left column's spare height and
///   centres its content, so a tall window never shows an empty card.
///
/// Every word comes from a shared projection (<see cref="PlayHeadline"/>,
/// <see cref="ControllerCardStatus"/>, <see cref="HeadsetStrip"/>,
/// <see cref="PlayAmiiboTile"/>, <see cref="AdapterStatus"/>), so this page and
/// Windows' cannot disagree.
///
/// The desktop headset keeps its separate microphone toggle beside the strip:
/// here the microphone is a wish the adapter may refuse, unlike Android's
/// mute-only control (companion audio experiment record).
/// </summary>
internal sealed class PlayPage : CompanionPage
{
    /// <summary>The columns fit from here (Android's HUB_MIN_WIDTH / HUB_MIN_HEIGHT).</summary>
    private const double HubMinWidth = 600;
    private const double HubMinHeight = 280;
    private const double HeadsetStripHeight = 96;
    private const double StackedHeadsetHeight = 120;
    private const double StackedAmiiboHeight = 300;
    private const double TileRadius = 16;

    private readonly ControllerLinkService controllerLink = AppServices.ControllerLink;
    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly SdlGamepadInputSource gamepad = AppServices.PhysicalGamepad;
    private readonly CompanionAudioService companionAudio = AppServices.CompanionAudio;

    private readonly Grid hub = new() { ColumnDefinitions = new ColumnDefinitions("1.35*,*"), ColumnSpacing = 16 };
    private readonly Grid left = new() { RowDefinitions = new RowDefinitions($"{HeadsetStripHeight},1.4*,*,Auto"), RowSpacing = 16 };
    private readonly StackPanel stack = new() { Spacing = 16 };
    private readonly Border frame = new();
    private readonly ScrollViewer scroller = new() { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
    private bool? hubShown;

    // Controller row: input -> output.
    private readonly Border heroCard;
    private readonly TextBlock controllerName = new() { FontSize = 16, FontWeight = FontWeight.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock controllerState = new() { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock outputName = new() { FontSize = 16, FontWeight = FontWeight.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock outputState = new() { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Button quickButton;
    private readonly TextBlock headline = new() { FontSize = 24, FontWeight = FontWeight.Bold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock blocker = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly Notice sourceAdvice = new(dismissible: false);
    private readonly Notice playNotice = new();
    private readonly Grid consoleRow = EqualRow(3);
    private readonly Border consoleTile;
    private readonly Grid actionRow = new() { ColumnDefinitions = new ColumnDefinitions("*,*,*"), ColumnSpacing = 8 };
    private readonly List<Personality> personalities = [];
    private ControllerModeSection? controllerMode;
    private ControllerSourceView? sources;
    private readonly Button touchButton;
    private readonly Button nextButton;
    private readonly Button startStop;

    // Headset.
    private readonly Border headsetTile;
    private readonly Button headsetButton;
    private readonly Border headsetIcon;
    private readonly TextBlock headsetTitle = new() { FontSize = 20, FontWeight = FontWeight.Bold, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    private readonly Border liveBadge;
    private readonly TextBlock headsetLine = new() { FontSize = 13, TextWrapping = TextWrapping.Wrap, MaxLines = 2, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly ProgressBar headsetBusy = new() { IsIndeterminate = true, Width = 48, IsVisible = false, VerticalAlignment = VerticalAlignment.Center };
    private readonly ToggleButton micToggle;

    // Amiibo.
    private readonly Border amiiboTile;
    private readonly Border wantedBadge;
    private readonly Image amiiboArt = new() { Stretch = Stretch.Uniform };
    private readonly Border amiiboPlaceholder;
    private readonly TextBlock amiiboTitle = new() { FontSize = 20, FontWeight = FontWeight.Bold, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Center };
    private readonly TextBlock amiiboSubtitle = new() { FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis, HorizontalAlignment = HorizontalAlignment.Center };
    private readonly Button amiiboOpen;
    private readonly Button amiiboAction;
    private PlayAmiiboTile? amiibo;
    private string? shownArt;

    private DispatcherTimer? adviceTimer;
    private string? shownAdvice;
    private readonly HashSet<ControllerButton> held = [];

    public PlayPage(MainWindow shell)
        : base(shell)
    {
        /* ---- controller row: input -> output */
        var inputIcon = Circle(44, Ui.Icon(Icons.Gamepad, 22), Tokens.Subtle);
        var outputIcon = Circle(44, Ui.Icon(Icons.Gamepad, 22), Tokens.AccentSoft);
        ((PathIcon)outputIcon.Child!).Bind(PathIcon.ForegroundProperty, Tokens.AccentText);
        controllerState.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);
        outputState.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);
        var arrow = Ui.Icon(Icons.ChevronRight, 20);
        arrow.VerticalAlignment = VerticalAlignment.Center;
        arrow.Margin = new Thickness(4, 0);
        arrow.Bind(PathIcon.ForegroundProperty, Tokens.TextSecondary);
        var flow = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,*") };
        var input = End(inputIcon, controllerName, controllerState);
        var output = End(outputIcon, outputName, outputState);
        Grid.SetColumn(arrow, 1);
        Grid.SetColumn(output, 2);
        flow.Children.AddRange([input, arrow, output]);

        quickButton = new Button
        {
            Width = 44,
            Height = 44,
            MinWidth = 0,
            Padding = new Thickness(0),
            CornerRadius = new CornerRadius(22),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Content = Ui.Icon(Icons.Settings, 20),
        };
        quickButton.Classes.Add("lift");
        AutomationProperties.SetName(quickButton, "Controller quick settings");
        ToolTip.SetTip(quickButton, "Which controller, and what the console sees");
        quickButton.Flyout = new MenuFlyout { Placement = PlacementMode.BottomEdgeAlignedRight };
        quickButton.Click += (_, _) => BuildQuickMenu();

        var headlineRow = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(quickButton, Dock.Right);
        headlineRow.Children.AddRange([quickButton, headline]);
        blocker.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);

        var heroBody = new StackPanel
        {
            Spacing = 12,
            VerticalAlignment = VerticalAlignment.Center,
            Children = { headlineRow, flow, blocker, sourceAdvice, playNotice },
        };
        heroCard = Tile(heroBody);
        heroCard.Padding = new Thickness(20, 16);
        heroCard.Background = new LinearGradientBrush
        {
            StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
            EndPoint = new RelativePoint(1, 1, RelativeUnit.Relative),
        };

        /* ---- console buttons: always in place, live only while streaming */
        // Hold, not click: the console tells a tap from a hold (Capture records
        // on a long hold; Home opens the power menu). Pressed THROUGH Controller
        // Link, so they work only while it streams.
        AddConsoleButton(0, "Home", Icons.Home, ControllerButton.Home);
        AddConsoleButton(1, "Capture", Icons.Capture, ControllerButton.Capture);
        AddConsoleButton(2, "C", Icons.Gamepad, ControllerButton.C);
        consoleTile = Tile(consoleRow);
        consoleTile.Padding = new Thickness(12);

        /* ---- actions */
        startStop = Ui.ActionButton("Start playing", Icons.Play, ActionTone.Primary);
        startStop.Click += async (_, _) => await OnStartStopAsync();
        touchButton = Ui.ActionButton("Touch Gamepad", Icons.Gamepad);
        touchButton.Click += (_, _) => Shell.ShowTouchGamepad();
        nextButton = Ui.ActionButton("Wake", Icons.Power);
        nextButton.Click += async (_, _) => await OnNextAsync();
        Grid.SetColumn(touchButton, 1);
        Grid.SetColumn(nextButton, 2);
        actionRow.Children.AddRange([startStop, touchButton, nextButton]);

        /* ---- headset */
        headsetIcon = Circle(56, Ui.Icon(Icons.Headphones, 28), Tokens.Subtle);
        liveBadge = Badge("LIVE");
        liveBadge.Margin = new Thickness(8, 0, 0, 0);
        var titleRow = new DockPanel { LastChildFill = true, HorizontalAlignment = HorizontalAlignment.Left };
        DockPanel.SetDock(liveBadge, Dock.Right);
        titleRow.Children.AddRange([liveBadge, headsetTitle]);
        var headsetText = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(14, 0, 8, 0), Children = { titleRow, headsetLine } };
        var headsetContent = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(headsetIcon, Dock.Left);
        DockPanel.SetDock(headsetBusy, Dock.Right);
        headsetContent.Children.AddRange([headsetIcon, headsetBusy, headsetText]);
        headsetButton = TileButton(headsetContent);
        headsetButton.Click += async (_, _) => await OnHeadsetToggleAsync();

        micToggle = new ToggleButton
        {
            Width = Sizes.ActionHeight,
            Height = Sizes.ActionHeight,
            Padding = new Thickness(0),
            CornerRadius = new CornerRadius(Sizes.ActionRadius),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 22, 0),
            Content = Ui.Icon(Icons.Microphone, 20),
        };
        micToggle.Classes.Add("lift");
        micToggle.Click += async (_, _) => await OnMicToggleAsync();

        var headsetRow = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(micToggle, Dock.Right);
        headsetRow.Children.AddRange([micToggle, headsetButton]);
        headsetTile = Tile(headsetRow);

        /* ---- amiibo */
        wantedBadge = Badge("GAME WANTS AN AMIIBO");
        wantedBadge.HorizontalAlignment = HorizontalAlignment.Center;
        wantedBadge.Margin = new Thickness(0, 0, 0, 8);
        amiiboPlaceholder = Circle(96, Ui.Icon(Icons.Amiibo, 48), Tokens.Subtle);
        amiiboSubtitle.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);
        var art = new Panel { Children = { amiiboPlaceholder, amiiboArt } };
        var openContent = new DockPanel { LastChildFill = true };
        var names = new StackPanel { Spacing = 2, Margin = new Thickness(0, 8, 0, 0), Children = { amiiboTitle, amiiboSubtitle } };
        DockPanel.SetDock(wantedBadge, Dock.Top);
        DockPanel.SetDock(names, Dock.Bottom);
        openContent.Children.AddRange([wantedBadge, names, art]);
        amiiboOpen = TileButton(openContent);
        amiiboOpen.Padding = new Thickness(16, 16, 16, 8);
        amiiboOpen.Click += (_, _) => Shell.OpenAmiiboLibrary(amiibo?.LibraryTarget);

        amiiboAction = Ui.ActionButton("Library", Icons.Amiibo);
        amiiboAction.Margin = new Thickness(16, 4, 16, 16);
        amiiboAction.Click += async (_, _) => await OnAmiiboActionAsync();
        var amiiboBody = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(amiiboAction, Dock.Bottom);
        amiiboBody.Children.AddRange([amiiboAction, amiiboOpen]);
        amiiboTile = Tile(amiiboBody);

        frame.Child = hub;
        scroller.Content = frame;
        Content = scroller;
        SizeChanged += (_, e) => Arrange(e.NewSize);
        ActualThemeVariantChanged += (_, _) => RenderLink();
    }

    /// <summary>A controller or keyboard arriving on Play lands on Start / Stop, so A is always "play".</summary>
    public override InputElement? PreferredFocus => startStop;

    protected override void OnShown()
    {
        controllerLink.View.Changed += OnViewChanged;
        gamepad.SourcesChanged += OnSourcesChanged;
        adapters.Connection.Changed += OnAdapterChanged;
        adapters.Snapshot.Changed += OnAdapterChanged;
        adapters.Registry.Changed += OnAdapterChanged;
        adapters.Relationship.Changed += OnAdapterChanged;
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

    protected override void OnHidden()
    {
        controllerLink.View.Changed -= OnViewChanged;
        gamepad.SourcesChanged -= OnSourcesChanged;
        adapters.Connection.Changed -= OnAdapterChanged;
        adapters.Snapshot.Changed -= OnAdapterChanged;
        adapters.Registry.Changed -= OnAdapterChanged;
        adapters.Relationship.Changed -= OnAdapterChanged;
        companionAudio.View.Changed -= OnHeadsetChanged;
        AppServices.AmiiboLibrary.Items.Changed -= OnAdapterChanged;
        adviceTimer?.Stop();
        ReleaseHeld();
    }

    protected override void OnShortWindowChanged(bool isShort) => Arrange(Bounds.Size);

    private void OnViewChanged() => AppServices.OnUiThread(Render);

    private void OnSourcesChanged() => AppServices.OnUiThread(RenderSources);

    private void OnAdapterChanged() => AppServices.OnUiThread(Render);

    private void OnHeadsetChanged() => AppServices.OnUiThread(RenderHeadset);

    /* ------------------------------------------------------------- layout */

    /// <summary>Side by side when the columns fit (measured on the page, never a device), else stacked.</summary>
    private void Arrange(Size size)
    {
        if (size.Width <= 0)
        {
            return;
        }

        var gap = ShortWindow ? 8 : 16;
        hub.ColumnSpacing = gap;
        left.RowSpacing = gap;
        stack.Spacing = 12;

        var wantHub = size.Width >= HubMinWidth && size.Height >= HubMinHeight;
        frame.Padding = wantHub
            ? ShortWindow ? new Thickness(16, 4, 16, 8) : Sizes.PagePadding
            : new Thickness(16, 12, 16, 24);
        frame.MaxWidth = Sizes.WidePageMaxWidth;
        frame.HorizontalAlignment = HorizontalAlignment.Stretch;

        // The hub fills the window's height exactly, so it never scrolls.
        frame.Height = wantHub ? size.Height : double.NaN;
        scroller.VerticalScrollBarVisibility = wantHub ? ScrollBarVisibility.Disabled : ScrollBarVisibility.Auto;
        if (hubShown == wantHub)
        {
            return;
        }

        hubShown = wantHub;

        // The controller row never clips its words; the console tiles give way first.
        left.RowDefinitions[1].MinHeight = 180;
        left.RowDefinitions[2].MinHeight = 72;
        left.RowDefinitions[2].MaxHeight = 140;
        Detach(headsetTile);
        Detach(heroCard);
        Detach(consoleTile);
        Detach(actionRow);
        Detach(amiiboTile);
        hub.Children.Clear();
        left.Children.Clear();
        stack.Children.Clear();

        if (wantHub)
        {
            headsetTile.Height = double.NaN;
            heroCard.MinHeight = 0;
            amiiboTile.Height = double.NaN;
            Grid.SetRow(heroCard, 1);
            Grid.SetRow(consoleTile, 2);
            Grid.SetRow(actionRow, 3);
            left.Children.AddRange([headsetTile, heroCard, consoleTile, actionRow]);
            Grid.SetColumn(amiiboTile, 1);
            hub.Children.AddRange([left, amiiboTile]);
            frame.Child = hub;
        }
        else
        {
            headsetTile.Height = StackedHeadsetHeight;
            heroCard.MinHeight = 0;
            amiiboTile.Height = StackedAmiiboHeight;
            stack.Children.AddRange([actionRow, heroCard, consoleTile, headsetTile, amiiboTile]);
            frame.Child = stack;
        }
    }

    private static void Detach(Control control) => (control.Parent as Panel)?.Children.Remove(control);

    /* --------------------------------------------------------------- link */

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
        Ui.SetOptional(blocker, PlayHeadline.Blocker(link));
        ToolTip.SetTip(headline, $"{link.Headline}. {link.Explanation}");

        // The output end: what the console sees, and the player it gave us.
        var snapshot = adapters.Snapshot.Value;
        controllerMode = AdapterDashboard.Project(
            snapshot, adapters.Relationship.Value, adapters.Connection.Value, null, streaming).ControllerMode;
        outputName.Text = connected ? ControllerModeSection.Label(snapshot.Personality.Current) : "No adapter";
        outputState.Text = !connected
            ? AdapterStatus.Of(false, adapters.Relationship.Value.Phase).Words
            : ConsoleCues.PlayerLabel(connected, snapshot.Input.Console) ?? (streaming ? "On the console" : "To the console");

        // One primary action at a time, the same size as every other: Start
        // while it can start, Stop while it is running.
        var stop = link.CanStop;
        Ui.SetActionLabel(startStop, stop ? "Stop" : link.Busy ? "Connecting…" : "Start playing", stop ? Icons.Stop : Icons.Play);
        Ui.SetActionTone(startStop, stop ? ActionTone.Danger : ActionTone.Primary);
        startStop.IsEnabled = stop || link.CanStart;

        // Always in place, so starting never rearranges the page under a thumb.
        consoleRow.IsEnabled = streaming;
        ToolTip.SetTip(consoleTile, streaming ? null : "Home, Capture and C work while you play.");
        if (!streaming)
        {
            ReleaseHeld();
        }

        touchButton.IsEnabled = adapters.Registry.Value.Records.Count > 0;
        var phase = adapters.Relationship.Value.Phase;
        if (connected)
        {
            Ui.SetActionLabel(nextButton, "Wake", Icons.Power);
            nextButton.IsEnabled = adapters.Snapshot.Value.Capabilities.Wake != CapabilityState.Unsupported;
            AutomationProperties.SetName(nextButton, "Wake the console");
        }
        else
        {
            Ui.SetActionLabel(nextButton, AdapterStatus.NextStep(false, phase), Icons.Bluetooth);
            nextButton.IsEnabled = !AdapterStatus.InProgress(phase);
        }

        // The card glows the accent while it plays, quieter otherwise.
        if (heroCard.Background is LinearGradientBrush glow &&
            this.TryFindResource(streaming ? Tokens.AccentSoft : Tokens.Subtle, ActualThemeVariant, out var start) && start is ISolidColorBrush from &&
            this.TryFindResource(Tokens.Card, ActualThemeVariant, out var end) && end is ISolidColorBrush to)
        {
            glow.GradientStops = [new GradientStop(from.Color, 0), new GradientStop(to.Color, 1)];
        }
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
        var connected = adapters.Connection.Value.Connected;
        if (connected)
        {
            await OnWakeAsync();
            return;
        }

        var phase = adapters.Relationship.Value.Phase;
        var registry = adapters.Registry.Value;
        var record = registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();
        if (!AdapterStatus.NextStepConnects(false, phase, record is not null) || record is null)
        {
            Shell.Navigate("adapter");
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
        playNotice.Hide();
        try
        {
            var status = await adapters.WakeConsoleAsync();

            // Unknown means older firmware cannot report the result; reporting it
            // as failure would be a lie about the console.
            playNotice.Show(
                null,
                status.Result switch
                {
                    WakeResult.ConsoleAwake => "The console is awake.",
                    WakeResult.Advertised => "Wake sent. The console should wake shortly.",
                    WakeResult.NoIdentity =>
                        "The adapter has no saved console identity, so it cannot wake one. " +
                        "Connect it to the console once first.",
                    WakeResult.RadioBusy => "The adapter's radio was busy. Try again in a moment.",
                    WakeResult.Unknown => "Wake was sent, but this adapter's firmware cannot report the result.",
                    _ => "The adapter is still trying to wake the console.",
                },
                status.Result is WakeResult.ConsoleAwake or WakeResult.Advertised
                    ? NoticeSeverity.Success
                    : NoticeSeverity.Informational);
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

    private void AddConsoleButton(int column, string label, string icon, ControllerButton button)
    {
        // A console button is a tile, icon over label, sharing the column's
        // spare height with the controller row (a 52-tall strip left the
        // controller card mostly empty; owner, 2026-11-08).
        var action = Ui.ActionButton(label, icon);
        action.Height = double.NaN;
        action.MinHeight = Sizes.ActionHeight;
        action.VerticalAlignment = VerticalAlignment.Stretch;
        if (action.Content is Viewbox { Child: StackPanel face })
        {
            face.Orientation = Orientation.Vertical;
            face.Spacing = 4;
            face.Children[0].Width = 26;
            face.Children[0].Height = 26;
            ((Control)face.Children[1]).HorizontalAlignment = HorizontalAlignment.Center;
        }

        AutomationProperties.SetName(action, $"Hold {label} on the console");
        Grid.SetColumn(action, column);
        var byPointer = false;
        action.AddHandler(PointerPressedEvent, (_, e) =>
        {
            if (e.GetCurrentPoint(action).Properties.IsLeftButtonPressed)
            {
                byPointer = true;
                Hold(button, true);
            }
        }, Avalonia.Interactivity.RoutingStrategies.Tunnel, handledEventsToo: true);
        action.AddHandler(PointerReleasedEvent, (_, _) => Hold(button, false), Avalonia.Interactivity.RoutingStrategies.Bubble, handledEventsToo: true);
        action.AddHandler(PointerCaptureLostEvent, (_, _) => Hold(button, false), Avalonia.Interactivity.RoutingStrategies.Bubble, handledEventsToo: true);

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

    /* --------------------------------------------------------- controller */

    private void RenderSources()
    {
        var view = ControllerSourceView.Of(gamepad.Sources, gamepad.SelectedSource, gamepad.UnreadableReason);
        var selected = gamepad.SelectedSource;
        var (name, tone, words) = ControllerCardStatus.Of(selected?.Name, selected?.CanDrive ?? false, view.Rows.Count);
        controllerName.Text = name;
        Ui.SetOptional(controllerState, tone == StatusTone.Good ? null : words);

        // Motion is a property of the selected pad; say what the console will
        // get before the user starts.
        var motion = selected is null ? string.Empty : gamepad.Available ? " Motion: available." : " Motion: this controller has no gyro.";
        ToolTip.SetTip(controllerName, string.IsNullOrWhiteSpace(view.Detail)
            ? view.Headline + "." + motion
            : $"{view.Headline}. {view.Detail}{motion}");
        ShowAdvice(view.Advice);

        sources = view;
    }

    /// <summary>
    /// Quick settings, built when opened so it is never stale: which controller
    /// this computer reads (and Refresh), and which controller the console
    /// sees. The full forms stay on the Adapter page.
    /// </summary>
    private void BuildQuickMenu()
    {
        if (quickButton.Flyout is not MenuFlyout menu)
        {
            return;
        }

        menu.Items.Clear();
        menu.Items.Add(new MenuItem { Header = "Controller", IsEnabled = false });
        if (sources is { } view)
        {
            foreach (var row in view.Rows)
            {
                var item = new MenuItem
                {
                    Header = row.Label,
                    ToggleType = MenuItemToggleType.Radio,
                    IsChecked = row.Id == view.SelectedId,
                    IsEnabled = view.CanChoose || row.Id == view.SelectedId,
                };
                var id = row.Id;
                item.Click += async (_, _) => await AppServices.SelectControllerSourceAsync(id);
                menu.Items.Add(item);
            }
        }

        var refresh = new MenuItem { Header = "Look for controllers", Icon = Ui.Icon(Icons.Refresh, 16) };
        refresh.Click += async (_, _) => await AppServices.RefreshControllerSourcesAsync();
        menu.Items.Add(refresh);

        menu.Items.Add(new Separator());
        var mode = controllerMode;
        menu.Items.Add(new MenuItem
        {
            Header = mode is { Availability.Enabled: false, Availability.DisabledReason: { } why } ? $"The console sees — {why}" : "The console sees",
            IsEnabled = false,
        });
        personalities.Clear();
        personalities.AddRange(mode?.Available ?? []);
        foreach (var personality in personalities)
        {
            var item = new MenuItem
            {
                Header = ControllerModeSection.Label(personality),
                ToggleType = MenuItemToggleType.Radio,
                IsChecked = mode is not null && personality == mode.Current,
                IsEnabled = mode?.Availability.Enabled == true,
            };
            var chosen = personality;
            item.Click += async (_, _) => await OnSwitchPersonalityAsync(chosen);
            menu.Items.Add(item);
        }
    }

    /// <summary>The Adapter page's switch, from Play. The console briefly loses the controller.</summary>
    private async Task OnSwitchPersonalityAsync(Personality personality)
    {
        if (controllerMode is { } mode && personality == mode.Current)
        {
            return;
        }

        playNotice.Hide();
        try
        {
            var outcome = await adapters.SetPersonalityAsync(personality);
            if (!outcome.Unchanged && !outcome.Reenumerated)
            {
                playNotice.Show(
                    null,
                    $"The adapter switched to {ControllerModeSection.Label(outcome.Personality)}, " +
                    "but the console has not seen the change yet. Unplug the adapter and plug it back in.",
                    NoticeSeverity.Warning);
            }
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

    /// <summary>
    /// Advice describes a BETTER setup, not a fault, so it goes away after
    /// thirty seconds and re-showing the same advice does not restart it.
    /// </summary>
    private void ShowAdvice(string? advice)
    {
        if (advice is null)
        {
            adviceTimer?.Stop();
            shownAdvice = null;
            sourceAdvice.Hide();
            return;
        }

        if (advice == shownAdvice)
        {
            return;
        }

        shownAdvice = advice;
        sourceAdvice.Show(null, advice, NoticeSeverity.Informational);
        adviceTimer ??= new DispatcherTimer(TimeSpan.FromSeconds(30), DispatcherPriority.Normal, (_, _) =>
        {
            adviceTimer?.Stop();
            sourceAdvice.Hide();
        });
        adviceTimer.Stop();
        adviceTimer.Start();
    }

    /* ------------------------------------------------------------ headset */

    private async Task OnHeadsetToggleAsync()
    {
        playNotice.Hide();
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
        liveBadge.IsVisible = strip.Live;
        Ui.SetOptional(headsetLine, strip.Line);
        headsetBusy.IsVisible = strip.Busy;
        headsetButton.IsEnabled = strip.Enabled;
        AutomationProperties.SetName(headsetButton, $"Headset. {(strip.Live ? "Live" : "Off")}. {strip.Line}");
        ToolTip.SetTip(headsetButton, "Play console audio on this computer (experimental)");
        if (headsetIcon.Child is PathIcon glyph)
        {
            if (strip.Live)
            {
                headsetIcon.Bind(Border.BackgroundProperty, Tokens.Accent);
                glyph.Foreground = Brushes.White;
            }
            else
            {
                headsetIcon.Bind(Border.BackgroundProperty, Tokens.Subtle);
                glyph.ClearValue(PathIcon.ForegroundProperty);
            }
        }

        if (strip.Live)
        {
            headsetTile.Bind(Border.BackgroundProperty, Tokens.AccentSoft);
        }
        else
        {
            headsetTile.Bind(Border.BackgroundProperty, Tokens.Card);
        }

        // A ToggleButton flips itself on click; the service's state is the truth.
        micToggle.IsChecked = companionAudio.MicrophoneEnabled;
        micToggle.IsEnabled = HeadsetLine.MicCanToggle(view);
        var mic = HeadsetLine.Mic(view, companionAudio.MicrophoneEnabled);
        AutomationProperties.SetName(micToggle, $"Microphone: {mic}");
        ToolTip.SetTip(micToggle,
            $"{mic}. Send this computer's microphone to the console as the headset's (experimental). " +
            "Use headphones: console audio on speakers reaches the microphone.");

        if (HeadsetLine.Notice(view) is { } notice)
        {
            playNotice.Show(null, notice.Text, notice.Tone switch
            {
                NoticeTone.Informational => NoticeSeverity.Informational,
                NoticeTone.Warning => NoticeSeverity.Warning,
                _ => NoticeSeverity.Error,
            });
        }
    }

    /* ------------------------------------------------------------- amiibo */

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

        wantedBadge.IsVisible = tile.Wanted;
        amiiboTile.Bind(Border.BackgroundProperty, tile.Wanted ? Tokens.AccentSoft : Tokens.Card);
        amiiboTile.BorderBrush = null;
        if (tile.Wanted)
        {
            amiiboTile.Bind(Border.BorderBrushProperty, Tokens.Accent);
        }
        else
        {
            amiiboTile.Bind(Border.BorderBrushProperty, Tokens.CardStroke);
        }

        amiiboTitle.Text = tile.Title;
        Ui.SetOptional(amiiboSubtitle, tile.Subtitle);
        AutomationProperties.SetName(amiiboOpen, tile.AccessibleName);

        Ui.SetActionLabel(amiiboAction, tile.ActionLabel, tile.Action == PlayAmiiboAction.Library ? Icons.Folder : Icons.Amiibo);
        Ui.SetActionTone(amiiboAction, tile.ActionPrimary ? ActionTone.Primary : ActionTone.Tonal);
        amiiboAction.IsEnabled = tile.ActionEnabled;

        var url = tile.Resident ? tile.ImageUrl : string.Empty;
        if (url != shownArt)
        {
            shownArt = url;
            amiiboArt.Source = null;
            amiiboPlaceholder.IsVisible = true;
            if (url.Length > 0)
            {
                _ = ShowArtAsync(url);
            }
        }
    }

    private async Task ShowArtAsync(string url)
    {
        var bitmap = await AmiiboArtwork.LoadAsync(url);
        if (url != shownArt || bitmap is null)
        {
            return;
        }

        amiiboArt.Source = bitmap;
        amiiboPlaceholder.IsVisible = false;
    }

    private async Task OnAmiiboActionAsync()
    {
        if (amiibo is not { } tile)
        {
            return;
        }

        if (tile.Action == PlayAmiiboAction.Library)
        {
            Shell.OpenAmiiboLibrary(tile.LibraryTarget);
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

    /* ------------------------------------------------------------ pieces */

    private void Fail(Exception error)
    {
        var message = ManagementErrorText.Summarize(error);
        AppServices.Diagnostics.Error("ui", message);
        playNotice.Show(null, message, NoticeSeverity.Error);
    }

    /// <summary>One end of the input → output row: an icon, a name, a quiet line.</summary>
    private static Control End(Border icon, TextBlock name, TextBlock state)
    {
        var text = new StackPanel { Spacing = 0, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 4, 0), Children = { name, state } };
        var end = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(icon, Dock.Left);
        end.Children.AddRange([icon, text]);
        return end;
    }

    private static Grid EqualRow(int columns)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        for (var i = 0; i < columns; i++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Star));
        }

        return grid;
    }

    private static Border Tile(Control child) => new Border
    {
        CornerRadius = new CornerRadius(TileRadius),
        BorderThickness = new Thickness(1),
        ClipToBounds = true,
        Child = child,
    }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);

    /// <summary>A whole-tile button: transparent, so the tile's own colour shows, and lifted under a controller.</summary>
    private static Button TileButton(Control content)
    {
        var button = new Button
        {
            MinWidth = 0,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(2),
            BorderBrush = Brushes.Transparent,
            CornerRadius = new CornerRadius(TileRadius),
            Padding = new Thickness(20, 12),
            Content = content,
        };
        button.Classes.Add("lift");

        // The tile's own colour shows through in every state; Fluent would
        // paint a disabled or pressed button grey over it.
        foreach (var key in new[] { "ButtonBackgroundDisabled", "ButtonBackgroundPressed", "ButtonBorderBrushDisabled" })
        {
            button.Resources[key] = Brushes.Transparent;
        }

        return button;
    }

    private static Border Circle(double size, PathIcon glyph, string background)
    {
        glyph.HorizontalAlignment = HorizontalAlignment.Center;
        glyph.VerticalAlignment = VerticalAlignment.Center;
        return new Border
        {
            Width = size,
            Height = size,
            CornerRadius = new CornerRadius(size / 2),
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Center,
            Child = glyph,
        }.Bind(Border.BackgroundProperty, background);
    }

    /// <summary>News, not a label: the headset's LIVE and the Amiibo tile's GAME WANTS AN AMIIBO.</summary>
    private static Border Badge(string text) => new Border
    {
        CornerRadius = new CornerRadius(10),
        Padding = new Thickness(10, 3),
        VerticalAlignment = VerticalAlignment.Center,
        HorizontalAlignment = HorizontalAlignment.Left,
        IsVisible = false,
        Child = new TextBlock { Text = text, FontSize = 12, FontWeight = FontWeight.Bold, Foreground = Brushes.White, TextWrapping = TextWrapping.NoWrap },
    }.Bind(Border.BackgroundProperty, Tokens.Accent);
}
