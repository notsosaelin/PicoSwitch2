using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;

namespace PicoSwitch.Companion.Linux.App.Shell;

/// <summary>
/// The design tokens: every colour the shell paints, once per theme variant.
///
/// Pages never write a colour. They bind a token (<see cref="Ui.Bind"/>), so a
/// theme switch repaints the whole app and light and dark cannot drift into
/// two slightly different designs. The values follow the Windows 11 surface
/// ladder (window, rail, card) the WinUI companion uses, so the two desktop
/// apps read as one product; the accent is the indigo of the app icon.
/// </summary>
/// <summary>
/// THE APP'S SIZE SCALE, the same numbers as Windows' Styles/Sizes.xaml and
/// Android's Sizes (docs/architecture/companion-shell-layout.md, "Sizes"), in
/// Avalonia's device-independent pixels. Nothing on a page sets a control size
/// except through these; explicit sizes remain only where the size IS the
/// content (the drawn keyboard's keys, swatches, the dock).
/// </summary>
internal static class Sizes
{
    public const double ControlHeight = 36;
    public const double LargeControlHeight = 44;
    public const double HeroControlHeight = 76;
    public const double CommandMinWidth = 96;
    public const double SelectorMinWidth = 220;
    public const double CardSelectorWidth = 280;
    public const double PageMaxWidth = 1000;
    public const double WidePageMaxWidth = 1240;

    /// <summary>
    /// Under this window height a page takes its short-window arrangement (the
    /// shell spec, "Short windows"): tabs join the header row and the vertical
    /// rhythm tightens. The same 560 as Android's LayoutTokens.ShortWindowHeight,
    /// measured on the WINDOW, never chosen by device.
    /// </summary>
    public const double ShortWindowHeight = 560;
    public static readonly Thickness PagePadding = new(32, 12, 32, 24);

    /// <summary>Every action button on Play (Start / Stop, Home, Capture, C, quick actions, Present / Eject, Library).</summary>
    public const double ActionHeight = 52;
    public const double ActionRadius = 14;

    /// <summary>The navigation: 80 wide at the side, 64 tall at the top or bottom, items 52.</summary>
    public const double NavSideWidth = 80;
    public const double NavBarHeight = 64;
    public const double NavItemSize = 52;

    /// <summary>Below this window width a top / bottom navigation uses one-word names (never an ellipsis).</summary>
    public const double NavFullNamesMin = 720;
}

internal static class Tokens
{
    public const string Window = "ps.window";
    public const string Rail = "ps.rail";
    public const string Card = "ps.card";
    public const string CardStroke = "ps.card.stroke";
    public const string Divider = "ps.divider";
    public const string Subtle = "ps.subtle";
    public const string TextPrimary = "ps.text";
    public const string TextSecondary = "ps.text.secondary";
    public const string TextTertiary = "ps.text.tertiary";
    public const string Accent = "ps.accent";
    public const string AccentText = "ps.accent.text";
    public const string AccentSoft = "ps.accent.soft";
    public const string Scrim = "ps.scrim";

    private static readonly Color AccentLight = Color.Parse("#4F5BD5");
    private static readonly Color AccentDark = Color.Parse("#7E88FF");

    public static void Install(Application app)
    {
        app.Styles.Add(new FluentTheme
        {
            Palettes =
            {
                [ThemeVariant.Light] = new ColorPaletteResources { Accent = AccentLight },
                [ThemeVariant.Dark] = new ColorPaletteResources { Accent = Color.Parse("#5B66E8") },
            },
        });

        app.Resources.ThemeDictionaries[ThemeVariant.Light] = Palette(
            window: "#F3F3F5", rail: "#EBEBEF", card: "#FFFFFF", cardStroke: "#E3E3E8",
            divider: "#ECECF0", subtle: "#0F000000", text: "#1B1B1F", text2: "#5D5D66",
            text3: "#8B8B94", accent: AccentLight, accentText: AccentLight, scrim: "#66000000");
        app.Resources.ThemeDictionaries[ThemeVariant.Dark] = Palette(
            window: "#1A1A1E", rail: "#202025", card: "#26262C", cardStroke: "#33333B",
            divider: "#303037", subtle: "#14FFFFFF", text: "#F2F2F5", text2: "#A9A9B3",
            text3: "#777781", accent: Color.Parse("#5B66E8"), accentText: AccentDark, scrim: "#99000000");

        app.Styles.Add(Controls());
    }

    private static ResourceDictionary Palette(
        string window, string rail, string card, string cardStroke, string divider, string subtle,
        string text, string text2, string text3, Color accent, Color accentText, string scrim) => new()
    {
        [Window] = Brush(window),
        [Rail] = Brush(rail),
        [Card] = Brush(card),
        [CardStroke] = Brush(cardStroke),
        [Divider] = Brush(divider),
        [Subtle] = Brush(subtle),
        [TextPrimary] = Brush(text),
        [TextSecondary] = Brush(text2),
        [TextTertiary] = Brush(text3),
        [Accent] = new SolidColorBrush(accent),
        [AccentText] = new SolidColorBrush(accentText),
        [AccentSoft] = new SolidColorBrush(Color.FromArgb(0x26, accent.R, accent.G, accent.B)),
        [Scrim] = Brush(scrim),
    };

    private static SolidColorBrush Brush(string hex) => new(Color.Parse(hex));

    /// <summary>
    /// One size for every control family, so a button beside a combo box beside
    /// a text box sits on one baseline and one height. The previous shell let each
    /// control size itself, which is most of why rows looked uneven. The values
    /// are the shared scale in <see cref="Sizes"/>.
    /// </summary>
    private static Styles Controls()
    {
        const double height = Sizes.ControlHeight;
        var radius = new CornerRadius(6);
        return
        [
            new Style(x => x.OfType<Button>())
            {
                Setters =
                {
                    new Setter(Layoutable.MinHeightProperty, height),
                    new Setter(Layoutable.MinWidthProperty, Sizes.CommandMinWidth),
                    new Setter(TemplatedControl.PaddingProperty, new Thickness(14, 6)),
                    new Setter(TemplatedControl.CornerRadiusProperty, radius),
                    new Setter(ContentControl.HorizontalContentAlignmentProperty, Avalonia.Layout.HorizontalAlignment.Center),
                    new Setter(Layoutable.VerticalAlignmentProperty, Avalonia.Layout.VerticalAlignment.Center),
                },
            },
            new Style(x => x.OfType<ToggleButton>())
            {
                Setters = { new Setter(TemplatedControl.CornerRadiusProperty, radius) },
            },
            new Style(x => x.OfType<ComboBox>())
            {
                Setters =
                {
                    new Setter(Layoutable.MinHeightProperty, height),
                    new Setter(Layoutable.MinWidthProperty, Sizes.SelectorMinWidth),
                    new Setter(TemplatedControl.CornerRadiusProperty, radius),
                    new Setter(Layoutable.VerticalAlignmentProperty, Avalonia.Layout.VerticalAlignment.Center),
                },
            },
            new Style(x => x.OfType<TextBox>())
            {
                Setters =
                {
                    new Setter(Layoutable.MinHeightProperty, height),
                    new Setter(TemplatedControl.CornerRadiusProperty, radius),
                },
            },
            new Style(x => x.OfType<ProgressBar>())
            {
                Setters = { new Setter(Layoutable.MinWidthProperty, 0.0) },
            },

            // THE CONTROLLER CURSOR (companion-shell-layout.md, "Play"): Play's
            // tiles and actions and the navigation's items lift (scale and an
            // accent outline) only while a keyboard or gamepad drives focus.
            // :focus-visible is exactly "focused by keys", so a mouse click
            // never leaves a lifted tile behind. One outline, no second focus
            // rectangle on top of it.
            new Style(x => x.OfType<Button>().Class("lift"))
            {
                Setters =
                {
                    new Setter(Control.FocusAdornerProperty, null),
                    new Setter(Visual.RenderTransformOriginProperty, RelativePoint.Center),
                },
            },
            new Style(x => x.OfType<Button>().Class("lift").Class(":focus-visible"))
            {
                Setters =
                {
                    new Setter(Visual.RenderTransformProperty, Avalonia.Media.Transformation.TransformOperations.Parse("scale(1.05)")),
                    new Setter(TemplatedControl.BorderThicknessProperty, new Thickness(2)),
                    new Setter(TemplatedControl.BorderBrushProperty, new Avalonia.Markup.Xaml.MarkupExtensions.DynamicResourceExtension(Accent)),
                },
            },
        ];
    }
}
