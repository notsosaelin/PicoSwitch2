using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.App.Pages;

/// <summary>
/// Settings: remembered adapters, Paired Controllers, theme, and the door to
/// Diagnostics (§16.2, §16.6).
///
/// Two management cards whose scopes must stay visibly different. **Remembered
/// adapters** are this PC's relationships with adapters. **Paired Controllers**
/// are the adapter's own relationships with controllers. Conflating them is the
/// mistake the copy on this page exists to prevent — they are different devices,
/// different credential stores, and different destructive operations.
/// </summary>
public sealed partial class SettingsPage : Page
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    private bool suppressThemeChange;

    public SettingsPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;

        suppressThemeChange = true;
        ThemeBox.SelectedIndex = (int)AppTheme.Current;
        suppressThemeChange = false;

        Render();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        adapters.Connection.Changed -= OnStateChanged;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Registry.Changed -= OnStateChanged;
        adapters.Relationship.Changed -= OnStateChanged;
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    /* ------------------------------------------------------------- adapters */

    private async void OnPair(object sender, RoutedEventArgs e) =>
        await SafeAsync(() => adapters.PairNewAdapterAsync());

    private async void OnConnectRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is { } id)
        {
            await SafeAsync(() => adapters.ConnectAsync(id));
        }
    }

    private async void OnRenameRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is not { } id ||
            adapters.Registry.Value.Record(id) is not { } record)
        {
            return;
        }

        var input = new TextBox
        {
            Text = record.UserAlias ?? string.Empty,
            PlaceholderText = record.LastKnownName,
            MaxLength = 64,
        };

        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Rename adapter",
            Content = input,
            PrimaryButtonText = "Save",
            SecondaryButtonText = "Clear name",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.None)
        {
            return;
        }

        // A local nickname only. It never reaches the adapter and is never part of
        // its identity, which stays the Bluetooth address.
        var alias = result == ContentDialogResult.Primary ? input.Text : null;
        await SafeAsync(() => adapters.RenameAsync(id, alias));
    }

    private async void OnRepairRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is not { } id)
        {
            return;
        }

        // Unpairing destroys a trust relationship, so it is never automatic and
        // the confirmation names the consequence.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Repair pairing?",
            Content =
                "Windows drops its pairing with this adapter, then you pair again with " +
                "the adapter's pairing window open.\n\n" +
                "The adapter stays in this list, and its name and controller history are kept.",
            PrimaryButtonText = "Repair pairing",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() == ContentDialogResult.Primary)
        {
            await SafeAsync(() => adapters.RepairAsync(id));
        }
    }

    private async void OnRemoveRow(object sender, RoutedEventArgs e)
    {
        if (RowId(sender) is not { } id)
        {
            return;
        }

        // States both halves. This copy previously promised the Windows pairing
        // was untouched, which stopped being true when Remove was corrected to
        // match §16.2 — a confirmation that misdescribes its own action is worse
        // than none, because it is the one place the user is deciding.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Remove this adapter?",
            Content =
                "This app forgets the adapter and its controller history, and Windows " +
                "drops its pairing with it. You will need to pair again, with the " +
                "adapter's pairing window open.\n\n" +
                "The controllers paired to the adapter itself are not affected.",
            PrimaryButtonText = "Remove and unpair",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var removal = await adapters.RemoveAsync(id);
            if (removal.LeftOrphanPairing)
            {
                throw new ManagementException(
                    "The adapter was removed from this app, but Windows could not drop its " +
                    "pairing. Remove it from Windows Bluetooth settings as well.");
            }
        });
    }

    /* ---------------------------------------------------------- controllers */

    private async void OnForgetPeer(object sender, RoutedEventArgs e)
    {
        if (PeerIdOf(sender) is not { } peerId)
        {
            return;
        }

        // The confirmation says what happens to the controller AND what happens to
        // the row, because "it disappeared entirely" is the surprise otherwise.
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Forget this controller?",
            Content =
                "The adapter deletes its pairing with the controller. If the controller is " +
                "connected it will disconnect.\n\n" +
                "It stays in this list under Recent, so you can see it was here.",
            PrimaryButtonText = "Forget",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        await SafeAsync(async () =>
        {
            var outcome = await adapters.ForgetControllerAsync(peerId);

            // already_absent is an idempotent success. incomplete stays visible
            // rather than being smoothed over: the adapter is telling us it did
            // not finish, and hiding that produces a list nobody can trust.
            if (outcome.Result == PeerForgetResult.Incomplete)
            {
                Report(
                    "The adapter reported that it could not fully remove that controller. " +
                    "Refresh and check the list.",
                    InfoBarSeverity.Warning);
            }
        });
    }

    private async void OnRemoveHistory(object sender, RoutedEventArgs e)
    {
        if (PeerIdOf(sender) is { } peerId)
        {
            await SafeAsync(() => adapters.RemoveFromHistoryAsync(peerId));
        }
    }

    private async void OnPairController(object sender, RoutedEventArgs e) =>
        await new ControllerPairingDialog(adapters) { XamlRoot = XamlRoot }.RunAsync();

    /* ---------------------------------------------------------------- theme */

    private void OnThemeChanged(object sender, SelectionChangedEventArgs e)
    {
        if (suppressThemeChange || ThemeBox.SelectedIndex < 0)
        {
            return;
        }

        AppTheme.Apply((AppThemeChoice)ThemeBox.SelectedIndex);
    }

    private void OnOpenDiagnostics(object sender, RoutedEventArgs e) =>
        Frame.Navigate(typeof(DiagnosticsPage));

    /* ------------------------------------------------------------- painting */

    private void Render()
    {
        var rows = adapters.RememberedAdapters();
        AdapterList.ItemsSource = rows;
        AdaptersEmpty.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        var controllers = adapters.Controllers();
        PeersUnsupported.IsOpen = !controllers.Visible;
        PeersUnsupported.Message = controllers.HiddenReason ?? string.Empty;
        PeerSections.Visibility = controllers.Visible ? Visibility.Visible : Visibility.Collapsed;

        PairControllerButton.IsEnabled = controllers.CanPairNewController;
        PairControllerReason.Text = controllers.PairDisabledReason ?? string.Empty;
        PairControllerReason.Visibility = controllers.PairDisabledReason is null
            ? Visibility.Collapsed
            : Visibility.Visible;

        var inventory = controllers.Inventory;
        Fill(ConnectedSection, ConnectedList, inventory.Connected, controllers.CanForget);
        Fill(PairedSection, PairedList, inventory.Paired, controllers.CanForget);

        // Recent rows never offer Forget: there is no adapter credential left to
        // delete, only a local history row.
        Fill(RecentSection, RecentList, inventory.Recent, canForget: false);

        PeersEmpty.Visibility = inventory.HasControllers ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void Fill(
        StackPanel section,
        ItemsControl list,
        IReadOnlyList<PeerListing> listings,
        bool canForget)
    {
        section.Visibility = listings.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        list.ItemsSource = PeerRows.Project(listings, canForget);
    }

    private static AdapterId? RowId(object sender) =>
        sender is FrameworkElement { Tag: string value } ? AdapterId.FromAddress(value) : null;

    private static string? PeerIdOf(object sender) =>
        sender is FrameworkElement { Tag: string value } && !string.IsNullOrWhiteSpace(value)
            ? value
            : null;

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
