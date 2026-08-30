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

    private KbmProfile profile = KbmProfile.Keyboard;

    /// <summary>
    /// Whether the user has chosen which profile to edit.
    ///
    /// Until they do, the page follows the adapter's ACTIVE profile. Defaulting
    /// to Keyboard regardless meant a user could bind a key, see every operation
    /// report success, and get nothing at the console -- because the adapter was
    /// resolving the other profile the whole time.
    /// </summary>
    private bool profileChosenByUser;
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

    private void OnProfileChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressSelection || ProfileBox.SelectedIndex < 0)
        {
            return;
        }

        profile = (KbmProfile)ProfileBox.SelectedIndex;
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
        if (result == ContentDialogResult.Secondary)
        {
            // A null destination is the protocol's `default`.
            await SafeAsync(() => adapters.BindAsync(profile, source, destination: null));
            return;
        }

        if (result != ContentDialogResult.Primary || list.SelectedIndex < 0)
        {
            return;
        }

        await SafeAsync(() => adapters.BindAsync(profile, source, destinations[list.SelectedIndex]));
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

        view = KeyboardMouse.Project(state, profile, connected);

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
}
