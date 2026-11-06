namespace PicoSwitch.Management;

/// <summary>
/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) — the same value
/// <c>java.util.zip.CRC32</c> produces, which is what the Kotlin client sends in
/// <c>amiibo begin</c> and checks a download against.
///
/// Implemented here rather than taken from a package so
/// <c>PicoSwitch.Management.Core</c> stays dependency-free: this project's whole
/// job is to be the portable half, and a NuGet reference in it is the first step
/// toward that not being true.
/// </summary>
public static class Crc32
{
    private static readonly uint[] Table = BuildTable();

    public static uint Compute(ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var value in data)
        {
            crc = (crc >> 8) ^ Table[(crc ^ value) & 0xFF];
        }

        return crc ^ 0xFFFFFFFFu;
    }

    /// <summary>Eight upper-case hex digits, the form the management protocol carries.</summary>
    public static string Hex(ReadOnlySpan<byte> data) => $"{Compute(data):X8}";

    private static uint[] BuildTable()
    {
        var table = new uint[256];
        for (var index = 0u; index < table.Length; index++)
        {
            var value = index;
            for (var bit = 0; bit < 8; bit++)
            {
                value = (value & 1) != 0 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            }

            table[index] = value;
        }

        return table;
    }
}
