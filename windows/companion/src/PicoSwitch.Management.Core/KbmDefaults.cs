using System.Reflection;
using System.Text.Json;

namespace PicoSwitch.Management;

/// <summary>
/// The firmware's canonical default mapping for each layout, embedded.
/// </summary>
/// <remarks>
/// WHY THIS IS IN THE APP AT ALL.
///
/// A local profile stores only SPARSE OVERRIDES — the same representation the
/// adapter stores, which is what makes an assignment a copy rather than a
/// translation. To draw one, or to compute what a key actually does, those
/// overrides have to be applied against the canonical table they are sparse
/// against.
///
/// The page used to fetch that table from the adapter. That made creating and
/// editing a profile require a connection, which is wrong: the library belongs
/// to the user, not to a device, and the app should stay useful with nothing
/// paired — exactly as the Amiibo library does.
///
/// The table is generated from src/ns2_kbm.c by tools/test_ns2_kbm_commands.c
/// and embedded from tools/fixtures, so there is one authority and no second
/// hand-maintained copy. KbmDefaultsTests asserts the embedded copy still
/// matches what the firmware emits.
/// </remarks>
public static class KbmDefaults
{
    private const string ResourceName = "PicoSwitch.Management.KbmDefaultMappings.json";

    private static readonly Lazy<IReadOnlyDictionary<KbmLayout, KbmDefaultMapping>> Loaded =
        new(Load);

    /// <summary>The canonical mapping a layout starts from.</summary>
    public static KbmDefaultMapping For(KbmLayout layout) =>
        Loaded.Value.TryGetValue(layout, out var mapping)
            ? mapping
            : new KbmDefaultMapping(ValueList<KbmBinding>.Empty, new KbmMouseConfig());

    /// <summary>
    /// A layout's full effective mapping: the canonical table with a profile's
    /// overrides applied over it.
    /// </summary>
    /// <remarks>
    /// The one place the two halves are combined, so the grid, the fingerprint
    /// and an upload cannot disagree about what a profile means. An override
    /// whose destination is <see cref="KbmDestination.None"/> is kept rather than
    /// dropped: "this key does nothing" is a real answer and differs from the
    /// default the adapter would otherwise apply.
    /// </remarks>
    public static IReadOnlyList<KbmBinding> Effective(
        KbmLayout layout, IReadOnlyList<KbmBinding> overrides)
    {
        var byCode = new Dictionary<(KbmSourceKind Kind, int Code), KbmBinding>();
        foreach (var binding in For(layout).Bindings)
        {
            byCode[(binding.Source.Kind, binding.Source.Code)] = binding;
        }

        foreach (var binding in overrides)
        {
            byCode[(binding.Source.Kind, binding.Source.Code)] =
                binding with { Custom = true };
        }

        return [.. byCode.Values
                         .OrderBy(binding => KbmFingerprint.FirmwareCode(binding.Source.Kind))
                         .ThenBy(binding => binding.Source.Code)];
    }

    private static IReadOnlyDictionary<KbmLayout, KbmDefaultMapping> Load()
    {
        var result = new Dictionary<KbmLayout, KbmDefaultMapping>();
        try
        {
            using var stream = typeof(KbmDefaults).GetTypeInfo().Assembly
                .GetManifestResourceStream(ResourceName);
            if (stream is null)
            {
                return result;
            }

            using var document = JsonDocument.Parse(stream);
            if (!document.RootElement.TryGetProperty("layouts", out var layouts))
            {
                return result;
            }

            foreach (var entry in layouts.EnumerateObject())
            {
                var layout = KbmLayouts.FromWire(entry.Name);
                if (layout is null)
                {
                    continue;
                }

                var bindings = new List<KbmBinding>();
                if (entry.Value.TryGetProperty("bindings", out var rows))
                {
                    foreach (var row in rows.EnumerateArray())
                    {
                        var source = KbmSource.Parse(row.String("src") ?? string.Empty);
                        var destination = KbmDestinations.FromWire(row.String("dst"));
                        if (source is not null && destination is not null)
                        {
                            bindings.Add(new KbmBinding(source, destination.Value,
                                                        Custom: false));
                        }
                    }
                }

                var mouse = new KbmMouseConfig();
                if (entry.Value.TryGetProperty("mouse", out var m))
                {
                    mouse = new KbmMouseConfig(
                        SensitivityX: m.Int("sensitivityX"),
                        SensitivityY: m.Int("sensitivityY"),
                        VelocityWindowMs: m.Int("velocityWindowMs"),
                        InvertX: m.Bool("invertX"),
                        InvertY: m.Bool("invertY"),
                        AntiDeadzone: m.Int("antiDeadzone"));
                }

                result[layout.Value] = new KbmDefaultMapping(
                    new ValueList<KbmBinding>(bindings), mouse);
            }
        }
        catch (Exception)
        {
            // A build without the resource degrades to an empty table rather
            // than failing to start. The parity test is what stops that
            // happening silently in a shipped build.
        }

        return result;
    }
}

public sealed record KbmDefaultMapping(
    ValueList<KbmBinding> Bindings,
    KbmMouseConfig Mouse);
