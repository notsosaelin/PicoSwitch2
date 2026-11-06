using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using PicoSwitch.Bridge.Core;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;
using Windows.UI;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// The adapter dashboard (§16.1). Phase 3's MVP surface.
///
/// The page paints <see cref="AdapterDashboardView"/> and calls
/// <see cref="AdapterConnectionService"/>. It makes no decisions of its own: every
/// "is this enabled", "what does this say" and "should this warn" is decided in
/// <see cref="AdapterDashboard"/>, where it is unit-tested. A rule that appears in
/// this file is a rule in the wrong place — the Phase 2 version of this page grew
/// its logic inline and none of it could be tested.
///
/// It calls the service and nothing below it — no GATT object, no management
/// command string.
/// </summary>
public sealed partial class AdapterPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    /// <summary>Input source ids, positionally matched to <c>InputBox</c>.</summary>
    private readonly List<long> inputIds = [];

    private readonly List<Personality> personalities = [];

    private AdapterDashboardView? view;

    public AdapterPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;
        adapters.Radio.Changed += OnStateChanged;

        // The console buttons are gated on Controller Link streaming, which this
        // page does not otherwise observe — without this they would stay disabled
        // until some unrelated adapter event happened to repaint.
        AppServices.ControllerLink.View.Changed += OnStateChanged;

        Render();
        await SafeAsync(() => adapters.ProbeRadioAsync());
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        // Page-scoped subscriptions end with the page. The SESSION does not:
        // navigating away must never tear down a live BLE connection.
        adapters.Connection.Changed -= OnStateChanged;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Registry.Changed -= OnStateChanged;
        adapters.Relationship.Changed -= OnStateChanged;
        adapters.Radio.Changed -= OnStateChanged;
        AppServices.ControllerLink.View.Changed -= OnStateChanged;
    }

    // Every observable this page watches is written from a WinRT pool thread.
    private void OnStateChanged() => AppServices.OnUiThread(Render);

    /* ------------------------------------------------------------- actions */

    private async void OnConnect(object sender, RoutedEventArgs e)
    {
        if (SelectedAdapter() is not { } record)
        {
            return;
        }

        await SafeAsync(() => adapters.ConnectAsync(record.Id));
    }

    private async void OnDisconnect(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.DisconnectAsync());

    private async void OnRefresh(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.RefreshAsync());

    private async void OnWake(object sender, RoutedEventArgs e) =>
        await SafeAsync(async () =>
        {
            var status = await adapters.WakeConsoleAsync();

            // Unknown means the adapter could not tell us -- older firmware has no
            // `wake status`. Reporting that as failure would be a lie about the
            // console, so it is reported as exactly what it is.
            Report(
                status.Result switch
                {
                    WakeResult.ConsoleAwake => "The console is awake.",
                    WakeResult.Advertised => "Wake sent. The console should wake shortly.",
                    WakeResult.NoIdentity =>
                        "The adapter has no saved console identity, so it cannot wake one. " +
                        "Connect it to the console once first.",
                    WakeResult.RadioBusy =>
                        "The adapter's radio was busy. Try again in a moment.",
                    WakeResult.Unknown =>
                        "Wake was sent, but this adapter's firmware cannot report the result.",
                    _ => "The adapter is still trying to wake the console.",
                },
                status.Result is WakeResult.ConsoleAwake or WakeResult.Advertised
                    ? InfoBarSeverity.Success
                    : InfoBarSeverity.Informational);
        });

    private async void OnSwitchPersonality(object sender, RoutedEventArgs e)
    {
        if (PersonalityBox.SelectedIndex < 0 || PersonalityBox.SelectedIndex >= personalities.Count)
        {
            return;
        }

        var personality = personalities[PersonalityBox.SelectedIndex];
        await SafeAsync(async () =>
        {
            var outcome = await adapters.SetPersonalityAsync(personality);

            // Three outcomes, three different things to say. "Switched, but the
            // console has not seen it" needs its own instruction: the user is
            // looking at a console that still shows the old controller.
            if (outcome.Unchanged)
            {
                Report("The adapter was already in that mode.", InfoBarSeverity.Informational);
            }
            else if (outcome.Reenumerated)
            {
                Report(
                    $"Switched to {ControllerModeSection.Label(outcome.Personality)}.",
                    InfoBarSeverity.Success);
            }
            else
            {
                Report(
                    $"The adapter switched to {ControllerModeSection.Label(outcome.Personality)}, " +
                    "but the console has not seen the change yet. Unplug the adapter and plug it " +
                    "back in.",
                    InfoBarSeverity.Warning);
            }
        });
    }

    private async void OnEditBody(object sender, RoutedEventArgs e) =>
        await EditColorAsync(ColorTarget.Body, view?.Appearance.Body);

    private async void OnEditLeft(object sender, RoutedEventArgs e) =>
        await EditColorAsync(ColorTarget.LeftAccent, view?.Appearance.LeftAccent);

    private async void OnEditRight(object sender, RoutedEventArgs e) =>
        await EditColorAsync(ColorTarget.RightAccent, view?.Appearance.RightAccent);

    private async Task EditColorAsync(ColorTarget target, RgbColor? current)
    {
        var picker = new ColorPicker
        {
            IsAlphaEnabled = false,
            IsColorSliderVisible = true,
            IsHexInputVisible = true,
            Color = current is { } value
                ? Color.FromArgb(255, (byte)value.Red, (byte)value.Green, (byte)value.Blue)
                : Colors.Black,
        };

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Choose a colour",
            Content = picker,
            PrimaryButtonText = "Set",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        var chosen = picker.Color;
        await SafeAsync(() =>
            adapters.SetColorAsync(target, new RgbColor(chosen.R, chosen.G, chosen.B)));
    }

    private async void OnApplyAppearance(object sender, RoutedEventArgs e) =>
        await SafeAsync(async () =>
        {
            await adapters.ApplyAppearanceAsync();
            Report("The console now sees the new colours.", InfoBarSeverity.Success);
        });

    private async void OnSetInput(object sender, RoutedEventArgs e)
    {
        if (InputBox.SelectedIndex < 0 || InputBox.SelectedIndex >= inputIds.Count)
        {
            return;
        }

        var id = inputIds[InputBox.SelectedIndex];
        await SafeAsync(() => adapters.SetActiveInputAsync(id));
    }

    /* ------------------------------------------------------------- painting */

    private AdapterRecord? SelectedAdapter()
    {
        var registry = adapters.Registry.Value;
        return registry.ActiveId is { } id
            ? registry.Record(id)
            : registry.Records.FirstOrDefault();
    }

    private void Render()
    {
        var selected = SelectedAdapter();

        view = AdapterDashboard.Project(
            adapters.Snapshot.Value,
            adapters.Relationship.Value,
            adapters.Connection.Value,
            selected,
            AppServices.ControllerLinkStreaming);

        var radio = adapters.Radio.Value;
        RadioBar.IsOpen = radio.ManagementBlockedReason is not null;
        RadioBar.Message = radio.ManagementBlockedReason ?? string.Empty;

        // The silence rule, expressed as one assignment.
        ContractBar.IsOpen = view.Contract.Visible;
        ContractBar.Message = view.Contract.Summary;

        AdapterTitle.Text = view.Title;
        PhaseText.Text = view.PhaseText;
        SetOptionalText(PhaseDetail, view.PhaseDetail);
        SetOptionalText(FirmwareText, view.FirmwareLine);

        ControllerText.Text = view.ControllerLine;
        SetOptionalText(BatteryText, view.BatteryLine);

        ConnectButton.IsEnabled = selected is not null && !view.Connected;
        DisconnectButton.IsEnabled = view.Connected;
        RefreshButton.IsEnabled = view.Connected;
        WakeButton.IsEnabled = view.Connected;
        NoAdapterHint.Visibility = selected is null ? Visibility.Visible : Visibility.Collapsed;

        RenderPersonality(view.ControllerMode);
        RenderAppearance(view.Appearance);
        RenderInput(view.ConsoleInput);

        ConsoleButtonsReason.Text = view.ConsoleButtons.DisabledReason ?? string.Empty;
        HomeButton.IsEnabled = view.ConsoleButtons.Enabled;
        CaptureButton.IsEnabled = view.ConsoleButtons.Enabled;
        CButton.IsEnabled = view.ConsoleButtons.Enabled;
    }

    // Home / Capture / C are pressed BY this PC through Controller Link, so they
    // go through the same input session everything else does rather than a
    // side-channel to the adapter: one thing decides what the console sees.
    private async void OnConsoleHome(object sender, RoutedEventArgs e) =>
        await AppServices.TapConsoleButtonAsync(ControllerButton.Home);

    private async void OnConsoleCapture(object sender, RoutedEventArgs e) =>
        await AppServices.TapConsoleButtonAsync(ControllerButton.Capture);

    private async void OnConsoleC(object sender, RoutedEventArgs e) =>
        await AppServices.TapConsoleButtonAsync(ControllerButton.C);

    private void RenderPersonality(ControllerModeSection section)
    {
        personalities.Clear();
        personalities.AddRange(section.Available);

        PersonalityBox.Items.Clear();
        foreach (var personality in personalities)
        {
            PersonalityBox.Items.Add(ControllerModeSection.Label(personality));
        }

        PersonalityBox.SelectedIndex = personalities.IndexOf(section.Current);
        Gate(section.Availability, PersonalityReason, PersonalityBox, PersonalityApply);
    }

    private void RenderAppearance(AppearanceSection section)
    {
        AppearanceHint.Text = section.ApplyHint;
        Paint(BodySwatch, section.Body);
        Paint(LeftSwatch, section.LeftAccent);
        Paint(RightSwatch, section.RightAccent);
        Gate(section.Availability, AppearanceReason, BodyEdit, LeftEdit, RightEdit, AppearanceApply);
    }

    private void RenderInput(ConsoleInputSection section)
    {
        inputIds.Clear();
        InputBox.Items.Clear();
        foreach (var source in section.Sources)
        {
            inputIds.Add(source.Id);
            InputBox.Items.Add(source.IsActive ? $"{source.Label} (in use)" : source.Label);
        }

        InputBox.SelectedIndex = inputIds.IndexOf(section.ActiveId);
        InputTruncated.IsOpen = section.Truncated;
        Gate(section.Availability, InputReason, InputBox, InputApply);
    }

    /// <summary>
    /// Enable or disable a section, and show its reason when disabled.
    ///
    /// A disabled control with no reason is the worst of both worlds: the user can
    /// neither act nor find out why. The reason itself is decided in the
    /// projection, not here.
    /// </summary>
    private static void Gate(
        SectionAvailability availability,
        TextBlock reason,
        params Control[] controls)
    {
        foreach (var control in controls)
        {
            control.IsEnabled = availability.Enabled;
        }

        SetOptionalText(reason, availability.DisabledReason);
    }

    private static void Paint(Border swatch, RgbColor color) =>
        swatch.Background = new SolidColorBrush(
            Color.FromArgb(255, (byte)color.Red, (byte)color.Green, (byte)color.Blue));

    private static void SetOptionalText(TextBlock block, string? text)
    {
        block.Text = text ?? string.Empty;
        block.Visibility = string.IsNullOrWhiteSpace(text) ? Visibility.Collapsed : Visibility.Visible;
    }

    private void Report(string message, InfoBarSeverity severity)
    {
        OperationBar.Severity = severity;
        OperationBar.Message = message;
        OperationBar.IsOpen = true;
    }

    /// <summary>
    /// Run one adapter operation, reporting a failure instead of crashing the page.
    ///
    /// Every error is logged before it is rendered, and the text shown is the one
    /// the service produced — the layers below already word their failures for a
    /// person ("Bluetooth is turned off", "Repair pairing to continue"), and
    /// replacing them with "Something went wrong" would throw that away.
    /// </summary>
    private async Task SafeAsync(Func<Task> operation)
    {
        BusyRing.IsActive = true;
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
            BusyRing.IsActive = false;
            Render();
        }
    }
}
