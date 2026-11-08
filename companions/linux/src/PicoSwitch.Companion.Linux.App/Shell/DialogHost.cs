using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace PicoSwitch.Companion.Linux.App.Shell;

/// <summary>
/// ContentDialog, inside the window.
///
/// Drawn as an overlay rather than a second top-level window ON PURPOSE:
/// gamescope (SteamOS Game Mode) shows one window at a time, so a modal
/// window there can land behind the app or never appear at all, leaving the
/// user facing a frozen-looking page. An overlay is always where the user is.
/// </summary>
internal sealed class DialogHost : Panel
{
    private TaskCompletionSource<int>? pending;

    public DialogHost()
    {
        IsVisible = false;
        this.Bind(BackgroundProperty, Tokens.Scrim);
        KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape)
            {
                Close(-1);
                e.Handled = true;
            }
        };
    }

    /// <summary>
    /// Show a dialog and wait for a button. Returns the button's index, or -1
    /// when dismissed with Escape. The first button marked primary is the
    /// accent; <paramref name="defaultIndex"/> takes focus, so destructive
    /// dialogs pass their Cancel button.
    /// </summary>
    public Task<int> ShowAsync(string title, Control content, IReadOnlyList<string> buttons, int primaryIndex = -1, int defaultIndex = -1)
    {
        // One dialog at a time; a second request supersedes the first as a
        // cancellation, never stacks behind it invisibly.
        Close(-1);
        pending = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Right,
        };
        Button? focus = null;
        for (var i = 0; i < buttons.Count; i++)
        {
            var index = i;
            var button = Ui.Button(buttons[i], (_, _) => Close(index), accent: i == primaryIndex);
            row.Children.Add(button);
            if (i == defaultIndex || (defaultIndex < 0 && i == buttons.Count - 1))
            {
                focus = button;
            }
        }

        var heading = Ui.Heading(title);
        heading.FontSize = 18;

        // Capped, never fixed: on a narrow window the card takes the width there
        // is (less a margin) and its content scrolls rather than running off the
        // bottom of a short landscape screen.
        var card = new Border
        {
            MaxWidth = 560,
            Margin = new Thickness(16),
            Padding = new Thickness(24, 22),
            CornerRadius = new CornerRadius(12),
            BorderThickness = new Thickness(1),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            BoxShadow = BoxShadows.Parse("0 8 32 0 #40000000"),
            Child = new DockPanel
            {
                Children =
                {
                    Docked(heading, Dock.Top, new Thickness(0, 0, 0, 14)),
                    Docked(row, Dock.Bottom, new Thickness(0, 18, 0, 0)),
                    new ScrollViewer { Content = content, MaxHeight = 520 },
                },
            },
        }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);

        Children.Clear();
        Children.Add(card);
        IsVisible = true;
        focus?.Focus();
        return pending.Task;
    }

    private static Control Docked(Control control, Dock dock, Thickness margin)
    {
        DockPanel.SetDock(control, dock);
        control.Margin = margin;
        return control;
    }

    /// <summary>A plain confirmation. True when the confirming (first) button was chosen.</summary>
    public async Task<bool> ConfirmAsync(string title, string body, string confirm, string cancel = "Cancel") =>
        await ShowAsync(title, Ui.Text(body), [confirm, cancel], primaryIndex: 0, defaultIndex: 1) == 0;

    public void Close(int result)
    {
        var completing = pending;
        pending = null;
        IsVisible = false;
        Children.Clear();
        completing?.TrySetResult(result);
    }
}
