using Avalonia.Controls;
using Avalonia.Media;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Diagnostics;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// The log, the support bundle, the adapter's bond table, and the counters
/// that say where input stopped: SDL frames in, Controller Link frames out,
/// frames carrying motion. A port of the WinUI DiagnosticsPage plus those
/// counters, which matter more here because the motion path is new.
/// </summary>
internal sealed class DiagnosticsPage : CompanionPage
{
    private const int ShownEntries = 400;

    private readonly AdapterConnectionService adapters = AppServices.Adapters;
    private readonly DiagnosticLog log = AppServices.Diagnostics;

    private readonly TextBlock summary = Ui.Caption();
    private readonly TextBlock counters = Ui.Mono();
    private readonly TextBlock bondsText = Ui.Mono();
    private readonly TextBox logText = new()
    {
        IsReadOnly = true,
        AcceptsReturn = true,
        TextWrapping = TextWrapping.NoWrap,
        FontFamily = Ui.MonoFont,
        FontSize = 12,
        Height = 320,
    };

    public DiagnosticsPage(MainWindow shell)
        : base(shell)
    {
        var back = Ui.Button("Settings", (_, _) => Shell.Navigate("settings"), "Back to Settings", icon: Icons.Back);
        back.HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left;
        back.Background = Brushes.Transparent;

        Content = Ui.Page(
            Ui.Stack(4, back, Ui.PageHeader(Ui.Title("Diagnostics"), Ui.Text("Where input stopped, what the app logged, and what the adapter stores.", secondary: true))),
            Operation,
            Ui.Section(
                "Input path",
                "SDL frames in, Controller Link frames out. Read on demand.",
                Ui.Button("Update", (_, _) => RenderCounters(), icon: Icons.Refresh),
                Ui.Body(counters)),
            Ui.Section(
                Ui.Heading("Log"),
                summary,
                Ui.Row(
                    Ui.Button("Copy log", async (_, _) => await CopyAsync(log.Render(), "Log copied.")),
                    Ui.Button("Copy support bundle", async (_, _) => await CopyAsync(
                        SupportBundle.Render(
                            log,
                            adapters.Snapshot.Value,
                            adapters.Registry.Value,
                            adapters.Relationship.Value,
                            adapters.Radio.Value,
                            adapters.Connection.Value),
                        "Support bundle copied. Addresses are shortened and nicknames are excluded.")),
                    Ui.Button("Clear", (_, _) =>
                    {
                        log.Clear();
                        Render();
                    })),
                Ui.Body(logText)),
            Ui.Section(
                "Adapter bonds",
                "The LE bonds stored on the adapter itself.",
                Ui.Row(Busy, Ui.Button("Read bonds", async (_, _) => await OnReadBondsAsync())),
                Ui.Body(bondsText)),
            Ui.Section(
                (string?)null,
                null,
                null,
                Ui.Setting(
                    "Management over Bluetooth",
                    "Turning this off disconnects this app for good. It can only be turned back on " +
                    "over USB serial or with the adapter's pairing gesture.",
                    Ui.Button("Turn off…", async (_, _) => await OnDisableGateAsync()))));
    }

    protected override void OnShown()
    {
        log.Recorded += OnRecorded;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Connection.Changed += OnStateChanged;
        Render();
    }

    protected override void OnHidden()
    {
        log.Recorded -= OnRecorded;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;
    }

    private void OnRecorded(DiagnosticEntry entry) => AppServices.OnUiThread(Render);

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    private Task OnReadBondsAsync() => SafeAsync(async () =>
    {
        var bonds = await adapters.RefreshBondDiagnosticsAsync();

        // An incomplete enumeration is stated, not smoothed over: a partial list
        // read as a full one is how a bond that still exists looks gone.
        var header = bonds.Complete
            ? $"{bonds.Entries.Count} of {bonds.Total?.ToString() ?? "?"} slots"
            : $"{bonds.Entries.Count} slots — INCOMPLETE, the adapter could not report them all";
        ((Control)bondsText.Parent!).IsVisible = true;
        bondsText.Text = bonds.Entries.Count == 0
            ? $"{header}\nNo LE bonds stored."
            : header + Environment.NewLine + string.Join(
                Environment.NewLine, bonds.Entries.Select(bond => $"  [{bond.Index}] {bond.Address}"));
    });

    private async Task OnDisableGateAsync()
    {
        // Says that the app cannot undo this: the natural assumption that a
        // toggle can be toggled back is wrong here.
        if (!await Dialogs.ConfirmAsync(
                "Turn off management over Bluetooth?",
                "This app will disconnect and will not be able to reach the adapter again.\n\n" +
                "You can only turn it back on over a USB serial connection to the adapter, or " +
                "with its physical pairing gesture. There is no way back from inside this app.",
                "Turn it off"))
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var enabled = await adapters.SetManagementEnabledAsync(false);
            Report(
                enabled == false
                    ? "Management over Bluetooth is now off. This app can no longer reach the adapter."
                    : "The adapter did not confirm the change.",
                NoticeSeverity.Warning);
        });
    }

    private void Render()
    {
        if (bondsText.Parent is Control bondsHost)
        {
            bondsHost.IsVisible = !string.IsNullOrEmpty(bondsText.Text);
        }

        var entries = log.Snapshot();
        summary.Text = $"{entries.Count} entries, {log.Dropped} dropped";
        logText.Text = string.Join(Environment.NewLine, entries.TakeLast(ShownEntries).Select(entry => entry.ToString()));
        RenderCounters();
    }

    /// <summary>
    /// Boundary counters, read on demand rather than streamed: a live-updating
    /// counter at report rate would itself be the high-frequency noise the
    /// diagnostics rules warn against.
    /// </summary>
    private void RenderCounters()
    {
        var gamepad = AppServices.PhysicalGamepad;
        var source = gamepad.SelectedSource;
        counters.Text =
            $"source        {(source is null ? "none" : $"{source.Name} ({source.Candidate.VendorId:x4}:{source.Candidate.ProductId:x4})")}\n" +
            $"motion        {(gamepad.Available ? "available" : "not available")}\n" +
            $"sdl frames    {gamepad.FramesPublished}\n" +
            $"sdl motion    {gamepad.MotionSamples}\n" +
            $"link          {AppServices.ControllerLink.Metrics.Summary()}";
    }

    private async Task CopyAsync(string text, string confirmation)
    {
        if (TopLevel.GetTopLevel(this)?.Clipboard is { } clipboard)
        {
            await clipboard.SetTextAsync(text);
            Report(confirmation, NoticeSeverity.Success);
        }
        else
        {
            Report("The clipboard is not available here.", NoticeSeverity.Warning);
        }
    }
}
