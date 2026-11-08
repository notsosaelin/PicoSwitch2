using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using PicoSwitch.Companion.Services.Presentation;

namespace PicoSwitch.Companion.Linux.App.Shell;

/// <summary>
/// The building blocks every page is made of, and the only place layout rules
/// live.
///
/// THE LAYOUT GRAMMAR. A page is a header (title, subtitle, page actions) over a
/// column of SECTIONS. A section is a card holding ROWS. A row is a label and
/// description on the left and its controls on the right, so every control on a
/// page starts at the same right-hand edge and every label at the same left one.
/// The first Linux shell let each card size to its own content and each control
/// to its own text, which is what made it look uneven; nothing here sizes to its
/// content horizontally except text.
///
/// RESPONSIVE BY CONSTRUCTION, NOT BY DEVICE. Nothing in this file knows what a
/// Steam Deck is. A row stacks its controls under its label when the two no
/// longer fit side by side; the page column is centred and capped so it does not
/// sprawl across an ultrawide; page padding tightens on narrow windows. The same
/// rules serve a 7-inch handheld at 1.5x and a 34-inch monitor at 1x.
/// </summary>
/// <summary>How Play's action buttons are told apart: colour only (companion-shell-layout.md, "Sizes").</summary>
internal enum ActionTone
{
    Tonal,
    Primary,
    Danger,
}

internal static class Ui
{
    public const double PageMaxWidth = Sizes.PageMaxWidth;
    public const double Gap = 12;

    /* ------------------------------------------------------------ tokens */

    /// <summary>Bind a property to a theme token, so it follows the theme variant.</summary>
    public static T Bind<T>(this T control, AvaloniaProperty property, string token)
        where T : Control
    {
        control.Bind(property, control.GetResourceObservable(token));
        return control;
    }

    /* -------------------------------------------------------------- text */

    /// <summary>
    /// A page's own name. Compact (the shell spec: no large title above the
    /// first card); the top bar already says which app and which adapter.
    /// </summary>
    public static TextBlock Title(string text = "") => new TextBlock
    {
        Text = text,
        FontSize = 20,
        FontWeight = FontWeight.SemiBold,
        TextWrapping = TextWrapping.Wrap,
    }.Bind(TextBlock.ForegroundProperty, Tokens.TextPrimary);

    public static TextBlock Heading(string text = "") => new TextBlock
    {
        Text = text,
        FontSize = 15,
        FontWeight = FontWeight.SemiBold,
        TextWrapping = TextWrapping.Wrap,
    }.Bind(TextBlock.ForegroundProperty, Tokens.TextPrimary);

    public static TextBlock Text(string? text = null, bool secondary = false) => new TextBlock
    {
        Text = text ?? string.Empty,
        FontSize = 14,
        TextWrapping = TextWrapping.Wrap,
    }.Bind(TextBlock.ForegroundProperty, secondary ? Tokens.TextSecondary : Tokens.TextPrimary);

    public static TextBlock Caption(string? text = null) => new TextBlock
    {
        Text = text ?? string.Empty,
        FontSize = 12.5,
        TextWrapping = TextWrapping.Wrap,
    }.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);

    /// <summary>
    /// A family list of common installed monospace faces, so the first one
    /// present wins on any distribution (and on Windows, for the screenshot harness).
    /// </summary>
    public static readonly FontFamily MonoFont = new("DejaVu Sans Mono, Liberation Mono, Noto Sans Mono, Cascadia Mono, Consolas, monospace");

    public static TextBlock Mono(string? text = null) => new TextBlock
    {
        Text = text ?? string.Empty,
        FontSize = 12.5,
        FontFamily = MonoFont,
        TextWrapping = TextWrapping.Wrap,
    }.Bind(TextBlock.ForegroundProperty, Tokens.TextPrimary);

    /* ------------------------------------------------------------- icons */

    public static PathIcon Icon(string geometry, double size = 18) => new PathIcon
    {
        Data = Geometry.Parse(geometry),
        Width = size,
        Height = size,
    };

    /* ----------------------------------------------------------- buttons */

    /// <summary>A button. The accessible name carries row identity where the label alone would repeat.</summary>
    public static Button Button(
        string label,
        EventHandler<RoutedEventArgs> click,
        string? accessibleName = null,
        bool accent = false,
        string? icon = null)
    {
        var button = new Button { Content = label };
        if (icon is not null)
        {
            button.Content = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
                Children = { Icon(icon, 16), new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center } },
            };
        }

        if (accent)
        {
            button.Classes.Add("accent");
        }

        AutomationProperties.SetName(button, accessibleName ?? label);
        button.Click += click;
        return button;
    }

    /// <summary>A square, icon-only button. Always named, because it has no text.</summary>
    public static Button IconButton(string icon, string accessibleName, EventHandler<RoutedEventArgs> click)
    {
        var button = new Button
        {
            Content = Icon(icon, 16),
            MinWidth = Sizes.ControlHeight,
            Width = Sizes.ControlHeight,
            Height = Sizes.ControlHeight,
            Padding = new Thickness(0),
        };
        AutomationProperties.SetName(button, accessibleName);
        ToolTip.SetTip(button, accessibleName);
        button.Click += click;
        return button;
    }

    /// <summary>Play's secondary actions and a status card's action (the scale's large height).</summary>
    public static Button LargeButton(
        string label,
        EventHandler<RoutedEventArgs> click,
        bool accent = false,
        string? icon = null)
    {
        var button = Button(label, click, accent: accent, icon: icon);
        button.Height = Sizes.LargeControlHeight;
        button.MinWidth = 160;
        button.Padding = new Thickness(18, 0);
        button.VerticalContentAlignment = VerticalAlignment.Center;
        return button;
    }

    /// <summary>
    /// Play's one action button (companion-shell-layout.md, "Sizes": 52 tall,
    /// radius 14): Start / Stop, Home, Capture, C, the quick actions, Present /
    /// Eject, Library. One size, told apart by colour only. It stretches to the
    /// width its row gives it and lifts under a controller (the "lift" style).
    /// </summary>
    public static Button ActionButton(string label, string icon, ActionTone tone = ActionTone.Tonal)
    {
        var text = new TextBlock
        {
            Text = label,
            FontSize = 15,
            FontWeight = FontWeight.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var button = new Button
        {
            Height = Sizes.ActionHeight,
            MinWidth = 0,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            CornerRadius = new CornerRadius(Sizes.ActionRadius),
            Padding = new Thickness(12, 0),
            // A label too long for its share of the row shrinks to fit, never
            // ends in an ellipsis ("Touch Gamep…" at the smallest window).
            Content = new Viewbox
            {
                Stretch = Stretch.Uniform,
                StretchDirection = StretchDirection.DownOnly,
                Child = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    Spacing = 8,
                    Children = { Icon(icon, 18), text },
                },
            },
        };
        button.Classes.Add("lift");
        SetActionTone(button, tone);
        AutomationProperties.SetName(button, label);
        return button;
    }

    /// <summary>Re-colour an action button (Start becomes Stop, Library becomes primary while a game waits).</summary>
    public static void SetActionTone(Button button, ActionTone tone)
    {
        button.Classes.Set("accent", tone == ActionTone.Primary);
        if (tone == ActionTone.Danger)
        {
            button.Background = ToneBrush(StatusTone.Bad);
            button.Foreground = Brushes.White;
        }
        else
        {
            button.ClearValue(TemplatedControl.BackgroundProperty);
            button.ClearValue(TemplatedControl.ForegroundProperty);
        }
    }

    /// <summary>Rewrite an action button's label (its accessible name follows).</summary>
    public static void SetActionLabel(Button button, string label, string icon)
    {
        if (button.Content is Viewbox { Child: StackPanel { Children: [PathIcon glyph, TextBlock text] } })
        {
            glyph.Data = Geometry.Parse(icon);
            text.Text = label;
        }

        AutomationProperties.SetName(button, label);
    }

    /// <summary>
    /// A dropdown that is a settings row's control: one fixed width, so the
    /// right-hand edge of every row lines up (Windows' CardSelectorStyle).
    /// </summary>
    public static ComboBox CardSelector(ComboBox box)
    {
        box.Width = Sizes.CardSelectorWidth;
        box.MinWidth = 0;
        return box;
    }

    /// <summary>The brush for a status dot. Which tone a state gets is decided in Services.</summary>
    public static IBrush ToneBrush(StatusTone tone) => new SolidColorBrush(tone switch
    {
        StatusTone.Good => Color.FromRgb(0x2E, 0xA0, 0x43),
        StatusTone.Busy => Color.FromRgb(0xD9, 0x8E, 0x04),
        StatusTone.Bad => Color.FromRgb(0xD1, 0x34, 0x38),
        _ => Color.FromRgb(0x8B, 0x8B, 0x94),
    });

    /// <summary>A button that opens a menu: where secondary and destructive commands go.</summary>
    public static Button MenuButton(string label, string accessibleName, params (string Label, Action Run)[] items)
    {
        var menu = new MenuFlyout { Placement = PlacementMode.BottomEdgeAlignedRight };
        foreach (var (text, run) in items)
        {
            var item = new MenuItem { Header = text };
            item.Click += (_, _) => run();
            menu.Items.Add(item);
        }

        var button = new Button { Content = label, Flyout = menu };
        AutomationProperties.SetName(button, accessibleName);
        return button;
    }

    /* ------------------------------------------------------------ layout */

    public static StackPanel Stack(params Control[] children) => Stack(10, children);

    public static StackPanel Stack(double spacing, params Control[] children)
    {
        var panel = new StackPanel { Spacing = spacing };
        panel.Children.AddRange(children);
        return panel;
    }

    /// <summary>Controls side by side, wrapping onto a second line rather than overflowing.</summary>
    public static WrapPanel Row(params Control[] children)
    {
        var panel = new WrapPanel
        {
            Orientation = Orientation.Horizontal,
            ItemSpacing = 8,
            LineSpacing = 8,
            VerticalAlignment = VerticalAlignment.Center,
        };
        panel.Children.AddRange(children);
        return panel;
    }

    /// <summary>
    /// The page: a header and its sections in a centred, capped column, in a
    /// vertical scroller. Horizontal scrolling is disabled deliberately: a page
    /// that scrolls sideways is a layout bug, and turning it off makes every
    /// child honour the width it is given.
    /// </summary>
    public static ScrollViewer Page(Control header, params Control[] sections) =>
        Page(Sizes.PageMaxWidth, header, sections);

    /// <summary>
    /// A page at a chosen column width (<see cref="Sizes.WidePageMaxWidth"/> for
    /// Play and Keyboard &amp; Mouse). The scroller is a LAST RESORT for windows
    /// too small for any layout (shell spec, "Fitting the screen"): a page that
    /// scrolls at an ordinary window size is a layout bug, and pages that would
    /// not fit are split into tabs instead.
    /// </summary>
    public static ScrollViewer Page(double maxWidth, Control header, params Control[] sections)
    {
        var column = new StackPanel { Spacing = Gap };
        column.Children.Add(header);
        column.Children.AddRange(sections);
        return new ScrollViewer
        {
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Content = new PageFrame(maxWidth) { Child = column },
        };
    }

    /// <summary>Title and subtitle on the left, the page's own actions on the right.</summary>
    public static Control PageHeader(TextBlock title, TextBlock? subtitle, params Control[] actions)
    {
        var text = Stack(4, title);
        if (subtitle is not null)
        {
            text.Children.Add(subtitle);
        }

        return new SettingRow(text, Row(actions), minimumLabelWidth: 280) { Margin = new Thickness(0, 4, 0, 4) };
    }

    /// <summary>
    /// A card of rows with an optional header. Rows are separated by hairlines
    /// that the stack draws itself, so a row that hides takes its separator
    /// with it and there is never a doubled line.
    /// </summary>
    public static Border Section(string? title, string? description, Control? action, params Control[] rows) =>
        Section(title is null ? null : Heading(title), description is null ? null : Caption(description), action, rows);

    public static Border Section(TextBlock? title, TextBlock? description, Control? action, params Control[] rows)
    {
        var stack = new RowStack();
        if (title is not null)
        {
            var text = Stack(2, title);
            if (description is not null)
            {
                text.Children.Add(description);
            }

            stack.Children.Add(new SettingRow(text, action ?? new Panel()) { Padding = new Thickness(18, 14) });
        }

        foreach (var row in rows)
        {
            stack.Children.Add(row);
        }

        return Card(stack);
    }

    /// <summary>A bare card. Prefer <see cref="Section"/>; this is for free-form content.</summary>
    public static Border Card(Control child) => new Border
    {
        BorderThickness = new Thickness(1),
        CornerRadius = new CornerRadius(10),
        ClipToBounds = true,
        Child = child,
    }.Bind(Border.BackgroundProperty, Tokens.Card).Bind(Border.BorderBrushProperty, Tokens.CardStroke);

    /// <summary>One setting: what it is on the left, how to change it on the right.</summary>
    public static SettingRow Setting(string label, string? description, params Control[] controls) =>
        Setting(Text(label), description is null ? null : Caption(description), controls);

    public static SettingRow Setting(TextBlock label, TextBlock? description, params Control[] controls)
    {
        var text = Stack(2, label);
        if (description is not null)
        {
            text.Children.Add(description);
        }

        return new SettingRow(text, Row(controls)) { Padding = new Thickness(18, 12) };
    }

    /// <summary>
    /// A setting whose label block also carries a live reason line: the static
    /// description says what the setting is, the reason says why it cannot be
    /// changed right now, and only the second ever hides.
    /// </summary>
    public static SettingRow Setting(string label, string? description, TextBlock reason, params Control[] controls)
    {
        var text = Stack(2, Text(label));
        if (description is not null)
        {
            text.Children.Add(Caption(description));
        }

        reason.Bind(TextBlock.ForegroundProperty, Tokens.TextTertiary);
        text.Children.Add(reason);
        return new SettingRow(text, Row(controls)) { Padding = new Thickness(18, 12) };
    }

    /// <summary>A right-hand value in a read-only row.</summary>
    public static TextBlock Value(string? text = null) => new TextBlock
    {
        Text = text ?? string.Empty,
        FontSize = 14,
        TextWrapping = TextWrapping.Wrap,
        TextAlignment = TextAlignment.Right,
        MaxWidth = 420,
    }.Bind(TextBlock.ForegroundProperty, Tokens.TextSecondary);

    /// <summary>Free content inside a section, with the row padding.</summary>
    public static Border Body(Control content) => new() { Padding = new Thickness(18, 14), Child = content };

    /// <summary>
    /// The state a page shows when it has nothing to show yet: what is missing,
    /// why, and the one action that fixes it. Replaces a page full of disabled
    /// controls, each repeating "connect an adapter to change this".
    /// </summary>
    public static Border EmptyState(string icon, TextBlock title, TextBlock detail, params Control[] actions)
    {
        title.FontSize = 20;
        title.TextAlignment = TextAlignment.Center;
        detail.TextAlignment = TextAlignment.Center;
        detail.MaxWidth = 460;

        var glyph = new Border
        {
            Width = 64,
            Height = 64,
            CornerRadius = new CornerRadius(32),
            HorizontalAlignment = HorizontalAlignment.Center,
            Child = Icon(icon, 28).Bind(PathIcon.ForegroundProperty, Tokens.AccentText),
        }.Bind(Border.BackgroundProperty, Tokens.AccentSoft);

        var row = Row(actions);
        row.HorizontalAlignment = HorizontalAlignment.Center;

        return Card(new StackPanel
        {
            Spacing = 12,
            Margin = new Thickness(24, 40),
            HorizontalAlignment = HorizontalAlignment.Center,
            Children = { glyph, title, detail, row },
        });
    }

    /* ------------------------------------------------------------- state */

    /// <summary>Show text, or collapse the block when there is none.</summary>
    public static void SetOptional(TextBlock block, string? text)
    {
        block.Text = text ?? string.Empty;
        block.IsVisible = !string.IsNullOrWhiteSpace(text);
    }

    /// <summary>
    /// Enable or disable a section and show its reason. The reason itself is
    /// decided in the projection; a disabled control without one is the state
    /// the projections exist to rule out.
    /// </summary>
    public static void Gate(SectionAvailability availability, TextBlock reason, params Control[] controls)
    {
        foreach (var control in controls)
        {
            control.IsEnabled = availability.Enabled;
        }

        SetOptional(reason, availability.DisabledReason);
    }

    public static IBrush Swatch(byte red, byte green, byte blue) =>
        new SolidColorBrush(Color.FromRgb(red, green, blue));
}

/// <summary>
/// The page column: centred, capped at <see cref="Ui.PageMaxWidth"/>, with
/// padding that tightens on narrow windows. Padding is decided from the width
/// actually offered, never from a device name.
/// </summary>
internal sealed class PageFrame(double maxWidth) : Decorator
{
    public PageFrame()
        : this(Sizes.PageMaxWidth)
    {
    }

    /// <summary>
    /// The short-window rhythm (<see cref="Sizes.ShortWindowHeight"/>): the
    /// same side gutters, a fraction of the vertical padding, and a tighter gap
    /// between sections, as Android's PageFrame does.
    /// </summary>
    public bool Short
    {
        get => isShort;
        set
        {
            isShort = value;
            if (Child is StackPanel column)
            {
                column.Spacing = value ? 8 : Ui.Gap;
            }

            InvalidateMeasure();
        }
    }

    private bool isShort;

    protected override Size MeasureOverride(Size availableSize)
    {
        var padding = availableSize.Width < 640
            ? new Thickness(14, 10, 14, 16)
            : Sizes.PagePadding;
        if (isShort)
        {
            padding = new Thickness(padding.Left, 4, padding.Right, 8);
        }

        if (Padding != padding)
        {
            Padding = padding;
        }

        if (Child is { } child)
        {
            child.MaxWidth = maxWidth;
            child.HorizontalAlignment = HorizontalAlignment.Center;
        }

        return base.MeasureOverride(availableSize);
    }
}

/// <summary>
/// A label block and a controls block, side by side when both fit and stacked
/// when they do not.
///
/// The deciding rule is width, measured: the controls keep their natural width
/// and the label must still get <c>minimumLabelWidth</c> beside them, otherwise
/// the controls move under the label, left-aligned. That is what lets the same
/// row read as a settings line on a monitor and as a compact form on a narrow
/// handheld window, with no breakpoint table anywhere.
/// </summary>
internal sealed class SettingRow : Panel
{
    private const double ColumnGap = 24;
    private const double StackGap = 10;
    private readonly double minimumLabelWidth;
    private bool stacked;

    public SettingRow(Control label, Control controls, double minimumLabelWidth = 220)
    {
        this.minimumLabelWidth = minimumLabelWidth;
        controls.VerticalAlignment = VerticalAlignment.Center;
        Children.Add(label);
        Children.Add(controls);
    }

    public static readonly StyledProperty<Thickness> PaddingProperty =
        Decorator.PaddingProperty.AddOwner<SettingRow>();

    public Thickness Padding
    {
        get => GetValue(PaddingProperty);
        set => SetValue(PaddingProperty, value);
    }

    static SettingRow() => AffectsMeasure<SettingRow>(PaddingProperty);

    private Control Label => Children[0];

    private Control Controls => Children[1];

    protected override Size MeasureOverride(Size availableSize)
    {
        var width = Math.Max(0, availableSize.Width - Padding.Left - Padding.Right);
        Controls.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var controls = Controls.IsVisible ? Controls.DesiredSize : default;
        var hasControls = controls.Width > 0;

        stacked = hasControls && double.IsFinite(width) && controls.Width + ColumnGap + minimumLabelWidth > width;

        Size label;
        double height;
        if (stacked)
        {
            Label.Measure(new Size(width, double.PositiveInfinity));
            Controls.Measure(new Size(width, double.PositiveInfinity));
            label = Label.DesiredSize;
            height = label.Height + StackGap + Controls.DesiredSize.Height;
        }
        else
        {
            var labelWidth = hasControls ? Math.Max(0, width - controls.Width - ColumnGap) : width;
            Label.Measure(new Size(labelWidth, double.PositiveInfinity));
            label = Label.DesiredSize;
            height = Math.Max(label.Height, controls.Height);
        }

        var desiredWidth = double.IsFinite(width) ? width : label.Width + controls.Width + ColumnGap;
        return new Size(
            desiredWidth + Padding.Left + Padding.Right,
            height + Padding.Top + Padding.Bottom);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        var left = Padding.Left;
        var top = Padding.Top;
        var width = Math.Max(0, finalSize.Width - Padding.Left - Padding.Right);
        var height = Math.Max(0, finalSize.Height - Padding.Top - Padding.Bottom);

        if (stacked)
        {
            var labelHeight = Label.DesiredSize.Height;
            Label.Arrange(new Rect(left, top, width, labelHeight));
            Controls.Arrange(new Rect(left, top + labelHeight + StackGap, width, Controls.DesiredSize.Height));
            return finalSize;
        }

        var controls = Controls.IsVisible ? Controls.DesiredSize : default;
        var labelWidth = controls.Width > 0 ? Math.Max(0, width - controls.Width - ColumnGap) : width;
        Label.Arrange(new Rect(left, top + ((height - Label.DesiredSize.Height) / 2), labelWidth, Label.DesiredSize.Height));
        Controls.Arrange(new Rect(left + width - controls.Width, top + ((height - controls.Height) / 2), controls.Width, controls.Height));
        return finalSize;
    }
}

/// <summary>
/// A vertical stack that draws a hairline between its VISIBLE children, so
/// hiding a row never leaves an orphaned or doubled separator.
///
/// The lines are painted by a private layer child, because Avalonia seals
/// <c>Panel.Render</c>; the layer is never measured as a row.
/// </summary>
internal sealed class RowStack : Panel
{
    private readonly DividerLayer layer = new();

    public RowStack()
    {
        layer.Bind(DividerLayer.BrushProperty, Tokens.Divider);
        Children.Add(layer);
    }

    private IEnumerable<Control> Rows => Children.Where(c => !ReferenceEquals(c, layer) && c.IsVisible);

    protected override Size MeasureOverride(Size availableSize)
    {
        double height = 0, width = 0;
        foreach (var child in Rows)
        {
            child.Measure(new Size(availableSize.Width, double.PositiveInfinity));
            height += child.DesiredSize.Height + 1;
            width = Math.Max(width, child.DesiredSize.Width);
        }

        layer.Measure(Size.Infinity);
        return new Size(double.IsFinite(availableSize.Width) ? availableSize.Width : width, Math.Max(0, height - 1));
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        double y = 0;
        var lines = new List<double>();
        foreach (var child in Rows)
        {
            if (y > 0)
            {
                lines.Add(y - 1);
            }

            child.Arrange(new Rect(0, y, finalSize.Width, child.DesiredSize.Height));
            y += child.DesiredSize.Height + 1;
        }

        layer.Lines = lines;
        layer.Arrange(new Rect(finalSize));
        layer.InvalidateVisual();
        return finalSize;
    }

    private sealed class DividerLayer : Control
    {
        public static readonly StyledProperty<IBrush?> BrushProperty =
            AvaloniaProperty.Register<DividerLayer, IBrush?>(nameof(Brush));

        static DividerLayer() => AffectsRender<DividerLayer>(BrushProperty);

        public DividerLayer() => IsHitTestVisible = false;

        public IBrush? Brush
        {
            get => GetValue(BrushProperty);
            set => SetValue(BrushProperty, value);
        }

        public IReadOnlyList<double> Lines { get; set; } = [];

        public override void Render(DrawingContext context)
        {
            if (Brush is null)
            {
                return;
            }

            foreach (var y in Lines)
            {
                context.FillRectangle(Brush, new Rect(0, y, Bounds.Width, 1));
            }
        }
    }
}

/// <summary>
/// The tab strip under a page header (Windows' SelectorBar): icon and label per
/// tab, the selected one marked by weight, a fill and an accent bar, never by
/// colour alone. Used where a page would otherwise not fit without scrolling
/// (Keyboard &amp; Mouse, Adapter).
/// </summary>
internal sealed class TabBar : StackPanel
{
    private readonly List<(Button Button, TextBlock Label, Border Indicator)> tabs = [];

    public TabBar(params (string Label, string? Icon)[] items)
    {
        Orientation = Orientation.Horizontal;
        Spacing = 4;
        for (var index = 0; index < items.Length; index++)
        {
            var (text, icon) = items[index];
            var label = new TextBlock { Text = text, FontSize = 14, VerticalAlignment = VerticalAlignment.Center };
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            if (icon is not null)
            {
                row.Children.Add(Ui.Icon(icon, 16));
            }

            row.Children.Add(label);
            var indicator = new Border
            {
                Width = 16,
                Height = 3,
                CornerRadius = new CornerRadius(2),
                HorizontalAlignment = HorizontalAlignment.Center,
                IsVisible = false,
            }.Bind(Border.BackgroundProperty, Tokens.Accent);

            var button = new Button
            {
                MinWidth = 0,
                Height = Sizes.ControlHeight + 8,
                Padding = new Thickness(14, 4, 14, 2),
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Content = new StackPanel { Spacing = 3, Children = { row, indicator } },
            };
            AutomationProperties.SetName(button, text);
            var at = index;
            button.Click += (_, _) => Select(at);
            tabs.Add((button, label, indicator));
            Children.Add(button);
        }
    }

    public event Action<int>? SelectionChanged;

    public int SelectedIndex { get; private set; } = -1;

    public void Select(int index, bool raise = true)
    {
        SelectedIndex = index;
        for (var i = 0; i < tabs.Count; i++)
        {
            var (button, label, indicator) = tabs[i];
            var selected = i == index;
            indicator.IsVisible = selected;
            label.FontWeight = selected ? FontWeight.SemiBold : FontWeight.Normal;
            if (selected)
            {
                button.Bind(TemplatedControl.BackgroundProperty, Tokens.Subtle);
            }
            else
            {
                button.Background = Brushes.Transparent;
            }
        }

        if (raise)
        {
            SelectionChanged?.Invoke(index);
        }
    }
}

internal enum NoticeSeverity
{
    Informational,
    Success,
    Warning,
    Error,
}

/// <summary>The InfoBar equivalent: a tinted, optionally dismissible message.</summary>
internal sealed class Notice : Border
{
    private readonly TextBlock title = new() { FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock message = new() { TextWrapping = TextWrapping.Wrap };
    private readonly PathIcon glyph = new() { Width = 18, Height = 18, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 1, 0, 0) };

    public Notice(bool dismissible = true)
    {
        BorderThickness = new Thickness(1);
        CornerRadius = new CornerRadius(8);
        Padding = new Thickness(14, 10);
        IsVisible = false;

        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), ColumnSpacing = 12 };
        grid.Children.Add(glyph);
        var text = new StackPanel { Spacing = 2, Children = { title, message }, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);
        if (dismissible)
        {
            var close = Ui.IconButton(Icons.Close, "Dismiss", (_, _) => IsVisible = false);
            close.Width = 28;
            close.MinWidth = 28;
            close.MinHeight = 28;
            close.Height = 28;
            close.Background = Brushes.Transparent;
            close.VerticalAlignment = VerticalAlignment.Top;
            Grid.SetColumn(close, 2);
            grid.Children.Add(close);
        }

        Child = grid;
        Severity = NoticeSeverity.Informational;
    }

    public NoticeSeverity Severity
    {
        set
        {
            var (hue, icon) = value switch
            {
                NoticeSeverity.Success => (Color.FromRgb(0x2E, 0xA0, 0x43), Icons.Check),
                NoticeSeverity.Warning => (Color.FromRgb(0xD9, 0x8E, 0x04), Icons.Warning),
                NoticeSeverity.Error => (Color.FromRgb(0xD1, 0x34, 0x38), Icons.Error),
                _ => (Color.FromRgb(0x5B, 0x66, 0xE8), Icons.Info),
            };
            BorderBrush = new SolidColorBrush(Color.FromArgb(0x66, hue.R, hue.G, hue.B));
            Background = new SolidColorBrush(Color.FromArgb(0x1F, hue.R, hue.G, hue.B));
            glyph.Data = Geometry.Parse(icon);
            glyph.Foreground = new SolidColorBrush(hue);
        }
    }

    public void Show(string? heading, string text, NoticeSeverity severity)
    {
        Severity = severity;
        Ui.SetOptional(title, heading);
        message.Text = text;
        IsVisible = true;
    }

    public void Hide() => IsVisible = false;
}
