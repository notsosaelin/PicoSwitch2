using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services.Diagnostics;
using Windows.ApplicationModel.DataTransfer;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// The live diagnostic stream.
///
/// Phase 7 adds the boundary counters, the radio capability block and the
/// redacted support bundle. What exists here is the part Phase 2 needs to be
/// judgeable on hardware: what the transport, the lifecycle and the app actually
/// did, in order, with the failures named by the layer that produced them.
/// </summary>
public sealed partial class DiagnosticsPage : Page
{
    private readonly DiagnosticLog log = AppServices.Diagnostics;

    public DiagnosticsPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        log.Recorded += OnRecorded;
        Render();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) => log.Recorded -= OnRecorded;

    // Entries are recorded from device callback threads.
    private void OnRecorded(DiagnosticEntry entry) => AppServices.OnUiThread(Render);

    private void OnCopy(object sender, RoutedEventArgs e)
    {
        var package = new DataPackage();
        package.SetText(log.Render());
        Clipboard.SetContent(package);
    }

    private void OnClear(object sender, RoutedEventArgs e)
    {
        log.Clear();
        Render();
    }

    private void Render()
    {
        var entries = log.Snapshot();
        Summary.Text = $"{entries.Count} entries, {log.Dropped} dropped";
        LogText.Text = string.Join(
            Environment.NewLine,
            entries.Select(entry => entry.ToString()));
        LogScroller.ChangeView(null, LogScroller.ScrollableHeight, null, disableAnimation: true);
    }
}
