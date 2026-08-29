using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Pair a controller to the ADAPTER, over a live management session (§16.2).
///
/// ## What owns the pairing window
///
/// The adapter does. `pairing start` opens a window the firmware closes on its
/// own timer, so this dialog is a VIEW of that operation rather than the thing
/// performing it. Cancel is a courtesy: closing the dialog without pressing it
/// leaves the adapter's window open until it expires, which is correct — the app
/// must not imply it can revoke an authorization it did not grant.
///
/// ## Generation pinning
///
/// Every poll is checked against the <c>op</c> generation this dialog started
/// with. A reply from a previous attempt, or from an adapter the user has since
/// switched away from, must not repaint this view — the same discipline the GATT
/// transport applies to callbacks, applied to a long-running protocol operation.
/// </summary>
public sealed class ControllerPairingDialog
{
    private const int PollIntervalMillis = 1000;

    private readonly AdapterConnectionService adapters;
    private readonly ContentDialog dialog;
    private readonly TextBlock headline = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock detail = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock remaining = new();
    private readonly ProgressBar progress = new() { IsIndeterminate = true };

    private long operation = -1;

    public ControllerPairingDialog(AdapterConnectionService adapters)
    {
        this.adapters = adapters;

        detail.Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current
            .Resources["TextFillColorSecondaryBrush"];
        remaining.Foreground = detail.Foreground;

        dialog = new ContentDialog
        {
            Title = "Pair a controller",
            Content = new StackPanel
            {
                Spacing = 8,
                Children = { headline, detail, remaining, progress },
            },
            PrimaryButtonText = "Cancel pairing",
            CloseButtonText = "Close",
            DefaultButton = ContentDialogButton.Close,
        };

        // Cancel must not dismiss the dialog: the user still wants to see the
        // outcome of the cancellation.
        dialog.PrimaryButtonClick += OnCancelClicked;
    }

    public XamlRoot? XamlRoot
    {
        get => dialog.XamlRoot;
        init => dialog.XamlRoot = value;
    }

    public async Task RunAsync()
    {
        var showing = dialog.ShowAsync();
        try
        {
            await PollAsync();
        }
        catch (Exception error)
        {
            Paint(
                "Pairing could not start",
                ManagementErrorText.Summarize(error),
                remainingText: string.Empty,
                busy: false);
            adapters.Diagnostics.Error("ui", ManagementErrorText.Summarize(error));
        }

        await showing;
    }

    private async Task PollAsync()
    {
        var started = await adapters.StartControllerPairingAsync();
        operation = started.Operation;
        Apply(RemotePairing.Project(started));

        while (started.Active)
        {
            await Task.Delay(PollIntervalMillis);

            PairingStatus status;
            try
            {
                status = await adapters.ControllerPairingStatusAsync();
            }
            catch (Exception error)
            {
                // A lost session ends the VIEW, not the adapter's window. Say so
                // rather than implying the pairing was cancelled.
                Paint(
                    "Lost contact with the adapter",
                    "Its pairing window may still be open. " +
                    ManagementErrorText.Summarize(error),
                    string.Empty,
                    busy: false);
                return;
            }

            if (status.Operation != operation)
            {
                // A different operation is running. This dialog is stale and must
                // stop painting rather than describe someone else's attempt.
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

    private void Apply(RemotePairingView view)
    {
        Paint(view.Headline, Detail(view), view.RemainingText, view.Active);
        dialog.IsPrimaryButtonEnabled = view.CanCancel;
    }

    private static string? Detail(RemotePairingView view) =>
        view.PointsToForget
            ? view.Detail + " Close this and use Forget on a controller below."
            : view.Detail;

    private void Paint(string headlineText, string? detailText, string remainingText, bool busy)
    {
        headline.Text = headlineText;
        detail.Text = detailText ?? string.Empty;
        detail.Visibility = string.IsNullOrWhiteSpace(detailText)
            ? Visibility.Collapsed
            : Visibility.Visible;
        remaining.Text = remainingText;
        remaining.Visibility = string.IsNullOrWhiteSpace(remainingText)
            ? Visibility.Collapsed
            : Visibility.Visible;
        progress.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
    }

    private async void OnCancelClicked(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        var deferral = args.GetDeferral();
        args.Cancel = true;
        try
        {
            Apply(RemotePairing.Project(await adapters.CancelControllerPairingAsync()));
        }
        catch (Exception error)
        {
            adapters.Diagnostics.Warn("ui", $"cancel pairing failed: {error.Message}");
        }
        finally
        {
            deferral.Complete();
        }
    }
}
