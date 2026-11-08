using System.Globalization;
using System.Text;

namespace PicoSwitch.Companion.Linux.Bluetooth;

/// <summary>
/// BlueZ object paths and addresses.
///
/// BlueZ names a device object <c>{adapter}/dev_AA_BB_CC_DD_EE_FF</c> (address
/// most significant octet first, upper case), and exposes the same address as
/// the <c>Address</c> property in <c>AA:BB:CC:DD:EE:FF</c> form. The numeric
/// form is the companion's durable identifier, exactly as on Windows.
/// </summary>
internal static class BluezPaths
{
    public static string DevicePath(string adapterPath, ulong address)
    {
        var text = new StringBuilder(adapterPath.Length + 21);
        text.Append(adapterPath).Append("/dev");
        for (var octet = 5; octet >= 0; octet--)
        {
            text.Append('_').Append(((address >> (octet * 8)) & 0xFF).ToString("X2", CultureInfo.InvariantCulture));
        }

        return text.ToString();
    }

    /// <summary>Parse BlueZ's <c>Address</c> property. False for anything that is not six hex octets.</summary>
    public static bool TryParseAddress(string? text, out ulong address)
    {
        address = 0;
        if (text is null)
        {
            return false;
        }

        var parts = text.Split(':');
        if (parts.Length != 6)
        {
            return false;
        }

        foreach (var part in parts)
        {
            if (part.Length != 2 ||
                !byte.TryParse(part, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value))
            {
                address = 0;
                return false;
            }

            address = (address << 8) | value;
        }

        return address != 0;
    }

    /// <summary>True when <paramref name="path"/> is a direct child object of <paramref name="parent"/>.</summary>
    public static bool IsChildOf(string path, string parent) =>
        path.Length > parent.Length + 1 &&
        path.StartsWith(parent, StringComparison.Ordinal) &&
        path[parent.Length] == '/' &&
        path.IndexOf('/', parent.Length + 1) < 0;
}
