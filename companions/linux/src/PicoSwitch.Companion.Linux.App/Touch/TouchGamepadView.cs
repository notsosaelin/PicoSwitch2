using System.Diagnostics;
using System.Text;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Styling;
using Avalonia.Threading;
using PicoSwitch.Bridge.Touch;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Touch;

/// <summary>
/// The on-screen controller: a dedicated screen, the Linux port of the WinUI
/// surface (<c>companions/windows/src/PicoSwitch.Companion.App/Touch/TouchGamepadView.xaml.cs</c>),
/// which is itself shaped on the Android one.
///
/// ```text
/// Play   the screen is a CONTROLLER   controls, one menu button, a status pill
/// Edit   the screen is a CANVAS       controls plus one floating, dockable toolbar
/// ```
///
/// <see cref="MainWindow.ShowTouchGamepad"/> removes the shell (top bar, page,
/// dock) and this control paints an opaque ground of its own: the controller is
/// a mode that replaces the shell, not a page in it
/// (docs/architecture/companion-shell-layout.md).
///
/// What this file decides is where a finger is and which shared function to
/// call about it. Geometry comes from the resolved layout, every edit is a
/// <see cref="TouchLayoutEditor"/> call, every sentence is a
/// <see cref="TouchEditorView"/> property, and the toolbar's position comes from
/// <see cref="TouchToolbarLayout"/>. In play mode pointer events are reduced to
/// complete, stable-ID contact batches for the shared engine; the view knows
/// nothing about HOGP. The WinUI file carries the reasoning behind each rule;
/// the comments here record only where Linux differs.
/// </summary>
internal sealed class TouchGamepadView : UserControl
{
    private const string DockDocument = "touch-editor-dock";
    private const string AlignmentDocument = "touch-editor-align";

    private const float ScaleStep = 1.08f;
    private const float RotationStep = 5f;

    /// <summary>The layout's quiet centre band; the Android surface's <c>BANNER_BAND</c>.</summary>
    private const double BannerBand = 0.22;

    /// <summary>Profiles-dialog content buttons close the dialog with these codes (footer buttons are 0..2).</summary>
    private const int DuplicateCode = 10;
    private const int RenameCode = 11;
    private const int DeleteCode = 12;
    private const int ExportCode = 13;
    private const int ImportCode = 14;

    private static readonly FilePickerFileType LayoutFiles = new("Touch Gamepad layout")
    {
        Patterns = ["*.json"],
    };

    private readonly MainWindow shell;
    private readonly TouchGamepadService gamepad = AppServices.TouchGamepad;
    private readonly TouchControlRenderer renderer;
    private readonly DispatcherTimer gameplayTimer = new() { Interval = TimeSpan.FromMilliseconds(16) };
    private readonly Dictionary<long, TouchContact> gameplayContacts = [];

    private readonly Panel surfaceHost = new();
    private readonly Canvas surface = new() { Background = Brushes.Transparent, Focusable = true };
    private readonly Button menuButton;
    private readonly Border linkPill;
    private readonly TextBlock linkPillText = new() { FontSize = 12 };
    private readonly Border unusableNotice;
    private readonly TextBlock unusableText = Ui.Text();

    private readonly Canvas toolbarLayer = new() { IsVisible = false };
    private readonly Border toolbarBorder;
    private readonly StackPanel toolbarItems = new() { Orientation = Orientation.Horizontal, Spacing = 4 };
    private readonly Border toolbarHandle;
    private readonly Button addButton;
    private readonly MenuFlyout addMenu = new();
    private readonly Button duplicateButton;
    private readonly Button groupButton;
    private readonly Button deleteButton;
    private readonly Button undoButton;
    private readonly Button redoButton;
    private readonly Button saveButton;
    private readonly MenuItem selectionHeader = new() { IsEnabled = false };
    private readonly MenuItem[] selectionItems;
    private readonly MenuItem latchItem;
    private readonly MenuItem groupsItem;
    private readonly MenuItem gridItem;
    private readonly MenuItem snapItem;
    private readonly MenuItem resetLayoutItem;

    private readonly Panel menuLayer;
    private readonly TextBlock menuController = Ui.Text();
    private readonly TextBlock menuLinkDetail = Ui.Caption();
    private readonly Button menuEdit;
    private readonly Button menuProfiles;
    private readonly Button menuUseDefault;
    private readonly ComboBox menuPersonality = new() { HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly TextBlock menuPersonalityNote = Ui.Caption();

    private IReadOnlyList<Personality> personalityRows = TouchProfileSelector.GameplayPersonalities;
    private TouchSurfaceMode mode = TouchSurfaceMode.Play;
    private bool menuOpen;
    private bool personalityReady;
    private string lastGameplaySignature = string.Empty;
    private bool fullScreen;
    private bool editGroup = true;

    private TouchToolbarPlacement toolbar = TouchToolbarPlacement.Default;
    private TouchToolbarPlacement? toolbarDrag;
    private Point toolbarGrab;

    private long? dragPointer;
    private string? dragPrimary;
    private Point dragLast;
    private bool dragMoved;
    private TouchLayoutDocument? dragBaseline;

    /// <summary>The user asked to leave. The shell restores its chrome.</summary>
    public event Action? CloseRequested;

    /// <summary>The user asked to enter or leave full screen. The shell owns the window state.</summary>
    public event Action? FullScreenToggleRequested;

    public TouchGamepadView(MainWindow shell, bool fullScreen)
    {
        this.shell = shell;
        this.fullScreen = fullScreen;
        renderer = new TouchControlRenderer(surface);

        AutomationProperties.SetName(surface, "Controller layout");
        surface.PointerPressed += OnSurfacePressed;
        surface.PointerMoved += OnSurfaceMoved;
        surface.PointerReleased += OnSurfaceReleased;
        surface.PointerCaptureLost += OnSurfaceCaptureLost;
        surface.KeyDown += OnSurfaceKeyDown;

        // Play mode's only chrome, and the answer to "how do I get out of this".
        // Right-click on the surface and the context-menu key do the same.
        menuButton = Ui.IconButton(Icons.Menu, "Touch Gamepad menu", (_, _) => SetMenuOpen(true));
        menuButton.Opacity = 0.55;
        menuButton.Width = 40;
        menuButton.Height = 40;
        menuButton.HorizontalAlignment = HorizontalAlignment.Left;
        menuButton.VerticalAlignment = VerticalAlignment.Top;

        linkPill = new Border
        {
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Top,
            IsVisible = false,
            IsHitTestVisible = false,
            CornerRadius = new CornerRadius(16),
            Padding = new Thickness(16, 7),
            Background = new SolidColorBrush(Color.FromRgb(0x4A, 0x3A, 0x10)),
            Child = linkPillText,
        };
        linkPillText.Foreground = new SolidColorBrush(Color.FromRgb(0xF5, 0xE6, 0xC0));

        // Shown INSTEAD of the controller when the window cannot hold it: an
        // overlapping controller would send one control while showing another.
        unusableNotice = new Border
        {
            IsVisible = false,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            MaxWidth = 460,
            Padding = new Thickness(24),
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(1),
            Child = Ui.Stack(12, unusableText, Ui.Button("Leave the Touch Gamepad", (_, _) => CloseRequested?.Invoke())),
        }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);

        toolbarHandle = new Border
        {
            Width = 18,
            Background = Brushes.Transparent,
            Cursor = new Cursor(StandardCursorType.SizeAll),
            Child = Ui.Icon(Icons.Drag, 14),
        };
        AutomationProperties.SetName(toolbarHandle, "Move the toolbar");
        toolbarHandle.PointerPressed += OnToolbarPressed;
        toolbarHandle.PointerMoved += OnToolbarMoved;
        toolbarHandle.PointerReleased += (_, e) => EndToolbarDrag(e.Pointer);
        toolbarHandle.PointerCaptureLost += (_, e) => EndToolbarDrag(null);

        addButton = Ui.IconButton(Icons.Plus, "Add a control", (_, _) => { });
        addButton.Flyout = addMenu;
        duplicateButton = Ui.IconButton(Icons.Copy, "Duplicate", (_, _) => Duplicate());
        groupButton = Ui.IconButton(Icons.Group, "Group (Ctrl+G)", (_, _) => GroupOrUngroup());
        deleteButton = Ui.IconButton(Icons.Delete, "Remove (Delete)", (_, _) => Delete());
        undoButton = Ui.IconButton(Icons.Undo, "Undo (Ctrl+Z)", (_, _) => gamepad.Undo());
        redoButton = Ui.IconButton(Icons.Redo, "Redo (Ctrl+Y)", (_, _) => gamepad.Redo());
        saveButton = Ui.IconButton(Icons.Save, "Save (Ctrl+S)", (_, _) => gamepad.Save());
        saveButton.Classes.Add("accent");
        var doneButton = Ui.IconButton(Icons.Check, "Done", async (_, _) => await LeaveEditingAsync());

        // The toolbar's one menu, headed by the name of the control it acts on.
        selectionItems =
        [
            Item("Smaller", () => Resize(1f / ScaleStep)),
            Item("Larger", () => Resize(ScaleStep)),
            Item("Turn left", () => Rotate(-RotationStep)),
            Item("Turn right", () => Rotate(RotationStep)),
            Item("Reset orientation", ResetRotation),
            Item("Bring forward", () => EditSelection("Bring forward", TouchLayoutEditor.BringForward)),
            Item("Send backward", () => EditSelection("Send backward", TouchLayoutEditor.SendBackward)),
            Item("Reset control", ResetControl),
        ];
        latchItem = Toggle("Hold to latch", ToggleLatch);
        groupsItem = Toggle("Whole groups", () =>
        {
            editGroup = !editGroup;
            Render();
        });
        gridItem = Toggle("Grid", () => SetAlignment(gamepad.State.Value.Alignment with { Grid = !gamepad.State.Value.Alignment.Grid }));
        snapItem = Toggle("Snapping", () => SetAlignment(gamepad.State.Value.Alignment with { Snap = !gamepad.State.Value.Alignment.Snap }));
        resetLayoutItem = Item("Reset layout", ResetProfile);

        var moreMenu = new MenuFlyout();
        moreMenu.Items.Add(selectionHeader);
        foreach (var item in selectionItems[..^1])
        {
            moreMenu.Items.Add(item);
        }

        moreMenu.Items.Add(latchItem);
        moreMenu.Items.Add(selectionItems[^1]);
        moreMenu.Items.Add(new Separator());
        moreMenu.Items.Add(groupsItem);
        moreMenu.Items.Add(gridItem);
        moreMenu.Items.Add(snapItem);
        moreMenu.Items.Add(new Separator());
        moreMenu.Items.Add(Item("Layouts…", () => _ = OpenProfilesAsync()));
        moreMenu.Items.Add(resetLayoutItem);
        var moreButton = Ui.IconButton(Icons.More, "More", (_, _) => { });
        moreButton.Flyout = moreMenu;

        toolbarItems.Children.AddRange(
        [
            toolbarHandle, addButton, duplicateButton, groupButton, deleteButton,
            undoButton, redoButton, moreButton, saveButton, doneButton,
        ]);
        toolbarBorder = new Border
        {
            Padding = new Thickness(6),
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(1),
            Child = toolbarItems,
        }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);
        toolbarBorder.SizeChanged += (_, _) => PlaceChrome(gamepad.State.Value);
        toolbarLayer.Children.Add(toolbarBorder);

        // The interaction-safe rectangle is built from this panel's size, and
        // nothing takes space from it: the editor's chrome floats over it.
        surfaceHost.Children.AddRange([surface, menuButton, linkPill, unusableNotice, toolbarLayer]);
        surfaceHost.SizeChanged += (_, _) => Measure();

        menuEdit = Ui.Button("Edit", (_, _) => EnterEditing(), accent: true);
        menuEdit.HorizontalAlignment = HorizontalAlignment.Stretch;
        menuProfiles = Ui.Button("Layouts", (_, _) => _ = OpenProfilesAsync());
        menuProfiles.HorizontalAlignment = HorizontalAlignment.Stretch;
        menuUseDefault = Ui.Button("Use the default", (_, _) => ResetProfile());
        menuUseDefault.HorizontalAlignment = HorizontalAlignment.Stretch;
        var menuExit = Ui.Button("Exit Touch Gamepad", (_, _) => CloseRequested?.Invoke());
        menuExit.HorizontalAlignment = HorizontalAlignment.Stretch;
        menuExit.Margin = new Thickness(0, 8, 0, 0);
        AutomationProperties.SetName(menuPersonality, "Controller");
        menuPersonality.SelectionChanged += async (_, _) => await OnMenuPersonalityChangedAsync();

        var layoutRow = new Grid { ColumnDefinitions = new ColumnDefinitions("*,8,*") };
        layoutRow.Children.Add(menuEdit);
        Grid.SetColumn(menuProfiles, 2);
        layoutRow.Children.Add(menuProfiles);

        var title = Ui.Heading("Touch Gamepad");
        var close = Ui.IconButton(Icons.Close, "Close the Touch Gamepad menu", (_, _) => SetMenuOpen(false));
        close.HorizontalAlignment = HorizontalAlignment.Right;
        var header = new Grid { Children = { title, close } };
        title.VerticalAlignment = VerticalAlignment.Center;

        var card = new Border
        {
            MaxWidth = 460,
            Margin = new Thickness(24),
            Padding = new Thickness(24),
            CornerRadius = new CornerRadius(8),
            BorderThickness = new Thickness(1),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Child = new ScrollViewer
            {
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                Content = Ui.Stack(
                    12,
                    header,
                    Strong("Controller"),
                    menuController,
                    menuLinkDetail,
                    Strong("Layout", top: 8),
                    layoutRow,
                    menuUseDefault,
                    Strong("Controller", top: 8),
                    menuPersonality,
                    menuPersonalityNote,
                    menuExit),
            },
        }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);

        // A press on the scrim dismisses; a press inside the card is not one.
        menuLayer = new Panel { IsVisible = false, Children = { card } };
        menuLayer.Bind(Panel.BackgroundProperty, Tokens.Scrim);
        menuLayer.PointerPressed += (_, e) =>
        {
            if (ReferenceEquals(e.Source, menuLayer))
            {
                SetMenuOpen(false);
                e.Handled = true;
            }
        };
        menuLayer.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape)
            {
                SetMenuOpen(false);
                e.Handled = true;
            }
        };

        // THE RULE THE WINUI FILE EXISTS TO KEEP: the root carries an explicit,
        // opaque, near-black ground in both app themes. The controller's colours
        // are fixed artwork, and anything translucent here lets the shell show
        // through the controller.
        //
        // The chrome on it (menu button, toolbar, menu card) is scoped to the
        // DARK variant for the same reason: under the light app theme the menu
        // button's light-theme fill all but vanished on this ground.
        Content = new ThemeVariantScope
        {
            RequestedThemeVariant = ThemeVariant.Dark,
            Child = new Panel
            {
                Background = new SolidColorBrush(Color.FromRgb(0x0B, 0x0C, 0x10)),
                Children = { surfaceHost, menuLayer },
            },
        };

        toolbar = TouchToolbarEdges.FromKey(AppServices.Documents.Read(DockDocument)) is { } edge
            ? new TouchToolbarPlacement.Docked(edge)
            : TouchToolbarPlacement.Default;

        gamepad.SetAlignment(ReadAlignment());
        gameplayTimer.Tick += (_, _) =>
        {
            gamepad.TickGameplay(MonotonicNanos());
            RepaintGameplay();
        };

        AttachedToVisualTree += OnAttached;
        DetachedFromVisualTree += OnDetached;
    }

    private static TextBlock Strong(string text, double top = 0)
    {
        var block = Ui.Text(text);
        block.FontWeight = FontWeight.SemiBold;
        block.Margin = new Thickness(0, top, 0, 0);
        return block;
    }

    private static MenuItem Item(string header, Action run)
    {
        var item = new MenuItem { Header = header };
        item.Click += (_, _) => run();
        return item;
    }

    /// <summary>
    /// A check item whose state is set from the model on every render, never
    /// toggled by the menu itself, so a menu and the layout cannot disagree.
    /// </summary>
    private static MenuItem Toggle(string header, Action run)
    {
        var item = new MenuItem { Header = header, ToggleType = MenuItemToggleType.CheckBox, StaysOpenOnClick = false };
        item.Click += (_, _) => run();
        return item;
    }

    // ------------------------------------------------------------------- lifecycle

    private void OnAttached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        gamepad.State.Changed += OnStateChanged;
        AppServices.ControllerLink.View.Changed += OnStateChanged;
        AppServices.Adapters.Snapshot.Changed += OnSnapshotChanged;
        AppServices.Adapters.Registry.Changed += OnSnapshotChanged;
        AppServices.Adapters.Connection.Changed += OnStateChanged;

        ApplyPersonality();
        BuildAddMenu();
        Measure();
        Render();
        gamepad.ActivateGameplay();
        gameplayTimer.Start();

        // The canvas, not a button: the editor is keyboard-operable first.
        Dispatcher.UIThread.Post(() => surface.Focus());
    }

    private void OnDetached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        gamepad.State.Changed -= OnStateChanged;
        AppServices.ControllerLink.View.Changed -= OnStateChanged;
        AppServices.Adapters.Snapshot.Changed -= OnSnapshotChanged;
        AppServices.Adapters.Registry.Changed -= OnSnapshotChanged;
        AppServices.Adapters.Connection.Changed -= OnStateChanged;
        gameplayTimer.Stop();
        ReleaseGameplay(TouchReleaseReason.Disposed);
        gamepad.DeactivateGameplay();
        renderer.Clear();
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    private void OnSnapshotChanged() => AppServices.OnUiThread(() =>
    {
        ApplyPersonality();
        BuildAddMenu();
        Render();
    });

    /// <summary>The confirmed personality, never a guess: live, else the active adapter's last confirmed one.</summary>
    private void ApplyPersonality() => gamepad.SetPersonality(
        AppServices.Adapters.Snapshot.Value.Personality.Current,
        AppServices.Adapters.Registry.Value.Active?.LastPersonality);

    /// <summary>The shell reports what the window actually did; the surface re-resolves into it.</summary>
    public void SetFullScreen(bool value)
    {
        if (fullScreen == value)
        {
            return;
        }

        fullScreen = value;
        Measure();
        Render();
    }

    // ---------------------------------------------------------------------- region

    /// <summary>
    /// Build the interaction-safe rectangle and re-resolve into it.
    ///
    /// No edge-gesture inset on Linux. <see cref="TouchRegionBuilder.EdgeGestureInsetEpx"/>
    /// is the Windows shell's edge swipe (Task View, notifications); gamescope
    /// has no such strip, and a desktop compositor's edge gestures are its own
    /// configuration that the app cannot see. No caption reservation either:
    /// the window's decorations are outside this surface.
    /// </summary>
    private void Measure()
    {
        var size = surfaceHost.Bounds.Size;
        gamepad.SetRegion(TouchRegionBuilder.Build(
            size.Width,
            size.Height,
            TouchRegionBuilder.Insets(fullWindow: true, captionBarEpx: 0f, touchCapable: false)));
    }

    // ---------------------------------------------------------------------- render

    private void Render()
    {
        var state = gamepad.State.Value;
        var view = TouchEditorView.Of(
            state,
            controllerLinkAvailable: AppServices.ControllerLink.View.Value.Phase == ControllerLinkPhase.Streaming,
            mode);

        var editing = view.ShowEditorChrome;
        var unusable = state.Personality is null || state.Resolved.RegionTooSmall;

        unusableNotice.IsVisible = unusable;
        unusableText.Text = unusable ? (state.Warning ?? view.Status) : string.Empty;

        menuButton.IsVisible = !menuOpen;
        menuLayer.IsVisible = menuOpen;
        toolbarLayer.IsVisible = editing && !menuOpen;

        RenderMenu(view);
        RenderPill(view, state, unusable);

        if (editing)
        {
            RenderToolbar(state, view);
        }

        // Shown INSTEAD of the controller, as on Android (TouchGamepadScreen.kt
        // draws no canvas at all when unusable): a controller drawn behind the
        // notice still looks pressable, and at this size its controls overlap.
        if (unusable)
        {
            renderer.Clear();
            PlaceChrome(state);
            return;
        }

        renderer.Draw(state.Resolved, WithGameplay(new TouchRenderOptions
        {
            Selection = state.Selection,
            Invalid = state.Resolved.InvalidControlIds,
            Editing = editing,
            Grid = editing
                ? TouchEditorAlignment.GridLines(state.Resolved.Region, state.Alignment)
                : [],
            Guides = editing
                ? TouchEditorAlignment.MatchedGuides(
                    state.Resolved, state.Selection, state.Selection.FirstOrDefault(), state.Alignment)
                : [],
        }, state.Resolved, editing));

        lastGameplaySignature = string.Empty;
        PlaceChrome(state);
    }

    /// <summary>What the engine is doing right now, added to a set of render options. Nothing while editing.</summary>
    private TouchRenderOptions WithGameplay(TouchRenderOptions options, ResolvedTouchLayout resolved, bool editing)
    {
        if (editing)
        {
            return options;
        }

        var diagnostics = gamepad.Diagnostics();
        var pressed = new HashSet<string>(StringComparer.Ordinal);
        var offsets = new Dictionary<string, (double X, double Y)>(StringComparer.Ordinal);

        foreach (var control in resolved.Controls)
        {
            if (diagnostics.HeldControls.Contains(control.Id))
            {
                pressed.Add(control.Id);
            }

            if (diagnostics.StickVectors.TryGetValue(control.Id, out var vector))
            {
                offsets[control.Id] = (vector.X, vector.Y);
            }
        }

        return options with
        {
            Pressed = pressed,
            Latched = diagnostics.LatchedControls,
            Armed = diagnostics.ArmedControls,
            AnalogTriggers = diagnostics.AnalogTriggers,
            AnalogTriggerFills = diagnostics.AnalogTriggerFills,
            StickOffsets = offsets,
            Dpad = diagnostics.Dpad,
        };
    }

    /// <summary>Repaint the controls alone; skipped when nothing visible moved (the tick runs at 60 Hz).</summary>
    private void RepaintGameplay()
    {
        if (mode != TouchSurfaceMode.Play || menuOpen)
        {
            return;
        }

        var state = gamepad.State.Value;
        if (state.Personality is null || state.Resolved.RegionTooSmall)
        {
            return;
        }

        var options = WithGameplay(
            new TouchRenderOptions { Invalid = state.Resolved.InvalidControlIds },
            state.Resolved,
            editing: false);

        var signature = GameplaySignature(options);
        if (signature == lastGameplaySignature)
        {
            return;
        }

        lastGameplaySignature = signature;
        renderer.Draw(state.Resolved, options);
    }

    /// <summary>What the last repaint drew, quantised to what a repaint could show.</summary>
    private static string GameplaySignature(TouchRenderOptions options)
    {
        var builder = new StringBuilder();

        foreach (var id in options.Pressed.Order(StringComparer.Ordinal))
        {
            builder.Append('P').Append(id).Append(';');
        }

        foreach (var id in options.Latched.Order(StringComparer.Ordinal))
        {
            builder.Append('L').Append(id).Append(';');
        }

        foreach (var id in options.Armed.Order(StringComparer.Ordinal))
        {
            builder.Append('A').Append(id).Append(';');
        }

        foreach (var (id, level) in options.AnalogTriggers.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            builder.Append('T').Append(id).Append('=').Append((int)Math.Round(level * 100f)).Append(';');
        }

        foreach (var (id, offset) in options.StickOffsets.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            builder.Append('S').Append(id).Append('=')
                .Append((int)Math.Round(offset.X * 100d)).Append(',')
                .Append((int)Math.Round(offset.Y * 100d)).Append(';');
        }

        builder.Append('D')
            .Append(options.Dpad.Up ? '1' : '0')
            .Append(options.Dpad.Right ? '1' : '0')
            .Append(options.Dpad.Down ? '1' : '0')
            .Append(options.Dpad.Left ? '1' : '0');

        return builder.ToString();
    }

    private void RenderMenu(TouchEditorView view)
    {
        menuController.Text = view.Subtitle is { Length: > 0 } subtitle
            ? $"{view.Title} — {subtitle}"
            : view.Title;
        Ui.SetOptional(menuLinkDetail, view.LinkDetail);

        menuEdit.IsEnabled = view.Editable;
        menuProfiles.Content = view.ProfileName is { Length: > 0 } name ? name : "Layouts";
        menuUseDefault.IsEnabled = view.CanResetToDefault;
        RenderPersonalityChoice();
    }

    /// <summary>
    /// Which controller the adapter emulates, chosen from inside the surface:
    /// only personalities with an on-screen layout, and only with an adapter
    /// connected, because switching is a real adapter operation.
    /// </summary>
    private void RenderPersonalityChoice()
    {
        var current = AppServices.Adapters.Snapshot.Value.Personality.Current;
        var connected = AppServices.Adapters.Connection.Value.Connected;

        personalityReady = false;
        try
        {
            if (menuPersonality.ItemCount == 0)
            {
                personalityRows = TouchProfileSelector.GameplayPersonalities;
                menuPersonality.ItemsSource = personalityRows.Select(ControllerModeSection.Label).ToList();
            }

            menuPersonality.SelectedIndex = personalityRows.ToList().IndexOf(current);
        }
        finally
        {
            personalityReady = true;
        }

        menuPersonality.IsEnabled = connected;
        menuPersonalityNote.Text = connected
            ? "Switching re-enumerates USB and loads that controller's own layout."
            : "Connect an adapter to change this.";
    }

    private async Task OnMenuPersonalityChangedAsync()
    {
        // Guard the population: assigning the items raises this, and acting on
        // it would switch the adapter on merely opening the menu.
        if (!personalityReady || menuPersonality.SelectedIndex < 0 ||
            menuPersonality.SelectedIndex >= personalityRows.Count)
        {
            return;
        }

        var chosen = personalityRows[menuPersonality.SelectedIndex];
        if (AppServices.Adapters.Snapshot.Value.Personality.Current == chosen)
        {
            return;
        }

        // Release and neutralize BEFORE the switch: it re-enumerates USB and
        // replaces the layout under any contact still down.
        ReleaseGameplay(TouchReleaseReason.PersonalityChanged);
        await AppServices.Adapters.SetPersonalityAsync(chosen);
    }

    /// <summary>The status pill, in the layout's quiet band; not while the surface explains why there is no controller.</summary>
    private void RenderPill(TouchEditorView view, TouchGamepadState state, bool unusable)
    {
        var show = view.LinkNote is not null && !unusable && !menuOpen;
        linkPill.IsVisible = show;
        if (!show)
        {
            return;
        }

        linkPillText.Text = view.LinkNote;
        var region = state.Resolved.Region;
        linkPill.Margin = new Thickness(0, region.Top + (region.Height * BannerBand), 0, 0);
    }

    private void RenderToolbar(TouchGamepadState state, TouchEditorView view)
    {
        var selected = state.Selection.Count > 0;

        // Enablement changes; the slot count never does, so the bar never reflows under a finger.
        duplicateButton.IsEnabled = selected;
        deleteButton.IsEnabled = view.CanDelete;
        undoButton.IsEnabled = view.CanUndo;
        redoButton.IsEnabled = view.CanRedo;
        saveButton.IsEnabled = view.CanSave;

        var grouped = view.CanUngroup;
        groupButton.IsEnabled = grouped || view.CanGroup;
        var groupLabel = grouped ? "Ungroup (Ctrl+Shift+G)" : "Group (Ctrl+G)";
        ToolTip.SetTip(groupButton, groupLabel);
        AutomationProperties.SetName(groupButton, grouped ? "Ungroup" : "Group");

        selectionHeader.Header = view.SelectionSummary;
        foreach (var item in selectionItems)
        {
            item.IsEnabled = selected;
        }

        latchItem.IsEnabled = state.Selection.Count == 1;
        latchItem.IsChecked = state.Selection.Count == 1 &&
            state.Document.Instance(state.Selection.First())?.Latch == true;

        groupsItem.IsChecked = editGroup;
        gridItem.IsChecked = state.Alignment.Grid;
        snapItem.IsChecked = state.Alignment.Snap;
        resetLayoutItem.IsEnabled = view.CanResetToDefault;
    }

    // --------------------------------------------------------------- surface input

    private void OnSurfacePressed(object? sender, PointerPressedEventArgs e)
    {
        var point = e.GetCurrentPoint(surface);

        // A secondary click is the desktop's way to the menu, in either mode.
        if (point.Properties.IsRightButtonPressed)
        {
            SetMenuOpen(true);
            e.Handled = true;
            return;
        }

        if (mode == TouchSurfaceMode.Play && !menuOpen)
        {
            DispatchGameplay(e, TouchPhase.Down, removeAfterDispatch: false);
            e.Pointer.Capture(surface);
            e.Handled = true;
            return;
        }

        if (mode != TouchSurfaceMode.Edit || !gamepad.State.Value.Editable || menuOpen)
        {
            return;
        }

        surface.Focus();

        var position = point.Position;
        var hit = ControlAt(position);
        var additive = e.KeyModifiers.HasFlag(KeyModifiers.Control) || e.KeyModifiers.HasFlag(KeyModifiers.Shift);

        if (hit is null)
        {
            if (!additive)
            {
                gamepad.SetSelection(new HashSet<string>(StringComparer.Ordinal));
            }

            return;
        }

        var selection = new HashSet<string>(gamepad.State.Value.Selection, StringComparer.Ordinal);
        if (additive)
        {
            if (!selection.Add(hit.Id))
            {
                selection.Remove(hit.Id);
            }
        }
        else if (!selection.Contains(hit.Id))
        {
            selection = [hit.Id];
        }

        gamepad.SetSelection(selection);

        if (selection.Contains(hit.Id))
        {
            dragPointer = e.Pointer.Id;
            dragPrimary = hit.Id;
            dragLast = position;
            dragMoved = false;
            dragBaseline = gamepad.State.Value.Document;
            e.Pointer.Capture(surface);
        }

        e.Handled = true;
    }

    /// <summary>A drag, one incremental delta per event, which is what the snap's stickiness is built for.</summary>
    private void OnSurfaceMoved(object? sender, PointerEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play && gameplayContacts.ContainsKey(e.Pointer.Id))
        {
            DispatchGameplay(e, TouchPhase.Move, removeAfterDispatch: false);
            e.Handled = true;
            return;
        }

        if (dragPointer != e.Pointer.Id || dragPrimary is null)
        {
            return;
        }

        var state = gamepad.State.Value;
        var point = e.GetPosition(surface);
        var delta = new TouchEditorDelta((float)(point.X - dragLast.X), (float)(point.Y - dragLast.Y));
        dragLast = point;

        if (delta is { X: 0f, Y: 0f })
        {
            return;
        }

        var snapped = TouchEditorAlignment.Snap(state.Resolved, state.Selection, dragPrimary, delta, state.Alignment);
        gamepad.Preview(TouchLayoutEditor.Move(
            state.Document, state.Resolved, state.Selection, snapped.X, snapped.Y, editGroup));

        dragMoved = true;
        e.Handled = true;
    }

    private void OnSurfaceReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play && gameplayContacts.ContainsKey(e.Pointer.Id))
        {
            DispatchGameplay(e, TouchPhase.Up, removeAfterDispatch: true);
            e.Pointer.Capture(null);
            e.Handled = true;
            return;
        }

        if (dragPointer != e.Pointer.Id)
        {
            return;
        }

        e.Pointer.Capture(null);
        EndDrag(commit: true);
        e.Handled = true;
    }

    /// <summary>
    /// Capture taken away. In play every contribution is released (a contact
    /// the surface can no longer see must not stay held on the console); in the
    /// editor the gesture commits, since the document already carries it.
    /// </summary>
    private void OnSurfaceCaptureLost(object? sender, PointerCaptureLostEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play)
        {
            // Our own Capture(null) on release raises this too, after the contact
            // has already been removed; only a contact still held is a real loss.
            if (gameplayContacts.ContainsKey(e.Pointer.Id))
            {
                ReleaseGameplay(TouchReleaseReason.HostInactive);
            }

            return;
        }

        if (dragPointer == e.Pointer.Id)
        {
            EndDrag(commit: true);
        }
    }

    private void DispatchGameplay(PointerEventArgs e, TouchPhase phase, bool removeAfterDispatch)
    {
        var point = e.GetPosition(surface);
        var contact = new TouchContact(e.Pointer.Id, phase, (float)point.X, (float)point.Y, MonotonicNanos());
        gameplayContacts[e.Pointer.Id] = contact;
        gamepad.DispatchGameplayContacts(gameplayContacts.Values.ToArray());
        gamepad.TickGameplay(contact.TimeNanos);

        // Paint now, not on the tick: the press visual has to land with the thumb.
        RepaintGameplay();

        if (removeAfterDispatch)
        {
            gameplayContacts.Remove(e.Pointer.Id);
        }
        else
        {
            gameplayContacts[e.Pointer.Id] = contact with { Phase = TouchPhase.Move };
        }
    }

    private void ReleaseGameplay(TouchReleaseReason reason)
    {
        gameplayContacts.Clear();
        gamepad.ReleaseGameplay(reason);
        RepaintGameplay();
    }

    private static long MonotonicNanos() =>
        (long)(Stopwatch.GetTimestamp() * (1_000_000_000d / Stopwatch.Frequency));

    private void EndDrag(bool commit)
    {
        if (dragMoved)
        {
            if (commit)
            {
                gamepad.Commit("Move");
            }
            else if (dragBaseline is { } baseline)
            {
                gamepad.Preview(baseline);
            }
        }

        dragPointer = null;
        dragPrimary = null;
        dragBaseline = null;
        dragMoved = false;
    }

    private ResolvedTouchControl? ControlAt(Point point) => gamepad.State.Value.Resolved.Controls
        .Where(control => control.HitTest((float)point.X, (float)point.Y))
        .OrderByDescending(control => control.Spec.ZIndex)
        .ThenBy(control => control.NormalizedDistance((float)point.X, (float)point.Y))
        .FirstOrDefault();

    // -------------------------------------------------------------------- keyboard

    /// <summary>
    /// Avalonia's key names, in the vocabulary <see cref="TouchEditorKeys"/> was
    /// written against (WinUI's <c>VirtualKey</c> names). Only the names that
    /// differ are translated; letters, arrows, Tab, Delete, Back, Escape, F11,
    /// Add and Subtract are spelled the same.
    /// </summary>
    private static string KeyName(Key key) => key switch
    {
        Key.Apps => "Application",
        Key.OemPlus => "187",
        Key.OemMinus => "189",
        _ => key.ToString(),
    };

    private void OnSurfaceKeyDown(object? sender, KeyEventArgs e)
    {
        var state = gamepad.State.Value;
        var stroke = TouchEditorKeys.Resolve(
            KeyName(e.Key),
            e.KeyModifiers.HasFlag(KeyModifiers.Control),
            e.KeyModifiers.HasFlag(KeyModifiers.Shift));
        if (stroke.Command == TouchEditorCommand.None)
        {
            return;
        }

        switch (stroke.Command)
        {
            case TouchEditorCommand.ToggleFullscreen:
                FullScreenToggleRequested?.Invoke();
                e.Handled = true;
                return;

            case TouchEditorCommand.Menu:
                SetMenuOpen(!menuOpen);
                e.Handled = true;
                return;

            case TouchEditorCommand.Deselect:
                _ = HandleEscapeAsync();
                e.Handled = true;
                return;
        }

        if (mode != TouchSurfaceMode.Edit || !state.Editable || menuOpen)
        {
            return;
        }

        e.Handled = Apply(stroke, state);
    }

    /// <summary>
    /// Escape, in order: cancel a live drag, close the menu, leave the editor
    /// (asking if there is unsaved work), leave the Touch Gamepad. No rung quits
    /// the app. Full screen is not a rung: the app always runs full screen.
    /// </summary>
    private async Task HandleEscapeAsync()
    {
        if (dragPointer is not null || dragMoved)
        {
            EndDrag(commit: false);
            Render();
            return;
        }

        if (menuOpen)
        {
            SetMenuOpen(false);
            return;
        }

        if (mode == TouchSurfaceMode.Edit)
        {
            await LeaveEditingAsync();
            return;
        }

        CloseRequested?.Invoke();
    }

    private bool Apply(TouchEditorKeyStroke stroke, TouchGamepadState state)
    {
        var step = TouchEditorKeys.NudgePixels(state.Resolved, stroke.Coarse);

        switch (stroke.Command)
        {
            case TouchEditorCommand.NudgeLeft:
                return Nudge(-step, 0f);
            case TouchEditorCommand.NudgeRight:
                return Nudge(step, 0f);
            case TouchEditorCommand.NudgeUp:
                return Nudge(0f, -step);
            case TouchEditorCommand.NudgeDown:
                return Nudge(0f, step);

            case TouchEditorCommand.Grow:
                return Resize(ScaleStep);
            case TouchEditorCommand.Shrink:
                return Resize(1f / ScaleStep);

            case TouchEditorCommand.Undo:
                gamepad.Undo();
                return true;
            case TouchEditorCommand.Redo:
                gamepad.Redo();
                return true;
            case TouchEditorCommand.Save:
                gamepad.Save();
                return true;

            case TouchEditorCommand.Delete:
                Delete();
                return true;
            case TouchEditorCommand.Group:
                Group();
                return true;
            case TouchEditorCommand.Ungroup:
                Ungroup();
                return true;

            case TouchEditorCommand.SelectAll:
                gamepad.SetSelection(state.Resolved.Controls
                    .Select(control => control.Id)
                    .ToHashSet(StringComparer.Ordinal));
                return true;

            case TouchEditorCommand.NextControl:
                return Walk(state, forward: true);
            case TouchEditorCommand.PreviousControl:
                return Walk(state, forward: false);

            case TouchEditorCommand.ToggleGrid:
                SetAlignment(state.Alignment with { Grid = !state.Alignment.Grid });
                return true;
            case TouchEditorCommand.ToggleSnap:
                SetAlignment(state.Alignment with { Snap = !state.Alignment.Snap });
                return true;

            default:
                return false;
        }
    }

    private bool Nudge(float x, float y)
    {
        var state = gamepad.State.Value;
        if (state.Selection.Count == 0)
        {
            return false;
        }

        gamepad.Edit("Move", document => TouchLayoutEditor.Move(
            document, state.Resolved, state.Selection, x, y, editGroup));
        return true;
    }

    private bool Resize(float factor)
    {
        var state = gamepad.State.Value;
        if (state.Selection.Count == 0)
        {
            return false;
        }

        gamepad.Edit("Resize", document => TouchLayoutEditor.ScaleBy(
            document, state.Resolved, state.Selection, factor, editGroup));
        return true;
    }

    private bool Walk(TouchGamepadState state, bool forward)
    {
        var ids = state.Resolved.Controls
            .OrderBy(control => control.Spec.ZIndex)
            .Select(control => control.Id)
            .ToList();

        if (ids.Count == 0)
        {
            return false;
        }

        var current = state.Selection.Count == 1 ? ids.IndexOf(state.Selection.First()) : -1;
        var next = current < 0
            ? (forward ? 0 : ids.Count - 1)
            : (current + (forward ? 1 : -1) + ids.Count) % ids.Count;

        gamepad.SetSelection(new HashSet<string>([ids[next]], StringComparer.Ordinal));
        return true;
    }

    // ------------------------------------------------------------------ mode + menu

    private void SetMenuOpen(bool open)
    {
        if (menuOpen == open)
        {
            return;
        }

        menuOpen = open;
        if (open && mode == TouchSurfaceMode.Play)
        {
            ReleaseGameplay(TouchReleaseReason.HostInactive);
        }

        Render();
        if (open)
        {
            menuEdit.Focus();
        }
        else
        {
            surface.Focus();
        }
    }

    private void EnterEditing()
    {
        gameplayTimer.Stop();
        ReleaseGameplay(TouchReleaseReason.EditorEntered);
        gamepad.DeactivateGameplay();
        mode = TouchSurfaceMode.Edit;
        menuOpen = false;
        gamepad.SetSelection(new HashSet<string>(StringComparer.Ordinal));
        Render();
        surface.Focus();
    }

    /// <summary>Leave the editor for play, asking first if there is unsaved work.</summary>
    private async Task LeaveEditingAsync()
    {
        if (mode != TouchSurfaceMode.Edit)
        {
            return;
        }

        if (gamepad.State.Value.Dirty)
        {
            var choice = await shell.Dialogs.ShowAsync(
                "Leave without saving?",
                Ui.Text("This layout has changes that have not been saved."),
                ["Save", "Discard", "Keep editing"],
                primaryIndex: 0,
                defaultIndex: 0);

            switch (choice)
            {
                case 0:
                    gamepad.Save();
                    break;
                case 1:
                    gamepad.Discard();
                    break;
                default:
                    return;
            }
        }

        mode = TouchSurfaceMode.Play;
        gamepad.SetSelection(new HashSet<string>(StringComparer.Ordinal));
        gamepad.ActivateGameplay();
        gameplayTimer.Start();
        Render();
        surface.Focus();
    }

    // --------------------------------------------------------------------- chrome

    /// <summary>The floating chrome, inside the SAFE region (never the window edge), via <see cref="TouchToolbarLayout"/>.</summary>
    private void PlaceChrome(TouchGamepadState state)
    {
        var region = state.Resolved.Region;
        menuButton.Margin = new Thickness(region.Left + 12, region.Top + 12, 0, 0);

        if (!toolbarLayer.IsVisible)
        {
            return;
        }

        var placement = toolbarDrag ?? toolbar;
        toolbarItems.Orientation = placement is TouchToolbarPlacement.Docked docked && docked.Edge.Vertical()
            ? Orientation.Vertical
            : Orientation.Horizontal;
        toolbarHandle.Width = toolbarItems.Orientation == Orientation.Horizontal ? 18 : double.NaN;
        toolbarHandle.Height = toolbarItems.Orientation == Orientation.Vertical ? 18 : double.NaN;

        var size = toolbarBorder.Bounds.Size;
        var (x, y) = TouchToolbarLayout.TopLeft(placement, (float)size.Width, (float)size.Height, region);
        Canvas.SetLeft(toolbarBorder, x);
        Canvas.SetTop(toolbarBorder, y);
    }

    private void OnToolbarPressed(object? sender, PointerPressedEventArgs e)
    {
        toolbarGrab = e.GetPosition(toolbarBorder);
        toolbarDrag = toolbar;
        e.Pointer.Capture(toolbarHandle);
        e.Handled = true;
    }

    private void OnToolbarMoved(object? sender, PointerEventArgs e)
    {
        if (toolbarDrag is null)
        {
            return;
        }

        var point = e.GetPosition(surface);
        var size = toolbarBorder.Bounds.Size;
        toolbarDrag = TouchToolbarLayout.PlacementFor(
            (float)(point.X - toolbarGrab.X),
            (float)(point.Y - toolbarGrab.Y),
            (float)size.Width,
            (float)size.Height,
            gamepad.State.Value.Resolved.Region);

        PlaceChrome(gamepad.State.Value);
        e.Handled = true;
    }

    private void EndToolbarDrag(IPointer? pointer)
    {
        if (toolbarDrag is { } placement)
        {
            toolbar = placement;

            // Only a DOCK is remembered: which edge is a question about which
            // hand is free, asked once; a floating position fits one window shape.
            if (placement is TouchToolbarPlacement.Docked docked)
            {
                _ = AppServices.Documents.Write(DockDocument, docked.Edge.Key());
            }
        }

        toolbarDrag = null;
        pointer?.Capture(null);
        PlaceChrome(gamepad.State.Value);
    }

    // ------------------------------------------------------------------ toolbar acts

    private void Delete()
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Remove", document => TouchLayoutEditor.Delete(document, selection, editGroup));
    }

    private void Duplicate()
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Duplicate", document => TouchLayoutEditor.Duplicate(document, selection, editGroup));
    }

    private void GroupOrUngroup()
    {
        var state = gamepad.State.Value;
        var grouped = state.Selection
            .Select(state.Document.Instance)
            .Any(instance => instance?.GroupId is not null);

        if (grouped)
        {
            Ungroup();
        }
        else
        {
            Group();
        }
    }

    private void Group()
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Group", document => TouchLayoutEditor.Group(document, selection));
    }

    private void Ungroup()
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Ungroup", document => TouchLayoutEditor.Ungroup(document, selection));
    }

    private void Rotate(float degrees)
    {
        var state = gamepad.State.Value;
        if (state.Selection.Count == 0)
        {
            return;
        }

        gamepad.Edit("Rotate", document => TouchLayoutEditor.RotateBy(
            document, state.Resolved, state.Selection, degrees, editGroup));
    }

    private void ResetRotation() => EditSelection("Reset orientation", TouchLayoutEditor.ResetRotation);

    private void EditSelection(
        string label, Func<TouchLayoutDocument, IReadOnlySet<string>, bool, TouchLayoutDocument> edit)
    {
        var selection = gamepad.State.Value.Selection;
        if (selection.Count == 0)
        {
            return;
        }

        gamepad.Edit(label, document => edit(document, selection, editGroup));
    }

    private void ToggleLatch()
    {
        var state = gamepad.State.Value;
        if (state.Personality is not { } personality || state.Selection.Count == 0)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        var latch = !(state.Document.Instance(state.Selection.First())?.Latch == true);
        gamepad.Edit("Latch", document => TouchLayoutEditor.SetLatch(
            document, profile, state.Selection, latch, editGroup));
    }

    private void ResetControl()
    {
        var state = gamepad.State.Value;
        if (state.Personality is not { } personality || state.Selection.Count == 0)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        gamepad.Edit("Reset", document => TouchLayoutEditor.Reset(document, profile, state.Selection, editGroup));
    }

    private void SetAlignment(TouchAlignmentSettings alignment)
    {
        gamepad.SetAlignment(alignment);
        _ = AppServices.Documents.Write(
            AlignmentDocument,
            string.Join(',', new[] { alignment.Grid ? "grid" : null, alignment.Snap ? "snap" : null }
                .OfType<string>()));
    }

    private static TouchAlignmentSettings ReadAlignment()
    {
        var stored = AppServices.Documents.Read(AlignmentDocument) ?? string.Empty;
        return new TouchAlignmentSettings
        {
            Grid = stored.Contains("grid", StringComparison.Ordinal),
            Snap = stored.Contains("snap", StringComparison.Ordinal),
        };
    }

    // ------------------------------------------------------------------ add control

    /// <summary>The Add menu, from the personality's own catalog, grouped by presentation category.</summary>
    private void BuildAddMenu()
    {
        addMenu.Items.Clear();
        if (gamepad.State.Value.Personality is not { } personality)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        foreach (var group in profile.Catalog.GroupBy(entry => entry.Category).OrderBy(group => group.Key))
        {
            var submenu = new MenuItem { Header = group.Key.Title() };
            foreach (var entry in group)
            {
                var catalogId = entry.Id;
                submenu.Items.Add(Item(
                    TouchControlNaming.NameFor(
                        profile.Bindings.GetValueOrDefault(entry.Output), entry.Visual.Label, entry.Id),
                    () => AddControl(catalogId)));
            }

            addMenu.Items.Add(submenu);
        }
    }

    private void AddControl(string catalogId)
    {
        if (gamepad.State.Value.Personality is not { } personality)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        gamepad.Edit("Add", document => TouchLayoutEditor.Add(document, profile, catalogId, 0.5f, 0.5f));
    }

    // --------------------------------------------------------------------- profiles

    private void ResetProfile()
    {
        SetMenuOpen(false);
        gamepad.ResetToDefault();
    }

    /// <summary>
    /// The named-layout library, as one dialog: use, new, duplicate, rename,
    /// delete, export, import. Every button closes the dialog with its action,
    /// so the library is only mutated once the dialog is down.
    /// </summary>
    private async Task OpenProfilesAsync()
    {
        SetMenuOpen(false);

        var state = gamepad.State.Value;
        var profiles = state.Library.Profiles;
        var list = new ListBox
        {
            MaxHeight = 240,
            SelectionMode = SelectionMode.Single,
            ItemsSource = profiles.Select(profile => profile.Name).ToList(),
            SelectedIndex = profiles
                .Select((profile, index) => (profile, index))
                .FirstOrDefault(entry => entry.profile.Id == state.Library.SelectedProfileId)
                .index,
        };

        Button Action(string label, int code) => Ui.Button(label, (_, _) => shell.Dialogs.Close(code));

        var actions = new WrapPanel { Orientation = Orientation.Horizontal };
        foreach (var button in new[]
                 {
                     Action("Duplicate", DuplicateCode),
                     Action("Rename…", RenameCode),
                     Action("Delete", DeleteCode),
                     Action("Export…", ExportCode),
                     Action("Import…", ImportCode),
                 })
        {
            button.Margin = new Thickness(0, 0, 8, 8);
            actions.Children.Add(button);
        }

        var code = await shell.Dialogs.ShowAsync(
            "Layouts",
            Ui.Stack(12, list, actions),
            ["Use", "New…", "Close"],
            primaryIndex: 0,
            defaultIndex: 0);

        var chosen = list.SelectedIndex >= 0 && list.SelectedIndex < profiles.Count
            ? profiles[list.SelectedIndex]
            : null;

        switch (code)
        {
            case 0 when chosen is not null:
                gamepad.SelectProfile(chosen.Id);
                break;

            case 1:
                if (await AskForNameAsync("New layout", TouchProfileLibraryEditor.DefaultNewProfileName) is { } created)
                {
                    gamepad.CreateProfile(created);
                }

                break;

            case DuplicateCode when chosen is not null:
                gamepad.DuplicateProfile(chosen.Id);
                break;

            case RenameCode when chosen is not null:
                if (await AskForNameAsync("Rename layout", chosen.Name) is { } renamed)
                {
                    gamepad.RenameProfile(chosen.Id, renamed);
                }

                break;

            case DeleteCode when chosen is not null:
                gamepad.DeleteProfile(chosen.Id);
                break;

            case ExportCode when chosen is not null:
                await ExportAsync(chosen);
                break;

            case ImportCode:
                await ImportAsync();
                break;
        }
    }

    private IStorageProvider? Storage => TopLevel.GetTopLevel(this)?.StorageProvider;

    private async Task ExportAsync(TouchLayoutProfile profile)
    {
        if (gamepad.Export(profile.Id) is not { } encoded || Storage is not { } storage)
        {
            return;
        }

        var file = await storage.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = "Export layout",
            SuggestedFileName = profile.Name,
            DefaultExtension = "json",
            FileTypeChoices = [LayoutFiles],
        });
        if (file is null)
        {
            return;
        }

        await using var stream = await file.OpenWriteAsync();
        await using var writer = new StreamWriter(stream, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        await writer.WriteAsync(encoded);
    }

    private async Task ImportAsync()
    {
        if (Storage is not { } storage)
        {
            return;
        }

        var files = await storage.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Import layout",
            AllowMultiple = false,
            FileTypeFilter = [LayoutFiles, FilePickerFileTypes.All],
        });
        if (files.Count == 0)
        {
            return;
        }

        string text;
        await using (var stream = await files[0].OpenReadAsync())
        using (var reader = new StreamReader(stream))
        {
            text = await reader.ReadToEndAsync();
        }

        if (gamepad.Import(text) is { } refusal)
        {
            await shell.Dialogs.ShowAsync("That layout could not be imported", Ui.Text(refusal), ["Close"], primaryIndex: 0);
        }
    }

    private async Task<string?> AskForNameAsync(string title, string initial)
    {
        var input = new TextBox { Text = initial, SelectionStart = initial.Length, SelectionEnd = initial.Length };
        var choice = await shell.Dialogs.ShowAsync(title, input, ["OK", "Cancel"], primaryIndex: 0, defaultIndex: 0);
        return choice == 0 && input.Text?.Trim() is { Length: > 0 } name ? name : null;
    }
}
