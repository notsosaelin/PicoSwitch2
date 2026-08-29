using System.Globalization;
using System.Text;

namespace PicoSwitch.Companion.Windows.Bluetooth;

/// <summary>
/// The one conversion between WinRT's 48-bit <c>ulong</c> address and the
/// canonical colon-separated text every other layer uses.
///
/// WinRT hands out addresses as numbers (<c>BluetoothLEDevice.BluetoothAddress</c>,
/// <c>BluetoothLEAdvertisementReceivedEventArgs.BluetoothAddress</c>) while the
/// registry document, the diagnostic log and every user-visible short label use
/// text. Two independent conversions would eventually disagree about byte order,
/// and an adapter whose identity flips endianness is an adapter the app has
/// silently forgotten.
/// </summary>
public static class BluetoothAddressFormat
{
    /// <summary>Big-endian colon form, upper case: <c>AA:BB:CC:DD:EE:FF</c>. Null for 0 or out of range.</summary>
    public static string? ToText(ulong address)
    {
        if (address == 0 || address > 0xFFFF_FFFF_FFFFUL)
        {
            return null;
        }

        var text = new StringBuilder(17);
        for (var octet = 5; octet >= 0; octet--)
        {
            if (text.Length > 0)
            {
                text.Append(':');
            }

            text.Append(((address >> (octet * 8)) & 0xFF).ToString("X2", CultureInfo.InvariantCulture));
        }

        return text.ToString();
    }

    /// <summary>The inverse. False for anything that is not six hex octets.</summary>
    public static bool TryParse(string? text, out ulong address)
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

        ulong parsed = 0;
        foreach (var part in parts)
        {
            if (part.Length != 2 ||
                !byte.TryParse(part, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var octet))
            {
                return false;
            }

            parsed = (parsed << 8) | octet;
        }

        address = parsed;
        return true;
    }
}
