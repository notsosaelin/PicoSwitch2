using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
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

    /// <summary>
    /// The user's local profiles. Separate from <see cref="adapters"/> on
    /// purpose: every library operation on this page goes here and reaches no
    /// management session at all.
    /// </summary>
    private readonly KbmLibraryRepository library = AppServices.KbmLibrary;

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
    /// The rows currently realized in the picker, so a render can tell whether the
    /// collection actually has to change.
    /// </summary>
    /// <remarks>
    /// The picker used to be cleared and rebuilt on every render, which is what
    /// let an asynchronously-arriving repaint tear the items out from under a
    /// control that still referenced them. See <see cref="KbmProfileSelection"/>
    /// for the crash this closes.
    /// </remarks>
    private IReadOnlyList<KbmSelectableProfile> renderedProfileRows = [];

    /// <summary>
    /// The selected LOCAL profile's id, or null for the built-in template.
    /// </summary>
    /// <remarks>
    /// Selection identity lives HERE, not in a <see cref="ComboBoxItem"/> that
    /// every render replaced — that was the crash. It is a local library id, not
    /// an adapter profile id: which profile the user is editing and which profile
    /// the adapter has resident are separate facts.
    /// </remarks>
    private string? selectedLocalId;

    /// <summary>
    /// The picker's rows: the built-in template, then the user's library.
    /// </summary>
    private IReadOnlyList<KbmSelectableProfile> LibraryRows() =>
    [
        new KbmSelectableProfile(string.Empty, "Default (built-in)"),
        .. library.Value.For(profile)
                  .Select(local => new KbmSelectableProfile(local.Id, local.Name)),
    ];

    /// <summary>
    /// Whether a <see cref="ContentDialog"/> is open. WinUI allows exactly one;
    /// a second <c>ShowAsync</c> throws, and from an `async void` handler that
    /// throw terminates the process.
    /// </summary>
    private bool dialogOpen;

    /// <summary>
    /// The local, unsaved copy of the profile being edited.
    ///
    /// Every edit on this page mutates THIS and nothing else, and Save writes it
    /// to the LOCAL library. No management command is sent by any of it.
    ///
    /// Keyed on a local GUID rather than an adapter profile id, which is the
    /// correction: while the editor held a KeyboardMouseDraft, "the profile I
    /// have open" and "the profile resident on the adapter" were one object, so
    /// creating a profile wrote to flash and Save could change what the console
    /// runs. Getting content onto the adapter is now a separate assignment.
    /// </summary>
    private KbmLocalDraft? draft;
    private DispatcherTimer? mouseCommit;
    private KbmMouseField? pendingMouseField;
    private int pendingMouseValue;
    private KeyboardMouseView? view;
    private bool suppressSelection;

    public KeyboardMousePage()
    {
        InitializeComponent();

        // Save is the primary action of the profile row and is styled as such.
        // Applied here rather than in markup because a Page.Resources style that
        // is BasedOn a framework theme style does not resolve at markup-compile
        // time, and the page already reaches for this resource the same way to
        // mark a changed key.
        SaveButton.Style = (Style)Application.Current.Resources["AccentButtonStyle"];

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private async void OnLoaded(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            adapters.KeyboardMouse.Changed += OnStateChanged;
            adapters.KeyboardMouseBusy.Changed += OnStateChanged;
            adapters.Connection.Changed += OnStateChanged;

            Render();

            // Read on arrival when there is a session, so the page is useful
            // without a manual step. A failure here is reported, not thrown at the
            // user as an empty keyboard with no explanation.
            if (adapters.Connection.Value.Connected && !adapters.KeyboardMouse.Value.Loaded)
            {
                await SafeAsync(() => adapters.RefreshKeyboardMouseAsync());
            }
        });

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
        await GuardAsync(() => SafeAsync(() => adapters.RefreshKeyboardMouseAsync()));

    private async void OnSetMode(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
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
    });

    private void RenderProfileSelector(KeyboardMouseView view)
    {
        // Only ever called in the Ready state, where the library is loaded and
        // Default always exists. The row has no hidden variant any more: an
        // adapter without a profile library does not reach this page at all.
        ProfileRow.Visibility = Visibility.Visible;
        // RECONCILE, NEVER REBUILD BLINDLY.
        //
        // This collection used to be cleared and repopulated on every render, and
        // the selection was read back out of the ComboBoxItem objects it created.
        // Renders arrive asynchronously — from the dispatcher-enqueued state
        // callback and from every command's completion — so the items could be
        // torn out from under a control that still referenced them. WER recorded
        // the result as 0xc000027b in Microsoft.UI.Xaml.dll, ERROR_NOT_FOUND.
        //
        // Now: identity is the LOCAL profile id, the rows are rebuilt only when
        // what the control DISPLAYS actually differs, and the collection is never
        // touched while the user has the popup open.
        //
        // The rows come from the LIBRARY, not from the adapter: this picker lists
        // what the user owns, and the adapter's three positions are the separate
        // card below.
        var rows = LibraryRows();
        var plan = KbmProfileSelection.Plan(renderedProfileRows, rows,
                                            selectedLocalId ?? string.Empty);
        selectedLocalId = plan.SelectedId.Length == 0 ? null : plan.SelectedId;

        if (plan.Rebuild && !ProfileSelector.IsDropDownOpen)
        {
            populatingProfiles = true;
            try
            {
                ProfileSelector.Items.Clear();
                foreach (var row in plan.Rows)
                {
                    ProfileSelector.Items.Add(new ComboBoxItem
                    {
                        Content = row.Label,
                        Tag = row.Id,
                    });
                }

                renderedProfileRows = [.. plan.Rows];
            }
            finally
            {
                populatingProfiles = false;
            }
        }

        if (ProfileSelector.SelectedIndex != plan.SelectedIndex &&
            plan.SelectedIndex < ProfileSelector.Items.Count)
        {
            populatingProfiles = true;
            try
            {
                ProfileSelector.SelectedIndex = plan.SelectedIndex;
            }
            finally
            {
                populatingProfiles = false;
            }
        }

        // These are LOCAL commands only. Activation moved to the bank card,
        // where the row shows which position it acts on -- an "apply" button
        // beside Save invited exactly the confusion the two-layer model removes.
        SaveButton.IsEnabled = view.CanSave;
        DiscardButton.IsEnabled = view.Dirty;
        DuplicateButton.IsEnabled = view.SelectedProfile is not null &&
                                    view.DraftState != KbmDraftState.Disconnected;
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
    private async void OnSelectedProfileChanged(object sender, SelectionChangedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (ProfileSelector.SelectedItem is not ComboBoxItem item ||
                item.Tag is not string id)
            {
                return;
            }

            // Decided BEFORE anything is awaited. The old version read the draft,
            // awaited a dialog, then acted on state that could have been replaced
            // while it was suspended.
            var action = KbmProfileSelection.Decide(
                id,
                draft?.ProfileId ?? string.Empty,
                draft?.Dirty == true,
                populatingProfiles);

            switch (action)
            {
                case KbmSelectionAction.Ignore:
                    return;

                case KbmSelectionAction.ConfirmDiscard
                    when !await ConfirmAsync(
                        "Discard unsaved changes?",
                        "The changes to this profile have not been saved and will be lost.",
                        "Discard"):
                    Render();  // put the selector back on the profile still open
                    return;
            }

            selectedLocalId = id.Length == 0 ? null : id;
            OpenProfile(id);
        });

    /// <summary>
    /// Open a LOCAL profile in the editor. No adapter involved.
    /// </summary>
    /// <remarks>
    /// This used to read the mapping from the adapter, which is what made
    /// opening — and therefore editing — require a connection. A local profile
    /// carries its own sparse overrides and the canonical table is embedded, so
    /// the whole mapping is composable offline.
    /// </remarks>
    private void OpenProfile(string? id)
    {
        if (string.IsNullOrEmpty(id))
        {
            draft = KbmLocalDraft.FromDefault(profile);
            Render();
            return;
        }

        var local = library.Value.Find(id);
        draft = local is null
            ? KbmLocalDraft.FromDefault(profile)
            : KbmLocalDraft.From(local);
        Render();
    }

    private async void OnSaveProfile(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (draft is null)
            {
                return;
            }

            // LOCAL SAVE. Writes the app's library and sends NOTHING to the
            // adapter -- not even when an older copy of this profile is resident
            // there. The page reports that divergence; only an explicit Assign
            // resolves it.
            if (draft.IsBuiltin)
            {
                // Default is a template, never written into: saving it creates a
                // new library profile, which is what keeps it always available.
                var name = await PromptAsync("Save as new profile", "Profile name",
                                             SuggestName("My mapping"));
                if (string.IsNullOrWhiteSpace(name))
                {
                    return;
                }

                var created = library.Create(profile, name!, draft.Overrides,
                                             draft.Mouse);
                draft = KbmLocalDraft.From(created);
                selectedLocalId = created.Id;
            }
            else
            {
                var saved = library.Save(draft.ProfileId, draft.Name,
                                         draft.Overrides, draft.Mouse);
                draft = draft.Rebased(saved);
            }

            Render();
        });

    private void OnDiscardProfile(object sender, RoutedEventArgs e)
    {
        // Local only, and there is nothing anywhere to undo: the draft never
        // left this process.
        draft = draft?.Discard();
        Render();
    }

    /// <summary>
    /// New, Duplicate, Rename and Delete are LOCAL LIBRARY operations.
    /// </summary>
    /// <remarks>
    /// None of them requires a connection and none of them writes to an adapter.
    /// New used to call the staged management transaction — creating a profile
    /// erased flash and assigned it to the resident working set in one step —
    /// and Duplicate called `kbm profile dup`. That coupling is the defect this
    /// removes: the library is the user's, the adapter's three positions per
    /// layout are a working set, and copying between them is an explicit act.
    /// </remarks>
    private async void OnNewProfile(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            var name = await PromptAsync(
                "New profile",
                "Starts from the built-in Default mapping. It stays in your " +
                "library until you assign it to the adapter.",
                SuggestName("My mapping"));
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            var fresh = KbmLocalDraft.FromDefault(profile);
            var created = library.Create(profile, name!, fresh.Overrides,
                                         fresh.Mouse);
            selectedLocalId = created.Id;
            draft = KbmLocalDraft.From(created);
            Render();
        });

    private async void OnDuplicateProfile(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (draft is null)
            {
                return;
            }

            var name = await PromptAsync("Duplicate profile", "Profile name",
                                         SuggestName($"{draft.Name} copy"));
            if (string.IsNullOrWhiteSpace(name))
            {
                return;
            }

            // The draft's CURRENT content, so duplicating an edited profile
            // copies what the user is looking at rather than what was saved.
            var created = library.Create(profile, name!, draft.Overrides,
                                         draft.Mouse);
            selectedLocalId = created.Id;
            draft = KbmLocalDraft.From(created);
            Render();
        });

    private async void OnRenameProfile(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
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

            // Identity and content are untouched, so a resident copy that agreed
            // before still agrees.
            var renamed = library.Rename(draft.ProfileId, name!);
            if (renamed is not null)
            {
                draft = draft.WithName(name!).Rebased(renamed);
            }

            Render();
        });

    private async void OnDeleteProfile(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (draft is null || draft.IsBuiltin)
            {
                return;
            }

            // A resident copy is a SEPARATE snapshot the adapter owns and may be
            // running. Deleting the library profile must not remove it, and the
            // user is told so rather than left to discover it.
            var resident = adapters.KeyboardMouse.Value.Profiles.Profiles
                .FirstOrDefault(row => row.Layout == profile &&
                                       row.Fingerprint == draft.Fingerprint);
            if (!await ConfirmAsync(
                    $"Delete '{draft.Name}' from your library?",
                    resident is null
                        ? "This removes it from this app. Nothing on the adapter changes."
                        : $"This removes it from this app. The copy on the adapter " +
                          $"({KbmPositions.Label(resident.Position)}) stays, and the " +
                          "console keeps working as it does now.",
                    "Delete"))
            {
                return;
            }

            library.Delete(draft.ProfileId);
            selectedLocalId = null;
            OpenProfile(null);
        });

    /// <summary>
    /// ASSIGN the open profile into one of this layout's bank positions.
    /// </summary>
    /// <remarks>
    /// The only path from the library to the adapter, and the only place on this
    /// page that costs a flash write. Explicit for that reason: a user who edits
    /// and saves has changed their library, not the adapter, and the two must
    /// stay separate acts.
    ///
    /// Deliberately does NOT activate. If the chosen position is currently
    /// running, the console keeps its realized snapshot until the user activates,
    /// so an assignment cannot change gameplay mid-session.
    /// </remarks>
    private async void OnAssignToAdapter(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (draft is null || view is null)
            {
                return;
            }

            var state = adapters.KeyboardMouse.Value;
            var bank = KbmBankView.Bank(state, profile)
                .Where(slot => slot.Position != KbmPositions.Default)
                .ToList();

            var list = new ListView
            {
                SelectionMode = ListViewSelectionMode.Single,
                ItemsSource = bank
                    .Select(slot => slot.Empty
                        ? $"{slot.PositionLabel} — Empty"
                        : $"{slot.PositionLabel} — {slot.ResidentLabel} (replace)")
                    .ToArray(),
            };
            // Preselect a free position: filling an empty slot is the common
            // case, and replacing one should be a deliberate choice.
            list.SelectedIndex = bank.FindIndex(slot => slot.Empty);
            if (list.SelectedIndex < 0)
            {
                list.SelectedIndex = 0;
            }

            var dialog = new ContentDialog
            {
                Title = $"Assign '{draft.Name}' to",
                Content = list,
                PrimaryButtonText = "Assign",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Primary,
            };

            if (await ShowDialogAsync(dialog) != ContentDialogResult.Primary ||
                list.SelectedIndex < 0)
            {
                return;
            }

            var chosen = bank[list.SelectedIndex];
            var local = new KbmLocalProfile
            {
                Id = draft.ProfileId,
                Layout = profile,
                Name = draft.Name,
                // The SPARSE overrides, which is exactly what the adapter stores.
                // Sending the effective mapping would upload the canonical table
                // as if the user had chosen every one of its bindings.
                Bindings = draft.Overrides,
                Mouse = draft.Mouse,
            };

            // SUCCESS IS REPORTED ONLY IF THE UPLOAD ACTUALLY COMPLETED. The
            // result-carrying overload returns null when the operation threw,
            // which is what distinguishes "committed and read back" from
            // "timed out halfway through the binds".
            //
            // The void overload was used here, so the message below printed
            // unconditionally — over the top of the error banner SafeAsync had
            // just raised. A user whose session died mid-transaction was told
            // their profile was on the adapter while the bank still showed
            // Empty. Observed 2026-08-31.
            var assigned = await SafeAsync(() => adapters.AssignKbmPositionAsync(
                profile, chosen.Position, local));
            if (assigned is null)
            {
                return;
            }

            Report(
                $"'{draft.Name}' is now {chosen.PositionLabel} on the adapter. " +
                "Activate it to use it now.",
                InfoBarSeverity.Success);
            Render();
        });

    /// <summary>
    /// Copy a resident profile into the local library.
    /// </summary>
    /// <remarks>
    /// THE CROSS-PLATFORM BRIDGE, in the direction that brings work in. A profile
    /// created on the Android companion reaches this one only as content resident
    /// on the adapter — the two libraries share no ids — so this is the only way
    /// to take ownership of it here.
    ///
    /// Matching is by CONTENT, not by name or id, so copying the same resident
    /// twice returns the row already held instead of adding another duplicate on
    /// every reconnect.
    /// </remarks>
    private async Task CopyToLibraryAsync(KbmBankSlot slot)
    {
        if (slot.Resident is not { } resident)
        {
            return;
        }

        KbmMapping? mapping = null;
        await SafeAsync(async () =>
            mapping = await adapters.LoadKbmPositionAsync(profile, slot.Position));
        if (mapping is null)
        {
            return;
        }

        var before = library.Value.Profiles.Count;
        // Only the OVERRIDES. `kbm map` reports the EFFECTIVE mapping, and
        // importing all of it would store the canonical table as if the user had
        // chosen every binding in it — after which a later change to the
        // firmware's defaults could never reach this profile.
        var overrides = mapping.Bindings.Where(binding => binding.Custom).ToList();
        var imported = library.Import(profile, resident.Name, overrides,
                                      adapters.KeyboardMouse.Value.Mouse);

        selectedLocalId = imported.Id;
        OpenProfile(imported.Id);
        Report(
            library.Value.Profiles.Count == before
                ? $"'{imported.Name}' is already in your library."
                : $"'{imported.Name}' copied to your library.",
            InfoBarSeverity.Success);
    }

    /// <summary>
    /// Empty one bank position on the adapter. The library keeps its copy.
    /// </summary>
    /// <remarks>
    /// The two stores are independent by design, so this is worth saying out
    /// loud in the confirmation: a user who expects Remove to delete their
    /// profile, or Delete to free an adapter position, will be surprised
    /// otherwise.
    /// </remarks>
    private async Task RemoveFromAdapterAsync(KbmBankSlot slot)
    {
        var consequence = slot.IsRuntime || slot.IsBoot
            ? " This layout falls back to the built-in Default."
            : string.Empty;

        if (!await ConfirmAsync(
                $"Remove '{slot.ResidentLabel}' from {slot.PositionLabel}?",
                $"The adapter's copy is removed.{consequence} Your library keeps " +
                "this profile.",
                "Remove"))
        {
            return;
        }

        await SafeAsync(() => adapters.RemoveKbmPositionAsync(profile, slot.Position));
    }

    /// <summary>
    /// Bind a physical key to one semantic switch action.
    /// </summary>
    /// <remarks>
    /// Any keyboard usage the model accepts. Function keys are offered first
    /// because they are the obvious choice, but nothing is reserved: the list is
    /// the same drawn keyboard the mapping grid uses.
    /// </remarks>
    private async Task ChooseSwitchKeyAsync(int position)
    {
        var caps = KeyboardLayout.AllKeys
            .Concat(KeyboardLayout.Other.Keys)
            .OrderBy(cap => cap.Usage is >= 0x3A and <= 0x45 ? 0 : 1)
            .ThenBy(cap => cap.Usage)
            .ToList();

        var list = new ListView
        {
            SelectionMode = ListViewSelectionMode.Single,
            MaxHeight = 380,
            ItemsSource = caps.Select(cap => cap.Label).ToArray(),
        };

        var dialog = new ContentDialog
        {
            Title = $"Key for {KbmPositions.Label(position)}",
            Content = list,
            PrimaryButtonText = "Assign",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        if (await ShowDialogAsync(dialog) != ContentDialogResult.Primary ||
            list.SelectedIndex < 0)
        {
            return;
        }

        var source = new KbmSource(KbmSourceKind.Key, caps[list.SelectedIndex].Usage);
        await SafeAsync(() => adapters.BindKbmSwitchAsync(source, position));
    }

    /// <summary>
    /// A name that is not already taken in the LOCAL library for this layout.
    /// </summary>
    /// <remarks>
    /// The library, not the adapter's residents: a suggestion drawn from the
    /// adapter would collide with the user's own profiles and would need a
    /// connection to make.
    /// </remarks>
    private string SuggestName(string basis)
    {
        var taken = library.Value
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

    private async void OnResetProfile(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (!await ConfirmAsync(
                    "Reset this mapping?",
                    "Every key goes back to the adapter's own default mapping. " +
                    "The other layout and the mouse tuning are not affected.",
                    "Reset mapping"))
            {
                return;
            }

            await SafeAsync(() => adapters.ResetProfileAsync(profile));
        });

    private async void OnResetAll(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (!await ConfirmAsync(
                    "Reset everything?",
                    "Both layouts and the mouse tuning go back to the adapter's own " +
                    "defaults. Every mapping you have changed is lost.",
                    "Reset everything"))
            {
                return;
            }

            await SafeAsync(() => adapters.ResetAllKeyboardMouseAsync());
        });

    private async void OnEditUndrawn(object sender, RoutedEventArgs e) =>
        await GuardAsync(async () =>
        {
            if (sender is FrameworkElement { Tag: int usage })
            {
                await EditAsync(new KbmSource(KbmSourceKind.Key, usage),
                                KeyboardLayout.Describe(
                                    new KbmSource(KbmSourceKind.Key, usage)));
            }
        });

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
            Title = $"What should {label} do?",
            Content = list,
            PrimaryButtonText = "Set",
            SecondaryButtonText = "Restore default",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        var result = await ShowDialogAsync(dialog);

        if (result == ContentDialogResult.Secondary)
        {
            await RestoreDefaultAsync(source);
            return;
        }

        if (result != ContentDialogResult.Primary || list.SelectedIndex < 0)
        {
            return;
        }

        await ApplyDestinationAsync(source, destinations[list.SelectedIndex]);
    }

    /// <summary>
    /// THE single path by which any binding changes.
    /// </summary>
    /// <remarks>
    /// The dialog, the middle-click clear gesture and anything added later all
    /// route through here, so a new gesture cannot acquire its own idea of what
    /// changing a binding means or forget to repaint.
    ///
    /// It edits the LOCAL DRAFT and sends nothing. That is the whole point of the
    /// draft: rebinding thirty keys costs zero adapter writes and zero flash
    /// erases, and Discard can actually undo it. Dirty state falls out of this
    /// for free — it is computed from draft content against the saved profile,
    /// never tracked with a flag — so a clear marks the profile unsaved exactly
    /// when it changed something.
    ///
    /// Before a profile has been opened there is no draft, and the per-binding
    /// command is the only way to reach the realized mapping.
    /// </remarks>
    private async Task ApplyDestinationAsync(KbmSource source, KbmDestination destination)
    {
        if (draft is not null)
        {
            draft = draft.With(source, destination);
            Render();
            return;
        }

        await SafeAsync(() => adapters.BindAsync(profile, source, destination));
    }

    /// <summary>
    /// Put back whatever the adapter shipped with, which is NOT the same as
    /// clearing: <c>default</c> restores, <c>none</c> silences.
    /// </summary>
    private async Task RestoreDefaultAsync(KbmSource source)
    {
        if (draft is not null)
        {
            draft = draft.With(source, DefaultDestination(source));
            Render();
            return;
        }

        // A null destination is the protocol's `default`.
        await SafeAsync(() => adapters.BindAsync(profile, source, destination: null));
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

        // TOP-LEVEL STATE FIRST. The editor exists only in Ready; every other
        // state says so and offers a reload. There is no legacy page behind this
        // to fall through to, deliberately: the profile workflow is the product,
        // and a mapping grid without it is the fallback that made a failed read
        // look like an unfinished app.
        // OFFLINE IS NOT A FAILURE. The library and the mapping editor need no
        // adapter and stay fully usable; only the adapter-owned half is disabled,
        // and it is disabled rather than hidden so the user can see what
        // connecting would bring back. A read that actually FAILED is a
        // different thing and still replaces the page.
        var ready = view.ShowEditor;
        NotReadyCard.Visibility = view.ShowNotReady ? Visibility.Visible
                                                    : Visibility.Collapsed;
        NotReadyTitle.Text = view.NotReadyTitle;
        NotReadyDetail.Text = view.NotReadyDetail;
        NotReadyRetry.IsEnabled = view.Availability.Enabled &&
                                  !adapters.KeyboardMouseBusy.Value;

        EditorCard.Visibility = ready ? Visibility.Visible : Visibility.Collapsed;
        BankCard.Visibility = ready ? Visibility.Visible : Visibility.Collapsed;
        MouseCard.Visibility = ready ? Visibility.Visible : Visibility.Collapsed;
        ResetCard.Visibility = ready ? Visibility.Visible : Visibility.Collapsed;

        BusyRing.IsActive = adapters.KeyboardMouseBusy.Value;
        ModeText.Text = ready ? view.ModeText : view.NotReadyTitle;
        DevicesText.Text = ready ? view.DevicesText : string.Empty;
        if (!ready)
        {
            // Mode is a real product control, but it is not meaningful against an
            // adapter whose state was never read.
            ModeBox.IsEnabled = false;
            ModeApply.IsEnabled = false;
            RefreshButton.IsEnabled = NotReadyRetry.IsEnabled;
            CountersExpander.Visibility = Visibility.Collapsed;
            DraftBar.IsOpen = false;
            InactiveProfileBar.IsOpen = false;
            return;
        }

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
        var interactive = enabled && !adapters.KeyboardMouseBusy.Value;
        ModeBox.IsEnabled = interactive;
        ModeApply.IsEnabled = interactive;
        RefreshButton.IsEnabled = interactive;
        ResetProfile.IsEnabled = interactive;
        ResetAll.IsEnabled = interactive;

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

        // The mapping grid follows the LOCAL draft and is editable offline; the
        // bank and switch keys are adapter-owned and are not.
        BuildKeyboard(view, true);
        BuildMouseButtons(view, true);
        BuildBank(view, view.AdapterAvailable && !adapters.KeyboardMouseBusy.Value);
        BuildSwitchKeys(view.AdapterAvailable && !adapters.KeyboardMouseBusy.Value);
        BuildSliders(view, enabled);
        BuildMouseToggles(view, enabled);

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
            OtherKeysHost.Children.Add(KeyButton(cell, enabled, fixedGeometry: false));
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
            MouseButtonHost.Children.Add(KeyButton(cell, enabled, width: cell.Cap.Width,
                                                   mouse: true, fixedGeometry: false));
        }
    }

    /// <param name="fixedGeometry">
    /// True for the keyboard grid, where a key must occupy exactly its share of
    /// the layout or the picture stops being a keyboard. False for the ISO,
    /// international and mouse-button rows, which are ordinary lists: those size
    /// to their content, because a key-unit width clipped "Muhenkan" and
    /// "Katakana/Hiragana" down to an ellipsis for no layout benefit at all.
    /// </param>
    private Button KeyButton(KeyBindingCell cell, bool enabled, double? width = null,
                             bool mouse = false, bool fixedGeometry = true)
    {
        var caption = new StackPanel { Spacing = 0 };
        caption.Children.Add(new TextBlock
        {
            Text = cell.Cap.Label,
            HorizontalAlignment = HorizontalAlignment.Center,
            // Only the grid clips. Elsewhere the button grows instead.
            TextTrimming = fixedGeometry
                ? TextTrimming.CharacterEllipsis
                : TextTrimming.None,
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
                TextTrimming = fixedGeometry
                    ? TextTrimming.CharacterEllipsis
                    : TextTrimming.None,
            });
        }

        var button = new Button
        {
            Content = caption,
            Height = KeyUnit - KeyGap,
            IsEnabled = enabled,
            Tag = cell.Cap.Usage,
        };

        if (fixedGeometry)
        {
            button.Width = (width ?? cell.Cap.Width) * KeyUnit - KeyGap;
            button.Padding = new Thickness(2);
        }
        else
        {
            // Grow to fit; the key unit becomes a floor rather than a ceiling.
            button.MinWidth = (width ?? cell.Cap.Width) * KeyUnit - KeyGap;
            button.Padding = new Thickness(10, 2, 10, 2);
        }

        // Colour alone cannot carry "this one is changed" -- it fails high contrast
        // and it fails a screen reader. The accessible name carries the whole fact.
        AutomationProperties.SetName(button, cell.AccessibleName);
        ToolTipService.SetToolTip(button, cell.Tooltip);

        if (cell.Overridden)
        {
            button.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        }

        var source = mouse
            ? new KbmSource(KbmSourceKind.MouseButton, cell.Cap.Usage)
            : new KbmSource(KbmSourceKind.Key, cell.Cap.Usage);
        button.Click += async (_, _) => await EditAsync(source, cell.Cap.Label);

        // MIDDLE-CLICK CLEARS.
        //
        // A shortcut for the most common edit, which otherwise costs a dialog and
        // a list selection. It routes through the same ApplyDestinationAsync as
        // the dialog, so it produces an ordinary draft edit -- no adapter write,
        // undone by Discard, and dirty state follows from the content like any
        // other change.
        //
        // Guarded on CanClear so the gesture cannot dirty a profile with a change
        // that alters nothing: middle-clicking an unmapped key does nothing.
        //
        // PointerPressed rather than PointerReleased because that is where the
        // button's own middle-button state is unambiguous, and Handled is set so
        // the gesture cannot also trip the click path that opens the dialog.
        if (cell.CanClear)
        {
            button.PointerPressed += async (s, args) =>
            {
                var point = args.GetCurrentPoint((UIElement)s);
                if (!point.Properties.IsMiddleButtonPressed)
                {
                    return;
                }

                args.Handled = true;
                if (!button.IsEnabled)
                {
                    return;
                }

                await ApplyDestinationAsync(source, KbmDestination.None);
            };
        }

        return button;
    }

    /// <summary>
    /// The adapter's bank: one row per position, empty ones included.
    /// </summary>
    /// <remarks>
    /// Each row carries the operations that act on THAT position, so the user can
    /// see what an Activate or a Remove is about to affect. The library's Save
    /// sends nothing; everything on these rows does.
    /// </remarks>
    private void BuildBank(KeyboardMouseView current, bool enabled)
    {
        BankHost.Children.Clear();
        var state = adapters.KeyboardMouse.Value;
        var bank = KbmBankView.Bank(state, profile);

        foreach (var slot in bank)
        {
            var row = new Grid { ColumnSpacing = 12, Margin = new Thickness(0, 2, 0, 2) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var position = new TextBlock
            {
                Text = slot.PositionLabel,
                Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(position, 0);
            row.Children.Add(position);

            var detail = new StackPanel { Spacing = 0, VerticalAlignment = VerticalAlignment.Center };
            detail.Children.Add(new TextBlock
            {
                Text = slot.ResidentLabel,
                Foreground = slot.Empty
                    ? (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]
                    : (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"],
                TextTrimming = TextTrimming.CharacterEllipsis,
            });

            // Runtime and boot are separate facts and are labelled separately: a
            // switch key moves the first and not the second, so after one press
            // they differ for the rest of the session.
            var marks = new List<string>();
            if (slot.IsRuntime)
            {
                marks.Add("active now");
            }

            if (slot.IsBoot)
            {
                marks.Add("on startup");
            }

            if (slot.SwitchKey is { } key)
            {
                marks.Add($"key {KeyboardLayout.Describe(key)}");
            }

            if (marks.Count > 0)
            {
                detail.Children.Add(new TextBlock
                {
                    Text = string.Join(" · ", marks),
                    Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                    Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                });
            }

            Grid.SetColumn(detail, 1);
            row.Children.Add(detail);

            var activate = new Button
            {
                Content = "Activate",
                MinWidth = 88,
                Height = 32,
                // Runtime activation only: zero flash writes. An empty position
                // has nothing to activate, and the position already running does
                // not need it.
                IsEnabled = enabled && !slot.Empty && !slot.IsRuntime,
                Tag = slot.Position,
            };
            activate.Click += async (_, _) => await GuardAsync(() => SafeAsync(
                () => adapters.ActivateKbmPositionAsync(profile, slot.Position)));
            Grid.SetColumn(activate, 2);
            row.Children.Add(activate);

            var boot = new Button
            {
                Content = "On startup",
                MinWidth = 96,
                Height = 32,
                // The one profile selection worth a flash write, and the only
                // reason it is a separate button from Activate.
                IsEnabled = enabled && !slot.Empty && !slot.IsBoot,
                Tag = slot.Position,
            };
            boot.Click += async (_, _) => await GuardAsync(() => SafeAsync(
                () => adapters.SetKbmBootPositionAsync(profile, slot.Position)));
            Grid.SetColumn(boot, 3);
            row.Children.Add(boot);

            // COPY and REMOVE. Only for a real position holding something --
            // Default is built in, cannot be emptied, and is already the template
            // every new profile starts from.
            if (slot.Position != KbmPositions.Default)
            {
                var actions = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    Spacing = 8,
                };

                var copy = new Button
                {
                    Content = "Copy to library",
                    Height = 32,
                    IsEnabled = enabled && !slot.Empty,
                };
                copy.Click += async (_, _) => await GuardAsync(
                    () => CopyToLibraryAsync(slot));
                actions.Children.Add(copy);

                var remove = new Button
                {
                    Content = "Remove",
                    MinWidth = 80,
                    Height = 32,
                    IsEnabled = enabled && !slot.Empty,
                };
                remove.Click += async (_, _) => await GuardAsync(
                    () => RemoveFromAdapterAsync(slot));
                actions.Children.Add(remove);

                Grid.SetColumn(actions, 4);
                row.Children.Add(actions);
            }

            BankHost.Children.Add(row);
        }

        var used = bank.Count(slot => !slot.Empty && slot.Position != KbmPositions.Default);
        BankSummary.Text = current.AdapterUnavailableReason ??
            $"{used} of {KbmLimits.PositionsPerLayout} positions used for " +
            $"{(profile == KbmLayout.Keyboard ? "keyboard" : "keyboard and mouse")}. " +
            "These work with no app connected.";

        // Assign needs BOTH a live adapter and something to send. A draft on the
        // built-in template has nothing of the user's in it yet.
        AssignButton.IsEnabled = enabled && draft is not null && !draft.IsBuiltin;
    }

    /// <summary>
    /// One row per semantic switch action, bound or not.
    /// </summary>
    /// <remarks>
    /// Rendered from the ACTIONS rather than from the adapter's bindings: a list
    /// built from the bindings would hide the actions the user has not set up
    /// yet, which are exactly the ones they came here to set up.
    /// </remarks>
    private void BuildSwitchKeys(bool enabled)
    {
        SwitchHost.Children.Clear();
        foreach (var (position, key) in
                 KbmBankView.SwitchActions(adapters.KeyboardMouse.Value))
        {
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var label = new TextBlock
            {
                Text = KbmPositions.Label(position),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(label, 0);
            row.Children.Add(label);

            var assigned = new TextBlock
            {
                Text = key is null ? "Not assigned" : KeyboardLayout.Describe(key),
                Foreground = key is null
                    ? (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]
                    : (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"],
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(assigned, 1);
            row.Children.Add(assigned);

            var buttons = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
            };
            var choose = new Button
            {
                Content = key is null ? "Assign key" : "Change",
                MinWidth = 96,
                Height = 32,
                IsEnabled = enabled,
            };
            choose.Click += async (_, _) => await GuardAsync(
                () => ChooseSwitchKeyAsync(position));
            buttons.Children.Add(choose);

            if (key is not null)
            {
                var clear = new Button { Content = "Clear", MinWidth = 72, Height = 32,
                                         IsEnabled = enabled };
                clear.Click += async (_, _) => await GuardAsync(() => SafeAsync(
                    () => adapters.BindKbmSwitchAsync(key, position: null)));
                buttons.Children.Add(clear);
            }

            Grid.SetColumn(buttons, 2);
            row.Children.Add(buttons);
            SwitchHost.Children.Add(row);
        }
    }

    /// <summary>
    /// The mouse-tuning block: name and value on one line, slider under it,
    /// description under that.
    /// </summary>
    /// <remarks>
    /// The value sits in a fixed-width, right-aligned column so the three
    /// readouts line up with each other and the sliders all start and end at the
    /// same x. Previously the value was appended below each slider as free text
    /// of varying length, so nothing in the section shared an edge.
    ///
    /// The wording comes from <see cref="MouseSlider"/>, not from here: it is
    /// product copy, it is asserted by tests, and building it in the page would
    /// put it beyond both.
    /// </remarks>
    private void BuildSliders(KeyboardMouseView current, bool enabled)
    {
        SliderHost.Children.Clear();
        foreach (var model in current.MouseSliders)
        {
            var panel = new StackPanel { Spacing = 4 };

            var heading = new Grid();
            heading.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            heading.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var label = new TextBlock
            {
                Text = model.Label,
                Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(label, 0);
            heading.Children.Add(label);

            var value = new TextBlock
            {
                Text = model.ValueText,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                HorizontalAlignment = HorizontalAlignment.Right,
                VerticalAlignment = VerticalAlignment.Center,
                TextAlignment = TextAlignment.Right,
                MinWidth = 96,
            };
            Grid.SetColumn(value, 1);
            heading.Children.Add(value);
            panel.Children.Add(heading);

            var slider = new Slider
            {
                Minimum = model.Minimum,
                Maximum = Math.Max(model.Maximum, model.Minimum + 1),
                Value = Math.Clamp(model.Value, model.Minimum, Math.Max(model.Maximum, model.Minimum + 1)),
                IsEnabled = enabled && model.Available,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Margin = new Thickness(0, -4, 0, 0),
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

            panel.Children.Add(new TextBlock
            {
                Text = model.Description,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, -4, 0, 0),
            });

            SliderHost.Children.Add(panel);
        }
    }

    /// <summary>
    /// The invert switches, each with its one-line description beneath it.
    /// </summary>
    /// <remarks>
    /// Stacked rather than side by side: two checkboxes on one row cannot carry
    /// descriptions without the second column's text wrapping against the first
    /// one's, and "invert vertical" is exactly the setting a user is unsure about.
    /// </remarks>
    private void BuildMouseToggles(KeyboardMouseView current, bool enabled)
    {
        ToggleHost.Children.Clear();
        foreach (var model in current.MouseToggles)
        {
            var panel = new StackPanel { Spacing = 0 };
            var box = new CheckBox
            {
                Content = model.Label,
                IsChecked = model.Value,
                IsEnabled = enabled,
                MinWidth = 0,
            };
            AutomationProperties.SetName(box, model.Label);

            var field = model.Field;
            box.Click += async (s, _) => await SafeAsync(() => adapters.SetMouseAsync(
                field, ((CheckBox)s).IsChecked == true ? 1 : 0));

            panel.Children.Add(box);
            panel.Children.Add(new TextBlock
            {
                Text = model.Description,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                TextWrapping = TextWrapping.Wrap,
                // Line the description up under the checkbox's label, not under
                // its box.
                Margin = new Thickness(30, 0, 0, 0),
            });

            ToggleHost.Children.Add(panel);
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

    /// <summary>
    /// THE exception boundary for every `async void` handler on this page.
    /// </summary>
    /// <remarks>
    /// An exception escaping an `async void` UI handler has no handler at all: it
    /// is rethrown on the dispatcher and terminates the process. WER recorded
    /// exactly that — 0xc000027b, a stowed exception surfacing through
    /// Microsoft.UI.Xaml.dll.
    ///
    /// This does NOT swallow: the failure is logged with its full detail and
    /// shown to the user, the page is repainted into a coherent state, and the
    /// action can be retried. Nothing is hidden; the process simply survives.
    /// </remarks>
    private async Task GuardAsync(Func<Task> handler)
    {
        try
        {
            await handler();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            adapters.Diagnostics.Error("ui", $"{message} ({error.GetType().Name})");
            Report(message, InfoBarSeverity.Error);
            Render();
        }
    }

    /// <summary>
    /// Show one dialog at a time.
    /// </summary>
    /// <remarks>
    /// WinUI permits a single open <see cref="ContentDialog"/>; a second
    /// <c>ShowAsync</c> throws. Every dialog on this page is opened from an
    /// `async void` handler, so that throw was a process kill — the second WER
    /// bucket (E_UNEXPECTED). Overlap is reachable whenever a dialog is open and
    /// another handler resumes: a confirmation during a selection change while a
    /// rename or a key-edit dialog is still up.
    ///
    /// A second request is DECLINED rather than queued. Queuing would show the
    /// user a dialog about a transition they have since moved past.
    /// </remarks>
    private async Task<ContentDialogResult> ShowDialogAsync(ContentDialog dialog)
    {
        if (dialogOpen)
        {
            return ContentDialogResult.None;
        }

        dialogOpen = true;
        try
        {
            dialog.XamlRoot = XamlRoot;
            return await dialog.ShowAsync();
        }
        finally
        {
            dialogOpen = false;
        }
    }

    private async Task<bool> ConfirmAsync(string title, string content, string action) =>
        await ShowDialogAsync(new ContentDialog
        {
            Title = title,
            Content = content,
            PrimaryButtonText = action,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        }) == ContentDialogResult.Primary;

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
            Title = title,
            Content = content,
            PrimaryButtonText = "OK",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };
        return await ShowDialogAsync(dialog) == ContentDialogResult.Primary
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
