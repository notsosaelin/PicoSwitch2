using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using PicoSwitch.Companion.Linux.App.Shell;
using PicoSwitch.Companion.Services;
using PicoSwitch.Companion.Services.Presentation;
using PicoSwitch.Management;

namespace PicoSwitch.Companion.Linux.App.Pages;

/// <summary>
/// Customization: how the emulated controller looks
/// (docs/architecture/companion-shell-layout.md, "What lives where"). A port of
/// the WinUI CustomizationPage. Today that is its colours; the clickable
/// controller drawing replaces the colour rows when it exists.
///
/// Paints <see cref="AppearanceSection"/> from <see cref="AdapterDashboard"/>
/// and decides nothing, like the Adapter page it came out of.
/// </summary>
internal sealed class CustomizationPage : CompanionPage
{
    private readonly AdapterConnectionService adapters = AppServices.Adapters;

    private readonly TextBlock appearanceReason = Ui.Text(secondary: true);
    private readonly TextBlock appearanceHint = Ui.Caption();
    private readonly Border bodySwatch = Swatch();
    private readonly Border leftSwatch = Swatch();
    private readonly Border rightSwatch = Swatch();
    private readonly Button bodyEdit;
    private readonly Button leftEdit;
    private readonly Button rightEdit;
    private readonly Button appearanceApply;

    private AppearanceSection? section;

    public CustomizationPage(MainWindow shell)
        : base(shell)
    {
        bodyEdit = Ui.Button("Change", async (_, _) => await EditColorAsync(ColorTarget.Body, section?.Body), "Change body colour");
        leftEdit = Ui.Button("Change", async (_, _) => await EditColorAsync(ColorTarget.LeftAccent, section?.LeftAccent), "Change left Joy-Con accent colour");
        rightEdit = Ui.Button("Change", async (_, _) => await EditColorAsync(ColorTarget.RightAccent, section?.RightAccent), "Change right Joy-Con accent colour");
        appearanceApply = Ui.Button("Show on the console", async (_, _) => await SafeAsync(async () =>
        {
            await adapters.ApplyAppearanceAsync();
            Report("The console now sees the new colours.", NoticeSeverity.Success);
        }, Render), accent: true);

        Content = Ui.Page(
            Ui.PageHeader(Ui.Title("Customization"), appearanceReason, Busy, appearanceApply),
            Operation,
            Ui.Section(
                "Colours",
                null,
                null,
                Ui.Setting("Body", null, bodySwatch, bodyEdit),
                Ui.Setting("Left Joy-Con accent", null, leftSwatch, leftEdit),
                Ui.Setting("Right Joy-Con accent", null, rightSwatch, rightEdit)),
            appearanceHint);
    }

    protected override void OnShown()
    {
        adapters.Connection.Changed += OnStateChanged;
        adapters.Snapshot.Changed += OnStateChanged;
        adapters.Registry.Changed += OnStateChanged;
        adapters.Relationship.Changed += OnStateChanged;
        Render();
    }

    protected override void OnHidden()
    {
        adapters.Connection.Changed -= OnStateChanged;
        adapters.Snapshot.Changed -= OnStateChanged;
        adapters.Registry.Changed -= OnStateChanged;
        adapters.Relationship.Changed -= OnStateChanged;
    }

    private void OnStateChanged() => AppServices.OnUiThread(Render);

    private void Render()
    {
        var registry = adapters.Registry.Value;
        var selected = registry.ActiveId is { } id ? registry.Record(id) : registry.Records.FirstOrDefault();
        var view = AdapterDashboard.Project(
            adapters.Snapshot.Value,
            adapters.Relationship.Value,
            adapters.Connection.Value,
            selected,
            AppServices.ControllerLinkStreaming);

        section = view.Appearance;
        appearanceHint.Text = section.ApplyHint;
        Paint(bodySwatch, section.Body);
        Paint(leftSwatch, section.LeftAccent);
        Paint(rightSwatch, section.RightAccent);
        Ui.Gate(section.Availability, appearanceReason, bodyEdit, leftEdit, rightEdit, appearanceApply);
    }

    private async Task EditColorAsync(ColorTarget target, RgbColor? current)
    {
        var start = current is { } value ? $"#{value.Red:X2}{value.Green:X2}{value.Blue:X2}" : "#000000";
        var preview = Swatch();
        preview.Width = preview.Height = Sizes.ControlHeight;
        preview.CornerRadius = new CornerRadius(Sizes.ControlHeight / 2);
        var field = new TextBox { Text = start, MaxLength = 7, Width = 160, Watermark = "#RRGGBB" };
        var error = Ui.Caption();

        void Preview()
        {
            if (Color.TryParse(field.Text ?? string.Empty, out var parsed))
            {
                preview.Background = Ui.Swatch(parsed.R, parsed.G, parsed.B);
                Ui.SetOptional(error, null);
            }
            else
            {
                Ui.SetOptional(error, "Enter a colour as #RRGGBB.");
            }
        }

        field.TextChanged += (_, _) => Preview();
        Preview();

        var choice = await Dialogs.ShowAsync(
            "Choose a colour",
            Ui.Stack(Ui.Row(preview, field), error),
            ["Set", "Cancel"],
            primaryIndex: 0,
            defaultIndex: 0);

        if (choice != 0 || !Color.TryParse(field.Text ?? string.Empty, out var chosen))
        {
            return;
        }

        await SafeAsync(() => adapters.SetColorAsync(target, new RgbColor(chosen.R, chosen.G, chosen.B)), Render);
    }

    private static Border Swatch() => new()
    {
        Width = 28,
        Height = 28,
        CornerRadius = new CornerRadius(14),
        BorderThickness = new Thickness(1),
        BorderBrush = new SolidColorBrush(Color.FromArgb(0x60, 0x80, 0x80, 0x80)),
        VerticalAlignment = VerticalAlignment.Center,
    };

    private static void Paint(Border swatch, RgbColor color) =>
        swatch.Background = Ui.Swatch((byte)color.Red, (byte)color.Green, (byte)color.Blue);
}
