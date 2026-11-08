using Avalonia.Controls;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// Pair a controller to the ADAPTER. A port of the WinUI ControllerPairingDialog
/// and bound by its two rules:
///
/// - The adapter owns the pairing window. `pairing start` opens a window the
///   firmware closes on its own timer; this is a VIEW of it. Closing the panel
///   leaves the window open until it expires — the app must not imply it can
///   revoke what it did not grant. Cancel is a courtesy.
/// - Generation pinning. Every poll is checked against the `op` this panel
///   started with; a reply about another attempt must not repaint it.
/// </summary>
internal sealed class ControllerPairingPanel
{
    private const int PollIntervalMillis = 1000;

    private readonly AdapterConnectionService adapters;
    private readonly DialogHost dialogs;
    private readonly TextBlock headline = Ui.Text();
    private readonly TextBlock detail = Ui.Text(secondary: true);
    private readonly TextBlock remaining = Ui.Text(secondary: true);
    private readonly ProgressBar progress = new() { IsIndeterminate = true };
    private readonly Button cancel;

    private long operation = -1;
    /// <summary>Set from the dialog's completion continuation, read by the poll loop.</summary>
    private volatile bool closed;

    public ControllerPairingPanel(AdapterConnectionService adapters, DialogHost dialogs)
    {
        this.adapters = adapters;
        this.dialogs = dialogs;

        // Inside the content, not a dialog button: cancelling must NOT dismiss,
        // because the user still wants to see the outcome of the cancellation.
        cancel = Ui.Button("Cancel pairing", async (_, _) => await OnCancelAsync());
    }

    public async Task RunAsync()
    {
        var showing = dialogs.ShowAsync(
            "Pair a controller",
            Ui.Stack(headline, detail, remaining, progress, Ui.Row(cancel)),
            ["Close"]);
        _ = showing.ContinueWith(_ => closed = true, TaskScheduler.Default);

        Paint("Starting pairing…", null, string.Empty, busy: true);
        try
        {
            await PollAsync();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            Paint("Pairing could not start", message, string.Empty, busy: false);
            adapters.Diagnostics.Error("ui", message);
        }

        await showing;
    }

    private async Task PollAsync()
    {
        var started = await adapters.StartControllerPairingAsync();
        operation = started.Operation;
        Apply(RemotePairing.Project(started));

        while (started.Active && !closed)
        {
            await Task.Delay(PollIntervalMillis);

            PairingStatus status;
            try
            {
                status = await adapters.ControllerPairingStatusAsync();
            }
            catch (Exception error)
            {
                // A lost session ends the VIEW, not the adapter's window.
                Paint(
                    "Lost contact with the adapter",
                    "Its pairing window may still be open. " + ManagementErrorText.Summarize(error),
                    string.Empty,
                    busy: false);
                return;
            }

            if (status.Operation != operation)
            {
                Paint("Pairing was replaced by another attempt", null, string.Empty, busy: false);
                return;
            }

            var view = RemotePairing.Project(status);
            Apply(view);
            if (!view.Active)
            {
                return;
            }
        }
    }

    private async Task OnCancelAsync()
    {
        try
        {
            Apply(RemotePairing.Project(await adapters.CancelControllerPairingAsync()));
        }
        catch (Exception error)
        {
            adapters.Diagnostics.Warn("ui", $"cancel pairing failed: {error.Message}");
        }
    }

    private void Apply(RemotePairingView view)
    {
        Paint(
            view.Headline,
            view.PointsToForget ? view.Detail + " Close this and use Forget on a controller below." : view.Detail,
            view.RemainingText,
            view.Active);
        cancel.IsEnabled = view.CanCancel;
    }

    private void Paint(string headlineText, string? detailText, string remainingText, bool busy)
    {
        headline.Text = headlineText;
        Ui.SetOptional(detail, detailText);
        Ui.SetOptional(remaining, remainingText);
        progress.IsVisible = busy;
    }
}
