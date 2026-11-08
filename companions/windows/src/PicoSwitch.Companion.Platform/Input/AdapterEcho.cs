namespace PicoSwitch.Companion.Platform.Input;

/// <summary>
/// Noticing that a controller source might be the connected adapter's own USB
/// output looping back into this computer.
///
/// Shared by every platform's source catalog (extracted from the Windows one on
/// 2026-11-06 unchanged) so the guard means the same thing everywhere. It only
/// FLAGS a candidate (<see cref="ControllerSource.MayBeThisAdapter"/>); the
/// selection rules decide what that costs.
/// </summary>
public static class AdapterEcho
{
    /// <summary>
    /// Nintendo USB identities the adapter can emulate. Used ONLY to notice that
    /// a candidate might be the connected adapter's own output — never to hide
    /// anything, because a genuine controller carries the same identity.
    /// </summary>
    /// <remarks>
    /// Keys are the management wire names from <c>Personalities.WireName</c>,
    /// not invented ones — the adapter reports "jcl", not "joycon2l", and a
    /// guessed key would silently never match, which is the worst outcome here:
    /// the guard would look present and do nothing.
    /// </remarks>
    private static readonly IReadOnlyDictionary<string, ushort> PersonalityProducts =
        new Dictionary<string, ushort>(StringComparer.OrdinalIgnoreCase)
        {
            ["pro2"] = 0x2069,   // Pro Controller 2
            ["gc"] = 0x2073,     // NSO GameCube Controller
            ["jcl"] = 0x2067,    // Joy-Con 2 (L)
            ["jcr"] = 0x2066,    // Joy-Con 2 (R)
        };

    public const ushort NintendoVendor = 0x057E;

    /// <summary>
    /// The USB product id a management personality wire name emulates, or null
    /// when that name is not a controller personality. Exposed so a test can
    /// prove every personality is covered — a missing one makes the
    /// adapter-echo guard silently inert.
    /// </summary>
    public static ushort? PersonalityProductId(string? wireName) =>
        wireName is not null && PersonalityProducts.TryGetValue(wireName, out var product)
            ? product
            : null;

    /// <summary>True when a Nintendo identity matches the personality the adapter reports.</summary>
    public static bool MayBeThisAdapter(ushort vendor, ushort product, string? adapterPersonality)
    {
        if (vendor != NintendoVendor || string.IsNullOrWhiteSpace(adapterPersonality))
        {
            return false;
        }

        return PersonalityProducts.TryGetValue(adapterPersonality, out var expected) &&
               expected == product;
    }
}
