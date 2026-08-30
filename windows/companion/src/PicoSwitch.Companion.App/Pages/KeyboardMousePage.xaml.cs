using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Keyboard and mouse mapping (§16.3) — the desktop advantage worth taking.
///
/// The Android app pages through a list because a phone cannot show 104 keys.
/// This draws the keyboard and lets the user click the key they mean.
///
/// The keyboard is built in code rather than markup because the layout is DATA:
/// <see cref="KeyboardLayout"/> owns which usage sits where and how wide it is,
/// and that table is unit-tested. Two hundred lines of hand-written XAML buttons
/// would be the same information in a form nothing can check.
///
/// As with every page here, no policy lives in this file. What is enabled, what a
/// slider's bounds are, and what each cell says all come from
/// <see cref="KeyboardMouseView"/>.
/// </summary>
public sealed partial class KeyboardMousePage : Page
{
    /// <summary>One key unit, in pixels. Everything in the layout is a multiple.</summary>
    private const double KeyUnit = 46;

    /// <summary>Taken OUT of each key rather than added between them, so 2u spans 2u.</summary>
    private const double KeyGap = 4;

    /// <summary>How long a slider must be still before its value is sent.</summary>
    private const int SliderSettleMillis = 250;

    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    private KbmLayout profile = KbmLayout.Keyboard;

    /// <summary>
    /// Whether the user has chosen which layout to view.
    ///
    /// Until they do, the page follows the layout the adapter is resolving, so
    /// what is on screen is what the console is using.
    /// </summary>
    private bool profileChosenByUser;

    /// <summary>
    /// Suppresses the selector's SelectionChanged while Render() populates it.
    /// Without it, redrawing the page would look like the user picking a
    /// profile and would open a different one on every refresh.
    /// </summary>
    private bool populatingProfiles;

    /// <summary>
    /// The local, unsaved copy of the profile being edited.
    ///
    /// Every edit on this page mutates THIS and nothing else. No management
    /// command is sent until the user presses Save, which is what makes Save and
    /// Discard mean anything and what stops a flash erase per keystroke.
    /// </summary>
    private KeyboardMouseDraft? draft;
    private DispatcherTimer? mouseCommit;
    private KbmMouseField? pendingMouseField;
    private int pendingMouseValue;
    private KeyboardMouseView? view;
    private bool suppressSelection;

    public KeyboardMousePage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        adapters.KeyboardMouse.Changed += OnStateChanged;
        adapters.KeyboardMouseBusy.Changed += OnStateChanged;
        adapters.Connection.Changed += OnStateChanged;

        Render();

        // Read on arrival when there is a session, so the page is useful without a
        // manual step. A failure here is reported, not thrown at the user as an
        // empty keyboard with no explanation.
        if (adapters.Connection.Value.Connected && !adapters.KeyboardMouse.Value.Loaded)
        {
            await SafeAsync(() => adapters.RefreshKeyboardMouseAsync());
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        adapters.KeyboardMouse.Changed -= OnStateChanged;
        adapters.KeyboardMouseBusy.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;

        // A pending commit belongs to a page that no longer exists.
        mouseCommit?.Stop();
        pendingMouseField = null;
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    /* ------------------------------------------------------------- actions */

    private async void OnRefresh(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.RefreshKeyboardMouseAsync());

    private async void OnSetMode(object sender, RoutedEventArgs e)
    {
        if (ModeBox.SelectedIndex < 0)
        {
            return;
        }

        var mode = (KbmMode)ModeBox.SelectedIndex;
        await SafeAsync(async () =>
        {
            var status = await adapters.SetKeyboardMouseModeAsync(mode);

            // The adapter decides. A mode needing a device that is not connected
            // resolves elsewhere, and saying "done" would be untrue.
            if (status.Mode != mode && status.ModeOverride == mode)
            {
                Report(
                    $"Set to {KeyboardMouse.Label(mode)}. The adapter is still using " +
                    $"{KeyboardMouse.Label(status.Mode)} until the device it needs is connected.",
                    InfoBarSeverity.Informational);
            }
        });
    }

    private void RenderProfileSelector(KeyboardMouseView view)
    {
        // Firmware without a profile library hides the whole row and behaves as
        // it always did: one mapping per layout, edited in place.
        if (!view.ProfilesSupported)
        {
            ProfileRow.Visibility = Visibility.Collapsed;
            DraftBar.IsOpen = false;
            return;
        }

        ProfileRow.Visibility = Visibility.Visible;
        populatingProfiles = true;
        try
        {
            ProfileSelector.Items.Clear();
            var selected = -1;
            for (var i = 0; i < view.Profiles.Count; i++)
            {
                var row = view.Profiles[i];
                ProfileSelector.Items.Add(new ComboBoxItem
                {
                    // Built-in Default is labelled as such so a user can tell at
                    // a glance which row they cannot rename or delete.
                    Content = row.Builtin ? $"{row.Name} (built-in)" : row.Name,
                    Tag = row.Id,
                });
                if (row.Id == view.SelectedProfile?.Id)
                {
                    selected = i;
                }
            }

            ProfileSelector.SelectedIndex = selected;
        }
        finally
        {
            populatingProfiles = false;
        }

        // Save and Apply are separate actions with separate enablement, which is
        // the whole contract this page exists to express.
        SaveButton.IsEnabled = view.CanSave;
        DiscardButton.IsEnabled = view.Dirty;
        ApplyButton.IsEnabled = view.CanApply;
        RenameButton.IsEnabled = view.CanRename;
        DeleteButton.IsEnabled = view.CanDelete;
        NewButton.IsEnabled = view.ProfilesSupported &&
                              view.DraftState != KbmDraftState.Disconnected;

        ProfileStatus.Text = view.StatusText;
        DraftBar.IsOpen = view.StatusDetail is not null;
        DraftBar.Message = view.StatusDetail ?? string.Empty;
        DraftBar.Severity = view.DraftState switch
        {
            KbmDraftState.Conflict => InfoBarSeverity.Error,
            KbmDraftState.SavedNotApplied => InfoBarSeverity.Warning,
            _ => InfoBarSeverity.Informational,
        };
    }

    /// <summary>
    /// Selecting a profile OPENS it for viewing and editing. It does not apply
    /// it: a list selection must never change what the console is doing.
    /// </summary>
    private async void OnSelectedProfileChanged(object sender, SelectionChangedEventArgs e)
    {
        if (populatingProfiles || ProfileSelector.SelectedItem is not ComboBoxItem item ||
            item.Tag is not int id || draft?.ProfileId == id)
        {
            return;
        }

        if (draft?.Dirty == true && !await ConfirmAsync(
                "Discard unsaved changes?",
                "The changes to this profile have not been saved to the adapter " +
                "and will be lost.",
                "Discard"))
        {
            Render();  // put the selector back on the profile still being edited
            return;
        }

        await OpenProfileAsync(id);
    }

    /// <summary>Load one profile's stored mapping into a fresh local draft.</summary>
    private async Task OpenProfileAsync(int id)
    {
        var state = adapters.KeyboardMouse.Value;
        var row = state.Profiles.For(profile).FirstOrDefault(p => p.Id == id);
        if (row is null)
        {
            return;
        }

        var mapping = await SafeAsync(() => adapters.LoadKeyboardMouseProfileAsync(row));
        if (mapping is null)
        {
            return;
        }

        draft = KeyboardMouseDraft.From(row, mapping.Bindings, state.Mouse);
        Render();
    }

    private async void OnSaveProfile(object sender, RoutedEventArgs e)
    {
        if (draft is null)
        {
            return;
        }

        var editing = draft;
        // Editing the built-in Default becomes a NEW profile: the template is
        // never written into, which is what keeps it always available.
        if (editing.IsBuiltin)
        {
            var name = await PromptAsync("Save as new profile", "Profile name",
                                         SuggestName("My mapping"));
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            editing = editing.WithName(name!);
        }

        var saved = await SafeAsync(() => adapters.SaveKeyboardMouseProfileAsync(editing));
        if (saved is null)
        {
            return;
        }

        draft = saved;
        Render();
    }

    private void OnDiscardProfile(object sender, RoutedEventArgs e)
    {
        // Local only. This is the point of the draft model: there is nothing on
        // the adapter to undo.
        draft = draft?.Discard();
        Render();
    }

    private async void OnApplyProfile(object sender, RoutedEventArgs e)
    {
        if (draft is null)
        {
            return;
        }

        await SafeAsync(() =>
            adapters.ApplyKeyboardMouseProfileAsync(draft.Layout, draft.ProfileId));
        Render();
    }

    private async void OnNewProfile(object sender, RoutedEventArgs e)
    {
        var source = draft?.ProfileId ?? KbmProfileIds.Default;
        var name = await PromptAsync(
            "New profile",
            source == KbmProfileIds.Default
                ? "Starts from the built-in Default mapping."
                : "Starts as a copy of the profile you are viewing.",
            SuggestName(source == KbmProfileIds.Default ? "My mapping" : "Copy"));
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }

        if (source == KbmProfileIds.Default)
        {
            // A profile from Default is an empty draft saved under a new name;
            // no adapter round trip is needed to compose it.
            var state = adapters.KeyboardMouse.Value;
            var template = state.Profiles.For(profile).First(p => p.Builtin);
            var mapping = await SafeAsync(
                () => adapters.LoadKeyboardMouseProfileAsync(template));
            if (mapping is null)
            {
                return;
            }

            var fresh = KeyboardMouseDraft
                .From(template, mapping.Bindings, state.Mouse)
                .WithName(name!);
            var created = await SafeAsync(() => adapters.SaveKeyboardMouseProfileAsync(fresh));
            if (created is null)
            {
                return;
            }

            draft = created;
        }
        else
        {
            var after = await SafeAsync(
                () => adapters.DuplicateKeyboardMouseProfileAsync(source, name!));
            if (after is null)
            {
                return;
            }
        }

        Render();
    }

    private async void OnRenameProfile(object sender, RoutedEventArgs e)
    {
        if (draft is null || draft.IsBuiltin)
        {
            return;
        }

        var name = await PromptAsync("Rename profile", "Profile name", draft.Name);
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }

        var after = await SafeAsync(
            () => adapters.RenameKeyboardMouseProfileAsync(draft.ProfileId, name!));
        if (after is not null)
        {
            draft = draft.WithName(name!).Rebased(draft.ProfileId, draft.BaseRevision,
                                                  name!);
        }

        Render();
    }

    private async void OnDeleteProfile(object sender, RoutedEventArgs e)
    {
        if (draft is null || draft.IsBuiltin)
        {
            return;
        }

        var active = adapters.KeyboardMouse.Value.Profiles.ActiveFor(profile);
        var applied = active?.SourceId == draft.ProfileId;
        if (!await ConfirmAsync(
                $"Delete '{draft.Name}'?",
                applied
                    ? "This profile is what the console is using. Deleting it " +
                      "switches this layout back to the built-in Default mapping."
                    : "This profile is removed from the adapter. The mapping the " +
                      "console is using does not change.",
                "Delete"))
        {
            return;
        }

        var after = await SafeAsync(
            () => adapters.DeleteKeyboardMouseProfileAsync(draft.ProfileId));
        if (after is not null)
        {
            draft = null;
            await OpenProfileAsync(KbmProfileIds.Default);
        }

        Render();
    }

    /// <summary>A name that is not already taken in this layout.</summary>
    private string SuggestName(string basis)
    {
        var taken = adapters.KeyboardMouse.Value.Profiles
            .For(profile)
            .Select(p => p.Name)
            .ToHashSet(StringComparer.CurrentCultureIgnoreCase);
        if (!taken.Contains(basis))
        {
            return basis;
        }

        for (var i = 2; i < 100; i++)
        {
            var candidate = $"{basis} {i}";
            if (!taken.Contains(candidate))
            {
                return candidate;
            }
        }

        return basis;
    }

    private void OnProfileChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection || ProfileBox.SelectedIndex < 0)
        {
            return;
        }

        profile = (KbmLayout)ProfileBox.SelectedIndex;
        profileChosenByUser = true;
        Render();
    }

    private async void OnResetProfile(object sender, RoutedEventArgs e)
    {
        if (!await ConfirmAsync(
                "Reset this profile?",
                "Every key in this profile goes back to the adapter's own default mapping. " +
                "The other profile and the mouse tuning are not affected.",
                "Reset profile"))
        {
            return;
        }

        await SafeAsync(() => adapters.ResetProfileAsync(profile));
    }

    private async void OnResetAll(object sender, RoutedEventArgs e)
    {
        if (!await ConfirmAsync(
                "Reset everything?",
                "Both profiles and the mouse tuning go back to the adapter's own defaults. " +
                "Every mapping you have changed is lost.",
                "Reset everything"))
        {
            return;
        }

        await SafeAsync(() => adapters.ResetAllKeyboardMouseAsync());
    }

    private async void OnEditUndrawn(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: int usage })
        {
            await EditAsync(new KbmSource(KbmSourceKind.Key, usage), KeyboardLayout.Describe(
                new KbmSource(KbmSourceKind.Key, usage)));
        }
    }

    private async void OnInvertX(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.SetMouseAsync(
            KbmMouseField.InvertX,
            InvertXBox.IsChecked == true ? 1 : 0));

    private async void OnInvertY(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.SetMouseAsync(
            KbmMouseField.InvertY,
            InvertYBox.IsChecked == true ? 1 : 0));

    /// <summary>
    /// Pick a destination for one source, clear it, or restore its default.
    ///
    /// **Clearing and restoring are different operations**, and the protocol says
    /// so: <c>none</c> makes the key do nothing, <c>default</c> puts back whatever
    /// the adapter shipped with. Offering only one of them either wipes a key the
    /// user wanted restored or restores one they wanted silent, so the list
    /// carries "Not mapped" and the dialog carries a separate Restore default
    /// button.
    /// </summary>
    private async Task EditAsync(KbmSource source, string label)
    {
        var destinations = Enum.GetValues<KbmDestination>();
        var list = new ListView
        {
            SelectionMode = ListViewSelectionMode.Single,
            MaxHeight = 380,
            ItemsSource = destinations.Select(KeyboardMouseView.Describe).ToArray(),
        };

        var current = CurrentDestination(source);
        list.SelectedIndex = Array.IndexOf(destinations, current);

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"What should {label} do?",
            Content = list,
            PrimaryButtonText = "Set",
            SecondaryButtonText = "Restore default",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        var result = await dialog.ShowAsync();

        // Both paths edit the LOCAL DRAFT and send nothing. That is the whole
        // point of the draft: rebinding thirty keys costs zero adapter writes
        // and zero flash erases, and Discard can actually undo it.
        //
        // Without a profile library (older firmware) there is no draft, and the
        // per-binding command remains the only way to change a mapping.
        if (result == ContentDialogResult.Secondary)
        {
            if (draft is not null)
            {
                draft = draft.With(source, DefaultDestination(source));
                Render();
            }
            else
            {
                // A null destination is the protocol's `default`.
                await SafeAsync(() => adapters.BindAsync(profile, source, destination: null));
            }

            return;
        }

        if (result != ContentDialogResult.Primary || list.SelectedIndex < 0)
        {
            return;
        }

        var chosen = destinations[list.SelectedIndex];
        if (draft is not null)
        {
            draft = draft.With(source, chosen);
            Render();
            return;
        }

        await SafeAsync(() => adapters.BindAsync(profile, source, chosen));
    }

    /// <summary>
    /// What "restore default" means inside a draft.
    ///
    /// The adapter's canonical default for this input, read from the built-in
    /// Default profile the page already has. Reconstructing it from firmware
    /// source in the client is exactly what the sparse-override model exists to
    /// avoid.
    /// </summary>
    private KbmDestination DefaultDestination(KbmSource source)
    {
        var cells = source.Kind == KbmSourceKind.MouseButton
            ? view?.MouseButtons
            : view?.Keys;
        var cell = cells?.FirstOrDefault(c => c.Cap.Usage == source.Code);
        return cell is { Overridden: false } ? cell.Destination : KbmDestination.None;
    }

    private KbmDestination CurrentDestination(KbmSource source)
    {
        if (view is null)
        {
            return KbmDestination.None;
        }

        var cells = source.Kind == KbmSourceKind.MouseButton ? view.MouseButtons : view.Keys;
        return cells.FirstOrDefault(cell => cell.Cap.Usage == source.Code)?.Destination
            ?? view.Undrawn.FirstOrDefault(cell => cell.Cap.Usage == source.Code)?.Destination
            ?? KbmDestination.None;
    }

    /* ------------------------------------------------------------- painting */

    private void Render()
    {
        var connected = adapters.Connection.Value.Connected;
        var state = adapters.KeyboardMouse.Value;

        // Follow the adapter until the user says otherwise.
        if (!profileChosenByUser && state.Loaded)
        {
            profile = state.Status.Profile;
        }

        // A draft belongs to one layout. Switching layouts closes it rather than
        // silently applying keyboard edits to a keyboard-and-mouse mapping.
        if (draft is not null && draft.Layout != profile)
        {
            draft = null;
        }

        view = KeyboardMouse.Project(state, profile, connected, draft);

        UnsupportedBar.IsOpen = !view.Visible;
        UnsupportedBar.Message = view.HiddenReason ?? string.Empty;
        Body.Visibility = view.Visible ? Visibility.Visible : Visibility.Collapsed;
        if (!view.Visible)
        {
            return;
        }

        BusyRing.IsActive = adapters.KeyboardMouseBusy.Value;
        ModeText.Text = view.Loaded ? view.ModeText : "Not read yet";
        DevicesText.Text = view.Loaded ? view.DevicesText : string.Empty;

        suppressSelection = true;
        ModeBox.SelectedIndex = (int)view.ModeOverride;
        ProfileBox.SelectedIndex = (int)profile;
        suppressSelection = false;

        var enabled = view.Availability.Enabled;

        // Commands are gated on the busy flag as well as on the connection.
        //
        // Without this, Reload stays clickable while a reload is running, so an
        // impatient click queues a SECOND full read -- four more exchanges behind
        // the first -- and a third, and a fourth. Observed on hardware 2026-08-29:
        // five reloads in five seconds against an adapter that was already slow to
        // answer. The busy flag existed; the buttons were simply not wired to it.
        //
        // The keys and sliders are deliberately NOT gated this way: the whole point
        // of a section-scoped flag is that live tuning stays usable, and a slider
        // already coalesces to its settled value.
        var ready = enabled && !adapters.KeyboardMouseBusy.Value;
        ModeBox.IsEnabled = ready;
        ModeApply.IsEnabled = ready;
        RefreshButton.IsEnabled = ready;
        ResetProfile.IsEnabled = ready;
        ResetAll.IsEnabled = ready;
        InvertXBox.IsEnabled = enabled;
        InvertYBox.IsEnabled = enabled;

        DisabledReason.Text = view.Availability.DisabledReason ?? string.Empty;
        DisabledReason.Visibility = view.Availability.DisabledReason is null
            ? Visibility.Collapsed
            : Visibility.Visible;

        MappingSummary.Text = view.Loaded
            ? $"{view.BoundCount} of {view.MappableCount} inputs mapped"
            : "Not read yet";

        // Read it, press one key, read it again: the useful signal is the
        // difference, so the whole block stays visible once anything has loaded.
        CountersExpander.Visibility = view.Counters is null
            ? Visibility.Collapsed
            : Visibility.Visible;
        CountersText.Text = view.Counters?.Text ?? string.Empty;

        RenderProfileSelector(view);

        InactiveProfileBar.IsOpen = view.Loaded && view.EditingInactiveProfile;
        InactiveProfileBar.Message = view.InactiveProfileWarning ?? string.Empty;

        InvertXBox.IsChecked = view.InvertX;
        InvertYBox.IsChecked = view.InvertY;

        BuildKeyboard(view, enabled);
        BuildMouseButtons(view, enabled);
        BuildSliders(view, enabled);

        UndrawnSection.Visibility = view.Undrawn.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        UndrawnList.ItemsSource = view.Undrawn;
    }

    /// <summary>
    /// Draw each cluster on its own canvas, positioned from the layout's geometry.
    ///
    /// A canvas rather than stacked rows because the layout has gaps, half-unit
    /// offsets and two-unit-tall keys, none of which a row of buttons can express.
    /// The three clusters share row origins, so they line up without this file
    /// knowing anything about where a numpad goes.
    /// </summary>
    private void BuildKeyboard(KeyboardMouseView current, bool enabled)
    {
        KeyboardHost.Children.Clear();
        foreach (var cluster in current.Clusters)
        {
            KeyboardHost.Children.Add(ClusterCanvas(cluster, enabled));
        }

        OtherKeysHost.Children.Clear();
        foreach (var cell in current.OtherKeys.Cells)
        {
            OtherKeysHost.Children.Add(KeyButton(cell, enabled));
        }
    }

    private Canvas ClusterCanvas(KeyClusterCells cluster, bool enabled)
    {
        var canvas = new Canvas
        {
            Width = cluster.Columns * KeyUnit,
            Height = cluster.Rows * KeyUnit,
        };

        foreach (var cell in cluster.Cells)
        {
            var button = KeyButton(cell, enabled);

            // The gap is taken out of the key, not added between them, so a 2u key
            // still spans exactly two units of the grid.
            button.Width = (cell.Cap.Width * KeyUnit) - KeyGap;
            button.Height = (cell.Cap.Height * KeyUnit) - KeyGap;
            Canvas.SetLeft(button, cell.Cap.Column * KeyUnit);
            Canvas.SetTop(button, cell.Cap.Row * KeyUnit);
            canvas.Children.Add(button);
        }

        return canvas;
    }

    private void BuildMouseButtons(KeyboardMouseView current, bool enabled)
    {
        // Hidden entirely in the keyboard-only profile: those bindings can never
        // fire there, and five dead controls invite a binding that silently does
        // nothing.
        MouseButtonSection.Visibility = current.ShowMouseButtons
            ? Visibility.Visible
            : Visibility.Collapsed;

        MouseButtonHost.Children.Clear();
        if (!current.ShowMouseButtons)
        {
            return;
        }

        foreach (var cell in current.MouseButtons)
        {
            MouseButtonHost.Children.Add(KeyButton(cell, enabled, width: cell.Cap.Width, mouse: true));
        }
    }

    private Button KeyButton(KeyBindingCell cell, bool enabled, double? width = null, bool mouse = false)
    {
        var caption = new StackPanel { Spacing = 0 };
        caption.Children.Add(new TextBlock
        {
            Text = cell.Cap.Label,
            HorizontalAlignment = HorizontalAlignment.Center,
        });

        // The binding is shown ON the key. A keyboard where you have to hover to
        // discover the mapping is a list with extra steps.
        if (cell.Bound)
        {
            caption.Children.Add(new TextBlock
            {
                Text = cell.DestinationLabel,
                FontSize = 10,
                HorizontalAlignment = HorizontalAlignment.Center,
                TextTrimming = TextTrimming.CharacterEllipsis,
            });
        }

        var button = new Button
        {
            Content = caption,
            Width = (width ?? cell.Cap.Width) * KeyUnit - KeyGap,
            Height = KeyUnit - KeyGap,
            Padding = new Thickness(2),
            IsEnabled = enabled,
            Tag = cell.Cap.Usage,
        };

        // Colour alone cannot carry "this one is changed" -- it fails high contrast
        // and it fails a screen reader. The accessible name carries the whole fact.
        AutomationProperties.SetName(button, cell.AccessibleName);
        ToolTipService.SetToolTip(button, cell.AccessibleName);

        if (cell.Overridden)
        {
            button.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        }

        var source = mouse
            ? new KbmSource(KbmSourceKind.MouseButton, cell.Cap.Usage)
            : new KbmSource(KbmSourceKind.Key, cell.Cap.Usage);
        button.Click += async (_, _) => await EditAsync(source, cell.Cap.Label);
        return button;
    }

    private void BuildSliders(KeyboardMouseView current, bool enabled)
    {
        SliderHost.Children.Clear();
        foreach (var model in current.MouseSliders)
        {
            var panel = new StackPanel { Spacing = 2 };
            panel.Children.Add(new TextBlock
            {
                Text = model.Label,
                Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
            });

            var slider = new Slider
            {
                Minimum = model.Minimum,
                Maximum = Math.Max(model.Maximum, model.Minimum + 1),
                Value = Math.Clamp(model.Value, model.Minimum, Math.Max(model.Maximum, model.Minimum + 1)),
                IsEnabled = enabled && model.Available,
                Width = 320,
            };
            AutomationProperties.SetName(slider, model.Label);

            // Debounced, not sent per tick.
            //
            // At drag rate a per-tick send would flood the single-flight session and
            // make the slider fight the readback correcting it. Committing on
            // pointer release would miss the keyboard and touch paths entirely, so
            // the settle timer is the one mechanism that covers every input method.
            var field = model.Field;
            slider.ValueChanged += (s, _) => ScheduleMouseCommit(field, (int)((Slider)s).Value);

            panel.Children.Add(slider);
            if (model.Detail is { } detail)
            {
                panel.Children.Add(new TextBlock
                {
                    Text = detail,
                    Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                    TextWrapping = TextWrapping.Wrap,
                });
            }

            SliderHost.Children.Add(panel);
        }
    }

    /// <summary>
    /// Send the last value a slider settled on, once it has settled.
    ///
    /// One timer for all three sliders: only one is ever being dragged, and a
    /// pending commit for a different field would be stale by definition.
    /// </summary>
    private void ScheduleMouseCommit(KbmMouseField field, int value)
    {
        pendingMouseField = field;
        pendingMouseValue = value;

        mouseCommit ??= CreateMouseCommitTimer();
        mouseCommit.Stop();
        mouseCommit.Start();
    }

    private DispatcherTimer CreateMouseCommitTimer()
    {
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(SliderSettleMillis) };
        timer.Tick += async (_, _) =>
        {
            timer.Stop();
            if (pendingMouseField is { } field)
            {
                pendingMouseField = null;
                await SafeAsync(() => adapters.SetMouseAsync(field, pendingMouseValue));
            }
        };

        return timer;
    }

    /* --------------------------------------------------------------- plumbing */

    private async Task<bool> ConfirmAsync(string title, string content, string action) =>
        await new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = title,
            Content = content,
            PrimaryButtonText = action,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        }.ShowAsync() == ContentDialogResult.Primary;

    private void Report(string message, InfoBarSeverity severity)
    {
        OperationBar.Severity = severity;
        OperationBar.Message = message;
        OperationBar.IsOpen = true;
    }

    /// <summary>
    /// Ask for a profile name. Bounded to what the adapter can store, so a name
    /// cannot be silently truncated after the user typed it.
    /// </summary>
    private async Task<string?> PromptAsync(string title, string description,
                                            string initial)
    {
        var box = new TextBox
        {
            Text = initial,
            MaxLength = KbmProfileNameMax,
            SelectionStart = initial.Length,
        };
        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(new TextBlock
        {
            Text = description,
            TextWrapping = TextWrapping.Wrap,
        });
        content.Children.Add(box);

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = title,
            Content = content,
            PrimaryButtonText = "OK",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary
            ? box.Text.Trim()
            : null;
    }

    /// <summary>Matches NS2_KBM_PROFILE_NAME_MAX minus its NUL.</summary>
    private const int KbmProfileNameMax = 19;

    private async Task SafeAsync(Func<Task> operation)
    {
        OperationBar.IsOpen = false;
        try
        {
            await operation();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            adapters.Diagnostics.Error("ui", message);
            Report(message, InfoBarSeverity.Error);
        }
        finally
        {
            Render();
        }
    }

    /// <summary>
    /// The same, for an operation with a result. Returns default on failure, so
    /// a caller can abandon a multi-step flow rather than continuing on a value
    /// it never received.
    /// </summary>
    private async Task<T?> SafeAsync<T>(Func<Task<T>> operation)
    {
        OperationBar.IsOpen = false;
        try
        {
            return await operation();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            adapters.Diagnostics.Error("ui", message);
            Report(message, InfoBarSeverity.Error);
            return default;
        }
        finally
        {
            Render();
        }
    }
}
