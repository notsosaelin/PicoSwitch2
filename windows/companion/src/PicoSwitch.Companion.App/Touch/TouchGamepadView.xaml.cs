using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
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
/// The Touch Gamepad surface and its layout editor.
///
/// ## What this file is allowed to decide
///
/// Where a finger is, and which shared function to call about it. Nothing else.
/// Geometry comes from the resolved layout, every edit is a
/// <see cref="TouchLayoutEditor"/> call, every sentence is a
/// <see cref="TouchEditorView"/> property, and the toolbar's position comes from
/// <see cref="TouchToolbarLayout"/>. That division is the whole reason the touch
/// subsystem could be ported at all, and it is the reason the Android and Windows
/// companions cannot drift apart over what an edit means.
///
/// ## Why there is no gameplay here
///
/// Routing contacts to a console is Phase 6b and is gated on Controller Link, which
/// this build does not have
/// (docs/experiments/windows-hogp-bridge-feasibility-2026-08-31.md). §15.8 says the
/// surface must still open, stay fully editable, and SAY what is missing rather than
/// appear broken — so the pointer handlers below are the editor's, the link note is
/// permanent, and no contact reaches <c>TouchGamepad</c>.
/// </summary>
public sealed partial class TouchGamepadView : UserControl
{
    private const string DockDocument = "touch-editor-dock";
    private const string AlignmentDocument = "touch-editor-align";

    private readonly TouchGamepadService gamepad = AppServices.TouchGamepad;
    private readonly TouchControlRenderer renderer;

    private TouchToolbarPlacement toolbar = TouchToolbarPlacement.Default;
    private TouchToolbarPlacement? toolbarDrag;
    private Point toolbarGrab;

    private uint? dragPointer;
    private string? dragPrimary;
    private Point dragLast;
    private bool dragMoved;

    private bool suppressProfileEvents;

    /// <summary>Raised when the user leaves the surface, so the shell can restore its chrome.</summary>
    public event Action? CloseRequested;

    public TouchGamepadView()
    {
        InitializeComponent();
        renderer = new TouchControlRenderer(Surface);

        toolbar = TouchToolbarEdges.FromKey(AppServices.Documents.Read(DockDocument)) is { } edge
            ? new TouchToolbarPlacement.Docked(edge)
            : TouchToolbarPlacement.Default;

        gamepad.SetAlignment(ReadAlignment());

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    // ------------------------------------------------------------------- lifecycle

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        gamepad.State.Changed += OnStateChanged;
        AppServices.Adapters.Snapshot.Changed += OnSnapshotChanged;
        AppServices.Adapters.Registry.Changed += OnSnapshotChanged;

        ApplyPersonality();
        BuildAddMenu();
        Measure();
        Render();

        // Focus the canvas, not the first button: a user who opened the editor wants to
        // act on the layout, and §26.5 runs it by keyboard before anything else.
        Surface.Focus(FocusState.Programmatic);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        gamepad.State.Changed -= OnStateChanged;
        AppServices.Adapters.Snapshot.Changed -= OnSnapshotChanged;
        AppServices.Adapters.Registry.Changed -= OnSnapshotChanged;
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
    /// The confirmed personality, never a guess (§15.8).
    /// </summary>
    /// <summary>
    /// The confirmed personality, never a guess (§15.8) — live when an adapter is
    /// connected, otherwise the last one the active adapter was confirmed to be showing.
    ///
    /// The remembered value is what makes the editor useful with nothing attached, which
    /// is the Phase 6a exit criterion; the surface says which of the two it is.
    /// </summary>
    private void ApplyPersonality() => gamepad.SetPersonality(
        AppServices.Adapters.Snapshot.Value.Personality.Current,
        AppServices.Adapters.Registry.Value.Active?.LastPersonality);

    // ---------------------------------------------------------------------- region

    private void OnSurfaceSizeChanged(object sender, SizeChangedEventArgs e) => Measure();

    /// <summary>
    /// Build the interaction-safe rectangle and re-resolve into it.
    ///
    /// The insets are the platform's contribution (§15.5): an edge-gesture strip on a
    /// touch machine, and — because this is a full-window mode with no drag region of its
    /// own — no caption reservation.
    /// </summary>
    private void Measure()
    {
        // Whether this machine can produce a touch contact at all. A pointer-only
        // desktop gives up no room to gesture strips it does not have.
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
        var view = TouchEditorView.Of(state, controllerLinkAvailable: false);

        Title.Text = view.Title;
        Subtitle.Text = view.Subtitle;

        StatusBar.Message = view.Status;
        StatusBar.Severity = view.StatusSeverity switch
        {
            TouchEditorSeverity.Blocking => InfoBarSeverity.Error,
            TouchEditorSeverity.Advisory => InfoBarSeverity.Warning,
            _ => InfoBarSeverity.Informational,
        };

        LinkBar.Message = view.LinkNote ?? string.Empty;
        LinkBar.IsOpen = view.LinkNote is not null;

        RenderProfiles(state);
        RenderToolbar(view);
        RenderProperties(state, view);

        renderer.Draw(state.Resolved, new TouchRenderOptions
        {
            Selection = state.Selection,
            Invalid = state.Resolved.InvalidControlIds,
            Editing = view.Editable,
            Grid = TouchEditorAlignment.GridLines(state.Resolved.Region, state.Alignment),
            Guides = TouchEditorAlignment.MatchedGuides(
                state.Resolved, state.Selection, state.Selection.FirstOrDefault(), state.Alignment),
        });

        PlaceToolbar();
    }

    private void RenderProfiles(TouchGamepadState state)
    {
        suppressProfileEvents = true;
        try
        {
            ProfilePicker.Items.Clear();
            foreach (var profile in state.Library.Profiles)
            {
                ProfilePicker.Items.Add(new ComboBoxItem
                {
                    Content = profile.Name,
                    Tag = profile.Id,
                });
            }

            ProfilePicker.SelectedIndex = state.Library.Profiles
                .Select((profile, index) => (profile, index))
                .FirstOrDefault(entry => entry.profile.Id == state.Library.SelectedProfileId)
                .index;

            // The factory profile is synthesized and immutable; the three actions that
            // would write to it are simply not offered there.
            var factory = state.Library.Selected.IsFactory;
            RenameProfile.IsEnabled = !factory;
            DeleteProfile.IsEnabled = !factory;
            ResetProfile.IsEnabled = !factory;
        }
        finally
        {
            suppressProfileEvents = false;
        }
    }

    private void RenderToolbar(TouchEditorView view)
    {
        Toolbar.Visibility = view.Editable ? Visibility.Visible : Visibility.Collapsed;
        UndoButton.IsEnabled = view.CanUndo;
        RedoButton.IsEnabled = view.CanRedo;
        SaveButton.IsEnabled = view.CanSave;
        DiscardButton.IsEnabled = view.CanSave;
        DeleteButton.IsEnabled = view.CanDelete;
        GroupButton.IsEnabled = view.CanGroup;
        UngroupButton.IsEnabled = view.CanUngroup;

        var alignment = gamepad.State.Value.Alignment;
        GridToggle.IsChecked = alignment.Grid;
        SnapToggle.IsChecked = alignment.Snap;
    }

    private void RenderProperties(TouchGamepadState state, TouchEditorView view)
    {
        Properties.Visibility = view.Editable ? Visibility.Visible : Visibility.Collapsed;
        SelectionSummary.Text = view.SelectionSummary;

        var hasSelection = state.Selection.Count > 0;
        SizePanel.Visibility = hasSelection ? Visibility.Visible : Visibility.Collapsed;
        LatchToggle.Visibility = hasSelection ? Visibility.Visible : Visibility.Collapsed;
        ResetControl.IsEnabled = hasSelection;

        if (view.Scale is { } scale)
        {
            ScaleSlider.Value = Math.Round(scale * 100d);
        }

        var instance = state.Selection.Count == 1
            ? state.Document.Instance(state.Selection.First())
            : null;
        LatchToggle.IsChecked = instance?.Latch ?? false;
        LatchToggle.IsEnabled = instance is not null;

        AuditList.ItemsSource = view.Findings
            .Select(finding => (finding.Blocking ? "• " : "· ") + finding.Message)
            .ToList();
    }

    // --------------------------------------------------------------- surface input

    private void OnSurfacePressed(object sender, PointerRoutedEventArgs e)
    {
        if (!gamepad.State.Value.Editable)
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
            snapped.X, snapped.Y, editGroup: true));

        dragMoved = true;
        e.Handled = true;
    }

    private void OnSurfaceReleased(object sender, PointerRoutedEventArgs e)
    {
        if (dragPointer != e.Pointer.PointerId)
        {
            return;
        }

        Surface.ReleasePointerCapture(e.Pointer);
        EndDrag();
        e.Handled = true;
    }

    /// <summary>
    /// Capture taken away mid-drag.
    ///
    /// The gesture is still committed: the document already carries every applied delta,
    /// and throwing it away would lose a move the user watched happen. What must not
    /// happen is a half-drag left outside the undo history, which is the case this exists
    /// to close.
    /// </summary>
    private void OnSurfaceCaptureLost(object sender, PointerRoutedEventArgs e) => EndDrag();

    private void EndDrag()
    {
        if (dragMoved)
        {
            gamepad.Commit("Move");
        }

        dragPointer = null;
        dragPrimary = null;
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
        var modifiers = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control);
        var control = modifiers.HasFlag(CoreVirtualKeyStates.Down);
        var shift = InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift)
            .HasFlag(CoreVirtualKeyStates.Down);

        var stroke = TouchEditorKeys.Resolve(e.Key.ToString(), control, shift);
        if (stroke.Command == TouchEditorCommand.None)
        {
            return;
        }

        // Escape with nothing selected is the way OUT, so focus is never trapped in the
        // layout. Left unhandled, it falls through to the shell.
        if (stroke.Command == TouchEditorCommand.Deselect && state.Selection.Count == 0)
        {
            CloseRequested?.Invoke();
            e.Handled = true;
            return;
        }

        if (!state.Editable && stroke.Command is not TouchEditorCommand.Exit)
        {
            return;
        }

        e.Handled = Apply(stroke, state);
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
                return Resize(TouchEditorKeys.ScaleStep);
            case TouchEditorCommand.Shrink:
                return Resize(1f / TouchEditorKeys.ScaleStep);

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
                OnDelete(this, new RoutedEventArgs());
                return true;
            case TouchEditorCommand.Group:
                OnGroup(this, new RoutedEventArgs());
                return true;
            case TouchEditorCommand.Ungroup:
                OnUngroup(this, new RoutedEventArgs());
                return true;

            case TouchEditorCommand.SelectAll:
                gamepad.SetSelection(state.Resolved.Controls
                    .Select(control => control.Id)
                    .ToHashSet(StringComparer.Ordinal));
                return true;
            case TouchEditorCommand.Deselect:
                gamepad.SetSelection(new HashSet<string>(StringComparer.Ordinal));
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
            document, state.Resolved, state.Selection, x, y, editGroup: true));
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
            document, state.Resolved, state.Selection, factor, editGroup: true));
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

    // ------------------------------------------------------------------- toolbar

    private void OnToolbarSizeChanged(object sender, SizeChangedEventArgs e) => PlaceToolbar();

    private void PlaceToolbar()
    {
        var region = gamepad.State.Value.Resolved.Region;
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

        PlaceToolbar();
        e.Handled = true;
    }

    private void OnToolbarReleased(object sender, PointerRoutedEventArgs e)
    {
        if (toolbarDrag is { } placement)
        {
            toolbar = placement;

            // Only a DOCK is remembered. A floating position is a working choice for one
            // window shape; the dock is the preference a handheld user makes once,
            // because which edge the toolbar occupies is a question about which hand is
            // free.
            if (placement is TouchToolbarPlacement.Docked docked)
            {
                _ = AppServices.Documents.Write(DockDocument, docked.Edge.Key());
            }
        }

        toolbarDrag = null;
        ToolbarHandle.ReleasePointerCapture(e.Pointer);
        PlaceToolbar();
    }

    // ------------------------------------------------------------------ toolbar acts

    private void OnUndo(object sender, RoutedEventArgs e) => gamepad.Undo();

    private void OnRedo(object sender, RoutedEventArgs e) => gamepad.Redo();

    private void OnSave(object sender, RoutedEventArgs e) => gamepad.Save();

    private void OnDiscard(object sender, RoutedEventArgs e) => gamepad.Discard();

    private void OnDelete(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Remove", document =>
            TouchLayoutEditor.Delete(document, selection, editGroup: true));
    }

    private void OnGroup(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Group", document => TouchLayoutEditor.Group(document, selection));
    }

    private void OnUngroup(object sender, RoutedEventArgs e)
    {
        var selection = gamepad.State.Value.Selection;
        gamepad.Edit("Ungroup", document => TouchLayoutEditor.Ungroup(document, selection));
    }

    private void OnToggleGrid(object sender, RoutedEventArgs e) =>
        SetAlignment(gamepad.State.Value.Alignment with { Grid = GridToggle.IsChecked == true });

    private void OnToggleSnap(object sender, RoutedEventArgs e) =>
        SetAlignment(gamepad.State.Value.Alignment with { Snap = SnapToggle.IsChecked == true });

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

    // ------------------------------------------------------------ add / properties

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

    private void OnScaleChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        var state = gamepad.State.Value;
        if (state.Selection.Count == 0)
        {
            return;
        }

        var wanted = (float)(e.NewValue / 100d);
        if (MathF.Abs(wanted - (TouchEditorView.Of(state, false).Scale ?? -1f)) < 0.005f)
        {
            return;
        }

        gamepad.Edit("Resize", document =>
            TouchLayoutEditor.SetScale(document, state.Selection, wanted, editGroup: true));
    }

    private void OnLatchToggled(object sender, RoutedEventArgs e)
    {
        var state = gamepad.State.Value;
        if (state.Selection.Count == 0)
        {
            return;
        }

        var profile = TouchProfileCatalog.Require(state.Personality!.Value);
        gamepad.Edit("Latch", document => TouchLayoutEditor.SetLatch(
            document, profile, state.Selection, LatchToggle.IsChecked == true, editGroup: true));
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
            TouchLayoutEditor.Reset(document, profile, state.Selection, editGroup: true));
    }

    // --------------------------------------------------------------------- profiles

    private void OnProfilePicked(object sender, SelectionChangedEventArgs e)
    {
        if (suppressProfileEvents ||
            ProfilePicker.SelectedItem is not ComboBoxItem { Tag: string profileId })
        {
            return;
        }

        gamepad.SelectProfile(profileId);
    }

    private async void OnNewProfile(object sender, RoutedEventArgs e)
    {
        if (await AskForNameAsync("New layout", TouchProfileLibraryEditor.DefaultNewProfileName)
                is { } name)
        {
            gamepad.CreateProfile(name);
        }
    }

    private void OnDuplicateProfile(object sender, RoutedEventArgs e) =>
        gamepad.DuplicateProfile(gamepad.State.Value.Library.SelectedProfileId);

    private async void OnRenameProfile(object sender, RoutedEventArgs e)
    {
        var current = gamepad.State.Value.Library.Selected;
        if (await AskForNameAsync("Rename layout", current.Name) is { } name)
        {
            gamepad.RenameProfile(current.Id, name);
        }
    }

    private void OnDeleteProfile(object sender, RoutedEventArgs e) =>
        gamepad.DeleteProfile(gamepad.State.Value.Library.SelectedProfileId);

    private void OnResetProfile(object sender, RoutedEventArgs e) => gamepad.ResetToDefault();

    private async void OnExportProfile(object sender, RoutedEventArgs e)
    {
        var state = gamepad.State.Value;
        if (gamepad.Export(state.Library.SelectedProfileId) is not { } encoded)
        {
            return;
        }

        var picker = new FileSavePicker { SuggestedFileName = state.Library.Selected.Name };
        picker.FileTypeChoices.Add("Layout", [".json"]);
        Initialize(picker);

        if (await picker.PickSaveFileAsync() is { } file)
        {
            await FileIO.WriteTextAsync(file, encoded);
        }
    }

    private async void OnImportProfile(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".json");
        Initialize(picker);

        if (await picker.PickSingleFileAsync() is not { } file)
        {
            return;
        }

        var refusal = gamepad.Import(await FileIO.ReadTextAsync(file));
        if (refusal is not null)
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

    private async Task MessageAsync(string title, string message) =>
        await new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = title,
            Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
            CloseButtonText = "Close",
        }.ShowAsync();

    private void OnClose(object sender, RoutedEventArgs e) => CloseRequested?.Invoke();
}
