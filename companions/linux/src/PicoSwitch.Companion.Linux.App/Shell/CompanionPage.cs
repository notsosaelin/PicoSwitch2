using Avalonia.Controls;
using Avalonia.Input;
using PicoSwitch.Companion.Services;

namespace PicoSwitch.Companion.Linux.App.Shell;

/// <summary>
/// What every page shares: page-scoped subscriptions, the operation notice,
/// and the rule that a failed operation is reported, never thrown at the user.
///
/// Subscriptions follow the visual tree (Loaded/Unloaded in WinUI). The
/// SESSION does not: navigating away must never tear down a live BLE link.
/// </summary>
internal abstract class CompanionPage : UserControl
{
    protected CompanionPage(MainWindow shell)
    {
        Shell = shell;
        AttachedToVisualTree += (_, _) =>
        {
            OnShown();
            ApplyShortWindow();
        };
        DetachedFromVisualTree += (_, _) => OnHidden();
        SizeChanged += (_, _) => ApplyShortWindow();
    }

    /// <summary>Whether the page last laid out for a short window.</summary>
    protected bool ShortWindow { get; private set; }

    protected MainWindow Shell { get; }

    protected DialogHost Dialogs => Shell.Dialogs;

    /// <summary>The result of the last action on this page.</summary>
    protected Notice Operation { get; } = new();

    protected ProgressBar Busy { get; } = new() { IsIndeterminate = true, IsVisible = false, MaxWidth = 240 };

    /// <summary>
    /// The page's own Back (companion-shell-layout.md, "Navigation"): close
    /// whatever the page has open (a pane, a selection) and return true, or
    /// return false and the shell shows or hides the navigation. Back never
    /// leaves the page or the app.
    /// </summary>
    public virtual bool HandleBack() => false;

    /// <summary>Where focus lands when a controller or keyboard arrives on the page (Play: Start / Stop).</summary>
    public virtual InputElement? PreferredFocus => null;

    protected virtual void OnShown()
    {
    }

    protected virtual void OnHidden()
    {
    }

    /// <summary>
    /// The window crossed <see cref="Sizes.ShortWindowHeight"/>. Pages with a
    /// short-window arrangement move their tabs and actions here; called once
    /// on arrival and then only on a change.
    /// </summary>
    protected virtual void OnShortWindowChanged(bool isShort)
    {
    }

    private bool shortApplied;

    private void ApplyShortWindow()
    {
        // The WINDOW's height, as Android measures it: the page's own height
        // may have other chrome taken out.
        var height = TopLevel.GetTopLevel(this)?.ClientSize.Height ?? double.PositiveInfinity;
        var isShort = height < Sizes.ShortWindowHeight;
        if (shortApplied && isShort == ShortWindow)
        {
            return;
        }

        shortApplied = true;
        ShortWindow = isShort;
        if (Content is ScrollViewer { Content: PageFrame frame })
        {
            frame.Short = isShort;
        }

        OnShortWindowChanged(isShort);
    }

    protected void Report(string message, NoticeSeverity severity) =>
        Operation.Show(null, message, severity);

    /// <summary>
    /// Run one operation. The text shown on failure is the one the service
    /// produced — the layers below already word their failures for a person,
    /// and "something went wrong" would throw that away.
    /// </summary>
    protected async Task SafeAsync(Func<Task> operation, Action? afterwards = null)
    {
        Busy.IsVisible = true;
        Operation.Hide();
        try
        {
            await operation();
        }
        catch (Exception error)
        {
            var message = ManagementErrorText.Summarize(error);
            AppServices.Diagnostics.Error("ui", message);
            Report(message, NoticeSeverity.Error);
        }
        finally
        {
            Busy.IsVisible = false;
            afterwards?.Invoke();
        }
    }
}
