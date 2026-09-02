using System.Diagnostics;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Input;
using PicoSwitch.Bridge.Touch;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using Windows.Devices.Input;
using Windows.Foundation;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.Core;

// `Windows` alone binds to PicoSwitch.Companion.Windows from inside this namespace, and
// Microsoft.UI.Input has a PointerDeviceType of its own, so the WinRT one is aliased
// rather than qualified.
using WinRtPointerDeviceType = global::Windows.Devices.Input.PointerDeviceType;

namespace PicoSwitch.Companion.App.Touch;

/// <summary>
/// The on-screen controller: a dedicated screen, in the shape the Android surface
/// established.
///
/// ## The structure, and why it is this structure
///
/// `CompanionApp.kt` returns early when the Touch Gamepad is active, so the scaffold, the
/// navigation rail and the content column are never composed at all; `TouchGamepadScreen`
/// then paints an opaque ground and owns the whole display. Windows does the same thing
/// in two halves: <see cref="MainWindow.ShowTouchGamepad"/> collapses the shell and drops
/// the window's Mica backdrop, and this control paints an opaque ground of its own.
///
/// ```text
/// Play   the screen is a CONTROLLER   controls, one menu button, a status pill
/// Edit   the screen is a CANVAS       controls plus one floating, dockable toolbar
/// ```
///
/// Play is the default, exactly as on Android. Nothing about the editor is visible until
/// the user asks for it: a permanent toolbar, inspector and status strip over a surface
/// whose whole job is to be pressed is a debugging overlay rather than a controller.
///
/// ## What this file is allowed to decide
///
/// Where a finger is, and which shared function to call about it. Geometry comes from the
/// resolved layout, every edit is a <see cref="TouchLayoutEditor"/> call, every sentence
/// is a <see cref="TouchEditorView"/> property, and the toolbar's position comes from
/// <see cref="TouchToolbarLayout"/>.
///
/// In play mode native pointer events are reduced to complete, stable-ID contact batches
/// and passed to the shared TouchGamepad engine. The view knows nothing about HOGP.
/// </summary>
public sealed partial class TouchGamepadView : UserControl
{
    private const string DockDocument = "touch-editor-dock";
    private const string AlignmentDocument = "touch-editor-align";

    /// <summary>One press of Smaller or Larger.</summary>
    private const float ScaleStep = 1.08f;

    /// <summary>One press of Turn left or Turn right, in degrees.</summary>
    private const float RotationStep = 5f;

    /// <summary>
    /// Where the status pill sits, as a fraction of the region's height.
    ///
    /// The layout's quiet centre band — the same band the Android surface puts its link
    /// banner in. The arrangement keeps that band free of controls, so a status there
    /// cannot shadow anything a thumb is reaching for, and it takes no height away from
    /// the controller.
    ///
    /// The value is the Android surface's own <c>BANNER_BAND</c>. It is not a taste: 0.12
    /// puts the pill across the shoulder and system row, which is exactly the mistake the
    /// band exists to avoid.
    /// </summary>
    private const double BannerBand = 0.22;

    private readonly TouchGamepadService gamepad = AppServices.TouchGamepad;
    private readonly TouchControlRenderer renderer;
    private readonly DispatcherTimer gameplayTimer = new() { Interval = TimeSpan.FromMilliseconds(16) };
    private readonly Dictionary<uint, TouchContact> gameplayContacts = [];

    private TouchSurfaceMode mode = TouchSurfaceMode.Play;
    private bool menuOpen;
    private bool fullScreen;
    private bool editGroup = true;

    private TouchToolbarPlacement toolbar = TouchToolbarPlacement.Default;
    private TouchToolbarPlacement? toolbarDrag;
    private Point toolbarGrab;

    private uint? dragPointer;
    private string? dragPrimary;
    private Point dragLast;
    private bool dragMoved;
    private TouchLayoutDocument? dragBaseline;

    /// <summary>The user asked to leave. The shell restores its chrome.</summary>
    public event Action? CloseRequested;

    /// <summary>The user asked to enter or leave full screen. The shell owns the presenter.</summary>
    public event Action? FullScreenToggleRequested;

    public TouchGamepadView()
    {
        InitializeComponent();
        renderer = new TouchControlRenderer(Surface);

        toolbar = TouchToolbarEdges.FromKey(AppServices.Documents.Read(DockDocument)) is { } edge
            ? new TouchToolbarPlacement.Docked(edge)
            : TouchToolbarPlacement.Default;

        gamepad.SetAlignment(ReadAlignment());
        gameplayTimer.Tick += (_, _) => gamepad.TickGameplay(MonotonicNanos());

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    // ------------------------------------------------------------------- lifecycle

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        gamepad.State.Changed += OnStateChanged;
        AppServices.ControllerLink.View.Changed += OnStateChanged;
        AppServices.Adapters.Snapshot.Changed += OnSnapshotChanged;
        AppServices.Adapters.Registry.Changed += OnSnapshotChanged;

        ApplyPersonality();
        BuildAddMenu();
        Measure();
        Render();
        gamepad.ActivateGameplay();
        gameplayTimer.Start();

        // Focus the canvas, not a button: a user who opened the controller wants to act on
        // the layout, and §26.5 runs the editor by keyboard before anything else.
        Surface.Focus(FocusState.Programmatic);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        gamepad.State.Changed -= OnStateChanged;
        AppServices.ControllerLink.View.Changed -= OnStateChanged;
        AppServices.Adapters.Snapshot.Changed -= OnSnapshotChanged;
        AppServices.Adapters.Registry.Changed -= OnSnapshotChanged;
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

    /// <summary>
    /// The confirmed personality, never a guess (§15.8) — live when an adapter is
    /// connected, otherwise the last one the active adapter was confirmed to be showing.
    ///
    /// The remembered value is what makes the editor useful with nothing attached; the
    /// surface says which of the two it is.
    /// </summary>
    private void ApplyPersonality() => gamepad.SetPersonality(
        AppServices.Adapters.Snapshot.Value.Personality.Current,
        AppServices.Adapters.Registry.Value.Active?.LastPersonality);

    /// <summary>
    /// The shell reports what the presenter actually did.
    ///
    /// Told rather than asked, because the window owns the presenter and the surface owns
    /// the layout: a surface that assumed the request had succeeded would resolve the
    /// controller into a rectangle the window never took.
    /// </summary>
    public void SetFullScreen(bool value)
    {
        if (fullScreen == value)
        {
            return;
        }

        fullScreen = value;

        // Re-resolve rather than rescale. Every control's position is a function of the
        // interaction-safe rectangle, and that is a different rectangle now.
        Measure();
        Render();
    }

    // ---------------------------------------------------------------------- region

    private void OnSurfaceSizeChanged(object sender, SizeChangedEventArgs e) => Measure();

    /// <summary>
    /// Build the interaction-safe rectangle and re-resolve into it.
    ///
    /// The insets are the platform's contribution (§15.5): an edge-gesture strip on a
    /// touch machine. No caption reservation, because the window's drag strip is a
    /// separate row ABOVE this surface rather than something drawn over it — so the
    /// controller never has to leave room for chrome it does not contain.
    /// </summary>
    private void Measure()
    {
        // Whether this machine can produce a touch contact at all. A pointer-only desktop
        // gives up no room to gesture strips it does not have.
        var touchCapable = PointerDevice.GetPointerDevices()
            .Any(device => device.PointerDeviceType == WinRtPointerDeviceType.Touch);

        gamepad.SetRegion(TouchRegionBuilder.Build(
            SurfaceHost.ActualWidth,
            SurfaceHost.ActualHeight,
            TouchRegionBuilder.Insets(fullWindow: true, captionBarEpx: 0f, touchCapable)));
    }

    // ---------------------------------------------------------------------- render

    private void Render()
    {
        var state = gamepad.State.Value;
        var view = TouchEditorView.Of(
            state,
            controllerLinkAvailable: AppServices.ControllerLink.View.Value.Phase ==
                ControllerLinkPhase.Connected,
            mode);

        var editing = view.ShowEditorChrome;

        // The editor stays open for an audit failure — moving a control is exactly how
        // that gets fixed — but not for a window that is simply too small, where no edit
        // can help, nor when there is no controller to draw at all.
        var unusable = state.Personality is null || state.Resolved.RegionTooSmall;

        UnusableNotice.Visibility = unusable ? Visibility.Visible : Visibility.Collapsed;
        UnusableText.Text = unusable ? (state.Warning ?? view.Status) : string.Empty;

        MenuButton.Visibility = menuOpen ? Visibility.Collapsed : Visibility.Visible;
        MenuLayer.Visibility = menuOpen ? Visibility.Visible : Visibility.Collapsed;
        ToolbarLayer.Visibility = editing && !menuOpen
            ? Visibility.Visible
            : Visibility.Collapsed;

        RenderMenu(view);
        RenderPill(view, state, unusable);

        if (editing)
        {
            RenderToolbar(state, view);
        }

        renderer.Draw(state.Resolved, new TouchRenderOptions
        {
            Selection = state.Selection,
            Invalid = state.Resolved.InvalidControlIds,
            Editing = editing,
            Grid = editing
                ? TouchEditorAlignment.GridLines(state.Resolved.Region, state.Alignment)
                : [],
            Guides = editing
                ? TouchEditorAlignment.MatchedGuides(
                    state.Resolved, state.Selection, state.Selection.FirstOrDefault(),
                    state.Alignment)
                : [],
        });

        PlaceChrome(state);
    }

    private void RenderMenu(TouchEditorView view)
    {
        MenuController.Text = view.Subtitle is { Length: > 0 } subtitle
            ? $"{view.Title} — {subtitle}"
            : view.Title;
        MenuLinkDetail.Text = view.LinkDetail ?? string.Empty;
        MenuLinkDetail.Visibility = view.LinkDetail is null
            ? Visibility.Collapsed
            : Visibility.Visible;

        MenuEdit.IsEnabled = view.Editable;
        MenuProfiles.Content = view.ProfileName is { Length: > 0 } name ? name : "Layouts";
        MenuUseDefault.IsEnabled = view.CanResetToDefault;
        MenuFullScreen.Content = fullScreen ? "Leave full screen (F11)" : "Full screen (F11)";
    }

    /// <summary>
    /// The status pill, in the layout's quiet band.
    ///
    /// Not while the surface is explaining why there is no controller: that explanation
    /// occupies the same band, and two overlapping messages read as neither.
    /// </summary>
    private void RenderPill(TouchEditorView view, TouchGamepadState state, bool unusable)
    {
        var show = view.LinkNote is not null && !unusable && !menuOpen;
        LinkPill.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (!show)
        {
            return;
        }

        LinkPillText.Text = view.LinkNote;
        var region = state.Resolved.Region;
        LinkPill.Margin = new Thickness(0, region.Top + (region.Height * BannerBand), 0, 0);
    }

    private void RenderToolbar(TouchGamepadState state, TouchEditorView view)
    {
        var selected = state.Selection.Count > 0;

        // Enablement changes; the slot count never does. A selection that added or removed
        // buttons would reflow the bar under the user's finger.
        DuplicateButton.IsEnabled = selected;
        DeleteButton.IsEnabled = view.CanDelete;
        UndoButton.IsEnabled = view.CanUndo;
        RedoButton.IsEnabled = view.CanRedo;
        SaveButton.IsEnabled = view.CanSave;

        var grouped = view.CanUngroup;
        GroupButton.IsEnabled = grouped || view.CanGroup;
        ToolTipService.SetToolTip(GroupButton, grouped ? "Ungroup" : "Group");
        AutomationProperties.SetName(GroupButton, grouped ? "Ungroup" : "Group");

        SelectionHeader.Text = view.SelectionSummary;
        foreach (var item in new[]
                 {
                     SmallerItem, LargerItem, TurnLeftItem, TurnRightItem, ResetRotationItem,
                     ForwardItem, BackwardItem, ResetControlItem,
                 })
        {
            item.IsEnabled = selected;
        }

        LatchItem.IsEnabled = state.Selection.Count == 1;
        LatchItem.IsChecked = state.Selection.Count == 1 &&
            state.Document.Instance(state.Selection.First())?.Latch == true;

        GroupsItem.IsChecked = editGroup;
        GridItem.IsChecked = state.Alignment.Grid;
        SnapItem.IsChecked = state.Alignment.Snap;
        ResetLayoutItem.IsEnabled = view.CanResetToDefault;
    }

    // --------------------------------------------------------------- surface input

    private void OnSurfacePressed(object sender, PointerRoutedEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play && !menuOpen)
        {
            DispatchGameplay(e, TouchPhase.Down, removeAfterDispatch: false);
            Surface.CapturePointer(e.Pointer);
            e.Handled = true;
            return;
        }

        if (mode != TouchSurfaceMode.Edit || !gamepad.State.Value.Editable || menuOpen)
        {
            return;
        }

        Surface.Focus(FocusState.Pointer);

        var point = e.GetCurrentPoint(Surface).Position;
        var hit = ControlAt(point);
        var modifiers = e.KeyModifiers;
        var additive = modifiers.HasFlag(VirtualKeyModifiers.Control) ||
                       modifiers.HasFlag(VirtualKeyModifiers.Shift);

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
            dragPointer = e.Pointer.PointerId;
            dragPrimary = hit.Id;
            dragLast = point;
            dragMoved = false;
            dragBaseline = gamepad.State.Value.Document;
            Surface.CapturePointer(e.Pointer);
        }

        e.Handled = true;
    }

    /// <summary>
    /// A drag, one incremental delta per event.
    ///
    /// Incremental rather than "total displacement since the press" because that is what
    /// <see cref="TouchEditorAlignment.Snap"/> is built for: it gives a snap its sticky
    /// behaviour, where small movements keep resolving back onto a guide until one is
    /// large enough to escape. The pointer's own position is what advances, so the
    /// stickiness is absorbed rather than accumulating into a drift.
    /// </summary>
    private void OnSurfaceMoved(object sender, PointerRoutedEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play && gameplayContacts.ContainsKey(e.Pointer.PointerId))
        {
            DispatchGameplay(e, TouchPhase.Move, removeAfterDispatch: false);
            e.Handled = true;
            return;
        }

        if (dragPointer != e.Pointer.PointerId || dragPrimary is null)
        {
            return;
        }

        var state = gamepad.State.Value;
        var point = e.GetCurrentPoint(Surface).Position;
        var delta = new TouchEditorDelta(
            (float)(point.X - dragLast.X), (float)(point.Y - dragLast.Y));
        dragLast = point;

        if (delta is { X: 0f, Y: 0f })
        {
            return;
        }

        var snapped = TouchEditorAlignment.Snap(
            state.Resolved, state.Selection, dragPrimary, delta, state.Alignment);

        gamepad.Preview(TouchLayoutEditor.Move(
            state.Document, state.Resolved, state.Selection,
            snapped.X, snapped.Y, editGroup));

        dragMoved = true;
        e.Handled = true;
    }

    private void OnSurfaceReleased(object sender, PointerRoutedEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play && gameplayContacts.ContainsKey(e.Pointer.PointerId))
        {
            DispatchGameplay(e, TouchPhase.Up, removeAfterDispatch: true);
            Surface.ReleasePointerCapture(e.Pointer);
            e.Handled = true;
            return;
        }

        if (dragPointer != e.Pointer.PointerId)
        {
            return;
        }

        Surface.ReleasePointerCapture(e.Pointer);
        EndDrag(commit: true);
        e.Handled = true;
    }

    /// <summary>
    /// Capture taken away mid-drag.
    ///
    /// The gesture is still committed: the document already carries every applied delta,
    /// and throwing it away would lose a move the user watched happen. What must not
    /// happen is a half-drag left outside the undo history, which is the case this closes.
    /// </summary>
    private void OnSurfaceCaptureLost(object sender, PointerRoutedEventArgs e)
    {
        if (mode == TouchSurfaceMode.Play)
        {
            ReleaseGameplay(TouchReleaseReason.HostInactive);
            return;
        }

        EndDrag(commit: true);
    }

    private void DispatchGameplay(
        PointerRoutedEventArgs e,
        TouchPhase phase,
        bool removeAfterDispatch)
    {
        var point = e.GetCurrentPoint(Surface).Position;
        var contact = new TouchContact(
            e.Pointer.PointerId,
            phase,
            (float)point.X,
            (float)point.Y,
            MonotonicNanos());
        gameplayContacts[e.Pointer.PointerId] = contact;
        gamepad.DispatchGameplayContacts(gameplayContacts.Values.ToArray());
        gamepad.TickGameplay(contact.TimeNanos);

        if (removeAfterDispatch)
        {
            gameplayContacts.Remove(e.Pointer.PointerId);
        }
        else
        {
            gameplayContacts[e.Pointer.PointerId] = contact with { Phase = TouchPhase.Move };
        }
    }

    private void ReleaseGameplay(TouchReleaseReason reason)
    {
        gameplayContacts.Clear();
        gamepad.ReleaseGameplay(reason);
    }

    private static long MonotonicNanos() =>
        (long)(Stopwatch.GetTimestamp() * (1_000_000_000d / Stopwatch.Frequency));

    private void OnSurfaceRightTapped(object sender, RightTappedRoutedEventArgs e)
    {
        SetMenuOpen(true);
        e.Handled = true;
    }

    /// <summary>Finish a drag. <paramref name="commit"/> false puts the layout back.</summary>
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

    private void OnSurfaceKeyDown(object sender, KeyRoutedEventArgs e)
    {
        var state = gamepad.State.Value;
        var control = InputKeyboardSource
            .GetKeyStateForCurrentThread(VirtualKey.Control).HasFlag(CoreVirtualKeyStates.Down);
        var shift = InputKeyboardSource
            .GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(CoreVirtualKeyStates.Down);

        var stroke = TouchEditorKeys.Resolve(e.Key.ToString(), control, shift);
        if (stroke.Command == TouchEditorCommand.None)
        {
            return;
        }

        // The three surface-level keys work in either mode.
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
                HandleEscape();
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
    /// Escape, in the order §8 prescribes.
    ///
    /// ```text
    /// a live drag     cancel it, leaving the layout as it was
    /// the menu open   close the menu
    /// edit mode       leave the editor, asking first if there is unsaved work
    /// full screen     leave full screen
    /// otherwise       leave the Touch Gamepad
    /// ```
    ///
    /// Android's Back opens the menu from gameplay rather than leaving, because on a phone
    /// that menu is the only way out. Windows has Escape as the desktop idiom for "back
    /// out of this" and a visible menu button, so it walks the ladder above instead — which
    /// is what §8 asks for. No rung terminates the application, and none can leave the user
    /// stuck in full screen.
    /// </summary>
    private async void HandleEscape()
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

        if (fullScreen)
        {
            FullScreenToggleRequested?.Invoke();
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

    /// <summary>Step the selection through the layout in draw order.</summary>
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
        if (!open)
        {
            Surface.Focus(FocusState.Programmatic);
        }
    }

    private void OnOpenMenu(object sender, RoutedEventArgs e) => SetMenuOpen(true);

    private void OnCloseMenu(object sender, RoutedEventArgs e) => SetMenuOpen(false);

    /// <summary>A tap on the scrim dismisses; a tap inside the card is not a tap on it.</summary>
    private void OnDismissMenu(object sender, TappedRoutedEventArgs e)
    {
        SetMenuOpen(false);
        e.Handled = true;
    }

    private void OnSwallowTap(object sender, TappedRoutedEventArgs e) => e.Handled = true;

    private void OnEnterEditing(object sender, RoutedEventArgs e)
    {
        gameplayTimer.Stop();
        ReleaseGameplay(TouchReleaseReason.EditorEntered);
        gamepad.DeactivateGameplay();
        mode = TouchSurfaceMode.Edit;
        menuOpen = false;
        gamepad.SetSelection(new HashSet<string>(StringComparer.Ordinal));
        Render();
        Surface.Focus(FocusState.Programmatic);
    }

    private async void OnLeaveEditing(object sender, RoutedEventArgs e) =>
        await LeaveEditingAsync();

    /// <summary>
    /// Leave the editor for play mode, asking first if there is unsaved work.
    ///
    /// Android does the same: backing out of a dirty editor raises a confirmation rather
    /// than discarding, because an editor that silently drops an edit is an editor nobody
    /// trusts with the next one.
    /// </summary>
    private async Task LeaveEditingAsync()
    {
        if (mode != TouchSurfaceMode.Edit)
        {
            return;
        }

        if (gamepad.State.Value.Dirty)
        {
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = "Leave without saving?",
                Content = new TextBlock
                {
                    Text = "This layout has changes that have not been saved.",
                    TextWrapping = TextWrapping.Wrap,
                },
                PrimaryButtonText = "Save",
                SecondaryButtonText = "Discard",
                CloseButtonText = "Keep editing",
                DefaultButton = ContentDialogButton.Primary,
            };

            switch (await dialog.ShowAsync())
            {
                case ContentDialogResult.Primary:
                    gamepad.Save();
                    break;
                case ContentDialogResult.Secondary:
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
        Surface.Focus(FocusState.Programmatic);
    }

    private void OnToggleFullScreen(object sender, RoutedEventArgs e)
    {
        SetMenuOpen(false);
        FullScreenToggleRequested?.Invoke();
    }

    private void OnClose(object sender, RoutedEventArgs e) => CloseRequested?.Invoke();

    // --------------------------------------------------------------------- chrome

    private void OnToolbarSizeChanged(object sender, SizeChangedEventArgs e) =>
        PlaceChrome(gamepad.State.Value);

    /// <summary>
    /// Position the floating chrome inside the interaction-safe region.
    ///
    /// Through <see cref="TouchToolbarLayout"/>, never by markup alignment: a docked
    /// toolbar docks to the SAFE edge, not the window edge, which is the same mistake the
    /// layout resolver exists to prevent for controls.
    /// </summary>
    private void PlaceChrome(TouchGamepadState state)
    {
        var region = state.Resolved.Region;

        // The menu button lives in the safe region's own top-left corner.
        MenuButton.Margin = new Thickness(region.Left + 12, region.Top + 12, 0, 0);

        if (ToolbarLayer.Visibility != Visibility.Visible)
        {
            return;
        }

        var placement = toolbarDrag ?? toolbar;
        ToolbarItems.Orientation = placement is TouchToolbarPlacement.Docked docked &&
                                   docked.Edge.Vertical()
            ? Orientation.Vertical
            : Orientation.Horizontal;

        var (x, y) = TouchToolbarLayout.TopLeft(
            placement, (float)Toolbar.ActualWidth, (float)Toolbar.ActualHeight, region);

        Canvas.SetLeft(Toolbar, x);
        Canvas.SetTop(Toolbar, y);
    }

    private void OnToolbarPressed(object sender, PointerRoutedEventArgs e)
    {
        toolbarGrab = e.GetCurrentPoint(Toolbar).Position;
        toolbarDrag = toolbar;
        ToolbarHandle.CapturePointer(e.Pointer);
        e.Handled = true;
    }

    private void OnToolbarMoved(object sender, PointerRoutedEventArgs e)
    {
        if (toolbarDrag is null)
        {
            return;
        }

        var point = e.GetCurrentPoint(Surface).Position;
        toolbarDrag = TouchToolbarLayout.PlacementFor(
            (float)(point.X - toolbarGrab.X),
            (float)(point.Y - toolbarGrab.Y),
            (float)Toolbar.ActualWidth,
            (float)Toolbar.ActualHeight,
            gamepad.State.Value.Resolved.Region);

        PlaceChrome(gamepad.State.Value);
        e.Handled = true;
    }

    private void OnToolbarReleased(object sender, PointerRoutedEventArgs e)
    {
        if (toolbarDrag is { } placement)
        {
            toolbar = placement;

            // Only a DOCK is remembered. A floating position is a working choice for one
            // window shape; the dock is the preference a handheld user makes once, because
            // which edge the toolbar occupies is a question about which hand is free.
            if (placement is TouchToolbarPlacement.Docked docked)
            {
                _ = AppServices.Documents.Write(DockDocument, docked.Edge.Key());
            }
        }

        toolbarDrag = null;
        ToolbarHandle.ReleasePointerCapture(e.Pointer);
        PlaceChrome(gamepad.State.Value);
    }

    // ------------------------------------------------------------------ toolbar acts

    private void OnUndo(object sender, RoutedEventArgs e) => gamepad.Undo();

    private void OnRedo(object sender, RoutedEventArgs e) => gamepad.Redo();

    private void OnSave(object sender, RoutedEventArgs e) => gamepad.Save();

    private void OnDelete(object sender, RoutedEventArgs e) => Delete();

    private void Delete()
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Remove", document =>
            TouchLayoutEditor.Delete(document, selection, editGroup));
    }

    private void OnDuplicate(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Duplicate", document =>
            TouchLayoutEditor.Duplicate(document, selection, editGroup));
    }

    private void OnGroupOrUngroup(object sender, RoutedEventArgs e)
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

    private void OnSmaller(object sender, RoutedEventArgs e) => Resize(1f / ScaleStep);

    private void OnLarger(object sender, RoutedEventArgs e) => Resize(ScaleStep);

    private void OnTurnLeft(object sender, RoutedEventArgs e) => Rotate(-RotationStep);

    private void OnTurnRight(object sender, RoutedEventArgs e) => Rotate(RotationStep);

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

    private void OnResetRotation(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        if (selection.Count == 0)
        {
            return;
        }

        gamepad.Edit("Reset orientation", document =>
            TouchLayoutEditor.ResetRotation(document, selection, editGroup));
    }

    private void OnBringForward(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Bring forward", document =>
            TouchLayoutEditor.BringForward(document, selection, editGroup));
    }

    private void OnSendBackward(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Send backward", document =>
            TouchLayoutEditor.SendBackward(document, selection, editGroup));
    }

    private void OnLatchToggled(object sender, RoutedEventArgs e)
    {
        var state = gamepad.State.Value;
        if (state.Personality is not { } personality || state.Selection.Count == 0)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        var latch = LatchItem.IsChecked;
        gamepad.Edit("Latch", document => TouchLayoutEditor.SetLatch(
            document, profile, state.Selection, latch, editGroup));
    }

    private void OnResetControl(object sender, RoutedEventArgs e)
    {
        var state = gamepad.State.Value;
        if (state.Personality is not { } personality || state.Selection.Count == 0)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        gamepad.Edit("Reset", document =>
            TouchLayoutEditor.Reset(document, profile, state.Selection, editGroup));
    }

    private void OnToggleEditGroup(object sender, RoutedEventArgs e)
    {
        editGroup = GroupsItem.IsChecked;
        Render();
    }

    private void OnToggleGrid(object sender, RoutedEventArgs e) =>
        SetAlignment(gamepad.State.Value.Alignment with { Grid = GridItem.IsChecked });

    private void OnToggleSnap(object sender, RoutedEventArgs e) =>
        SetAlignment(gamepad.State.Value.Alignment with { Snap = SnapItem.IsChecked });

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

    /// <summary>
    /// The Add menu, from the personality's own catalog.
    ///
    /// Grouped by <see cref="TouchControlCategory"/>, which exists for exactly this: it is
    /// presentation grouping and never affects bindings, so a GameCube Z can be filed as a
    /// shoulder without changing what it sends.
    /// </summary>
    private void BuildAddMenu()
    {
        AddMenu.Items.Clear();
        if (gamepad.State.Value.Personality is not { } personality)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        foreach (var group in profile.Catalog
                     .GroupBy(entry => entry.Category)
                     .OrderBy(group => group.Key))
        {
            var submenu = new MenuFlyoutSubItem { Text = group.Key.Title() };
            foreach (var entry in group)
            {
                // The same naming the rest of the surface uses, so a menu entry and an
                // audit finding call the control the same thing. The raw catalog id
                // ("stick-left") is a wire identifier, not a name.
                var item = new MenuFlyoutItem
                {
                    Text = TouchControlNaming.NameFor(
                        profile.Bindings.GetValueOrDefault(entry.Output),
                        entry.Visual.Label,
                        entry.Id),
                    Tag = entry.Id,
                };
                item.Click += OnAddControl;
                submenu.Items.Add(item);
            }

            AddMenu.Items.Add(submenu);
        }
    }

    private void OnAddControl(object sender, RoutedEventArgs e)
    {
        if (sender is not MenuFlyoutItem { Tag: string catalogId } ||
            gamepad.State.Value.Personality is not { } personality)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(personality);
        gamepad.Edit("Add", document =>
            TouchLayoutEditor.Add(document, profile, catalogId, 0.5f, 0.5f));
    }

    // --------------------------------------------------------------------- profiles

    private void OnResetProfile(object sender, RoutedEventArgs e)
    {
        SetMenuOpen(false);
        gamepad.ResetToDefault();
    }

    /// <summary>What the Layouts dialog was closed to do.</summary>
    private enum TouchProfileAction
    {
        None,
        Use,
        New,
        Duplicate,
        Rename,
        Delete,
        Export,
        Import,
    }

    /// <summary>
    /// The named-layout library, as one dialog.
    ///
    /// A dialog rather than a permanent picker in a header bar: the header bar was
    /// companion chrome standing around a controller, and choosing a layout is something a
    /// user does occasionally and deliberately.
    ///
    /// Every library action the header bar carried is here — use, new, duplicate, rename,
    /// delete, export, import — because moving a control panel is not a reason to lose the
    /// controls that were on it. The in-content buttons close the dialog with a chosen
    /// action rather than acting inside it, so the library is only ever mutated once the
    /// dialog is down and the surface can redraw against the result.
    /// </summary>
    private async void OnOpenProfiles(object sender, RoutedEventArgs e)
    {
        SetMenuOpen(false);

        var state = gamepad.State.Value;
        var profiles = state.Library.Profiles;
        var list = new ListView
        {
            SelectionMode = ListViewSelectionMode.Single,
            MaxHeight = 240,
            ItemsSource = profiles.Select(profile => profile.Name).ToList(),
            SelectedIndex = profiles
                .Select((profile, index) => (profile, index))
                .FirstOrDefault(entry => entry.profile.Id == state.Library.SelectedProfileId)
                .index,
        };

        var action = TouchProfileAction.None;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Layouts",
            PrimaryButtonText = "Use",
            SecondaryButtonText = "New…",
            CloseButtonText = "Close",
            DefaultButton = ContentDialogButton.Primary,
        };

        Button Action(string label, TouchProfileAction chosen)
        {
            var button = new Button { Content = label, MinWidth = 96 };
            button.Click += (_, _) =>
            {
                action = chosen;
                dialog.Hide();
            };
            return button;
        }

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(Action("Duplicate", TouchProfileAction.Duplicate));
        buttons.Children.Add(Action("Rename…", TouchProfileAction.Rename));
        buttons.Children.Add(Action("Delete", TouchProfileAction.Delete));
        buttons.Children.Add(Action("Export…", TouchProfileAction.Export));
        buttons.Children.Add(Action("Import…", TouchProfileAction.Import));

        var content = new StackPanel { Spacing = 12, MinWidth = 420 };
        content.Children.Add(list);
        content.Children.Add(buttons);
        dialog.Content = content;

        var result = await dialog.ShowAsync();
        if (action == TouchProfileAction.None)
        {
            action = result switch
            {
                ContentDialogResult.Primary => TouchProfileAction.Use,
                ContentDialogResult.Secondary => TouchProfileAction.New,
                _ => TouchProfileAction.None,
            };
        }

        var chosenProfile = list.SelectedIndex >= 0 && list.SelectedIndex < profiles.Count
            ? profiles[list.SelectedIndex]
            : null;

        await ApplyProfileActionAsync(action, chosenProfile);
    }

    private async Task ApplyProfileActionAsync(
        TouchProfileAction action, TouchLayoutProfile? profile)
    {
        switch (action)
        {
            case TouchProfileAction.Use when profile is not null:
                gamepad.SelectProfile(profile.Id);
                break;

            case TouchProfileAction.New:
                if (await AskForNameAsync(
                        "New layout",
                        TouchProfileLibraryEditor.DefaultNewProfileName) is { } created)
                {
                    gamepad.CreateProfile(created);
                }

                break;

            case TouchProfileAction.Duplicate when profile is not null:
                gamepad.DuplicateProfile(profile.Id);
                break;

            case TouchProfileAction.Rename when profile is not null:
                if (await AskForNameAsync("Rename layout", profile.Name) is { } renamed)
                {
                    gamepad.RenameProfile(profile.Id, renamed);
                }

                break;

            case TouchProfileAction.Delete when profile is not null:
                gamepad.DeleteProfile(profile.Id);
                break;

            case TouchProfileAction.Export when profile is not null:
                await ExportAsync(profile);
                break;

            case TouchProfileAction.Import:
                await ImportAsync();
                break;
        }
    }

    private async Task ExportAsync(TouchLayoutProfile profile)
    {
        if (gamepad.Export(profile.Id) is not { } encoded)
        {
            return;
        }

        var picker = new FileSavePicker { SuggestedFileName = profile.Name };
        picker.FileTypeChoices.Add("Layout", [".json"]);
        Initialize(picker);

        if (await picker.PickSaveFileAsync() is { } file)
        {
            await FileIO.WriteTextAsync(file, encoded);
        }
    }

    private async Task ImportAsync()
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".json");
        Initialize(picker);

        if (await picker.PickSingleFileAsync() is not { } file)
        {
            return;
        }

        if (gamepad.Import(await FileIO.ReadTextAsync(file)) is { } refusal)
        {
            await MessageAsync("That layout could not be imported", refusal);
        }
    }

    /// <summary>
    /// A picker in an unpackaged process has no window of its own and throws without one.
    /// </summary>
    private static void Initialize(object picker)
    {
        if (App.Window is { } window)
        {
            WinRT.Interop.InitializeWithWindow.Initialize(
                picker, WinRT.Interop.WindowNative.GetWindowHandle(window));
        }
    }

    private async Task MessageAsync(string title, string message) =>
        await new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = title,
            Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
            CloseButtonText = "Close",
        }.ShowAsync();

    private async Task<string?> AskForNameAsync(string title, string initial)
    {
        var input = new TextBox { Text = initial, SelectionStart = initial.Length };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = title,
            Content = input,
            PrimaryButtonText = "OK",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        return await dialog.ShowAsync() == ContentDialogResult.Primary &&
               input.Text.Trim() is { Length: > 0 } name
            ? name
            : null;
    }
}
