using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Diagnostics;
using PicoSwitch.Management;
using Windows.ApplicationModel.DataTransfer;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// The live diagnostic stream, the support bundle, and the two low-level controls
/// that belong nowhere else (§16.5).
///
/// Raw LE bond slots and the management gate live HERE rather than on a routine
/// page, and that placement is the point:
///
/// - bond slots are an LE-only, slot-addressed view of one credential store, and
///   letting them drive Paired Controllers is the exact defect Bluetooth
///   Management 2.0 exists to prevent;
/// - turning the management gate off is one-way from the app's side. It would
///   need this very channel to turn it back on.
/// </summary>
public sealed partial class DiagnosticsPage : Page
{
    private readonly DiagnosticLog log = AppServices.Diagnostics;
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    public DiagnosticsPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        log.Recorded += OnRecorded;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Connection.Changed += OnStateChanged;
        Render();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        log.Recorded -= OnRecorded;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Connection.Changed -= OnStateChanged;
    }

    // Entries are recorded from device callback threads.
    private void OnRecorded(DiagnosticEntry entry) => AppServices.OnUiThread(Render);

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    private void OnCopy(object sender, RoutedEventArgs e) => Copy(log.Render(), "Log copied.");

    private void OnCopyBundle(object sender, RoutedEventArgs e) => Copy(
        SupportBundle.Render(
            log,
            adapters.Snapshot.Value,
            adapters.Registry.Value,
            adapters.Relationship.Value,
            adapters.Radio.Value,
            adapters.Connection.Value,
            AppServices.ControllerLinkHostLog()),
        "Support bundle copied. Addresses are shortened and nicknames are excluded.");

    private void OnClear(object sender, RoutedEventArgs e)
    {
        log.Clear();
        Render();
    }

    private async void OnReadBonds(object sender, RoutedEventArgs e) =>
        await SafeAsync(async () =>
        {
            var bonds = await adapters.RefreshBondDiagnosticsAsync();

            // An incomplete enumeration is stated, not smoothed over: a partial
            // list read as a full one is how a bond that still exists looks gone.
            var header = bonds.Complete
                ? $"{bonds.Entries.Count} of {bonds.Total?.ToString() ?? "?"} slots"
                : $"{bonds.Entries.Count} slots — INCOMPLETE, the adapter could not report them all";

            BondsText.Text = bonds.Entries.Count == 0
                ? $"{header}\nNo LE bonds stored."
                : header + Environment.NewLine + string.Join(
                    Environment.NewLine,
                    bonds.Entries.Select(bond => $"  [{bond.Index}] {bond.Address}"));
        });

    private async void OnDisableGate(object sender, RoutedEventArgs e)
    {
        // The confirmation has to say that the app cannot undo this, because the
        // obvious assumption -- that a toggle can be toggled back -- is wrong here
        // and the consequence is losing the app entirely.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Turn off management over Bluetooth?",
            Content =
                "This app will disconnect and will not be able to reach the adapter again.\n\n" +
                "You can only turn it back on over a USB serial connection to the adapter, or " +
                "with its physical pairing gesture. There is no way back from inside this app.",
            PrimaryButtonText = "Turn it off",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
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
                InfoBarSeverity.Warning);
        });
    }

    private void Render()
    {
        var entries = log.Snapshot();
        Summary.Text = $"{entries.Count} entries, {log.Dropped} dropped";
        LogText.Text = string.Join(
            Environment.NewLine,
            entries.Select(entry => entry.ToString()));
        LogScroller.ChangeView(null, LogScroller.ScrollableHeight, null, disableAnimation: true);

        var connected = adapters.Connection.Value.Connected;
        BondsRefresh.IsEnabled = connected;
        GateDisable.IsEnabled = connected;

        // Unknown is a real answer: an adapter that has not been asked, or whose
        // firmware has no gate command, is not "enabled".
        GateState.Text = adapters.Snapshot.Value.ManagementEnabled switch
        {
            true => "Management over Bluetooth is on.",
            false => "Management over Bluetooth is off.",
            _ => "Not known — refresh a connected adapter to read it.",
        };
    }

    private void Copy(string text, string confirmation)
    {
        var package = new DataPackage();
        package.SetText(text);
        Clipboard.SetContent(package);
        Report(confirmation, InfoBarSeverity.Success);
    }

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
            log.Error("ui", message);
            Report(message, InfoBarSeverity.Error);
        }
    }
}
