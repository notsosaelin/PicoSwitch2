using PicoSwitch.Companion.Services.Presentation;
using Windows.UI;

namespace PicoSwitch.Companion.App;

/// <summary>
/// The Windows brush for each <see cref="StatusTone"/>. What a state is called
/// and which tone it gets is decided once, in the shared
/// <see cref="Services.Presentation.AdapterStatus"/> and
/// <see cref="ControllerCardStatus"/>; only the colour is platform-local.
/// </summary>
internal static class StatusColors
{
    public static Color Of(StatusTone tone) => tone switch
    {
        StatusTone.Good => Color.FromArgb(0xFF, 0x2E, 0xA0, 0x43),
        StatusTone.Busy => Color.FromArgb(0xFF, 0xD9, 0x8E, 0x04),
        StatusTone.Bad => Color.FromArgb(0xFF, 0xD1, 0x34, 0x38),
        _ => Color.FromArgb(0xFF, 0x8B, 0x8B, 0x94),
    };
}
