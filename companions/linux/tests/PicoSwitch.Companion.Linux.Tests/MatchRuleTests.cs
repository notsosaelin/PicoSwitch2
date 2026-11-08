using Tmds.DBus.Protocol;
using Xunit;

namespace PicoSwitch.Companion.Linux.Tests;

/// <summary>
/// D-Bus match rules the backend may build. Regression guard for the first
/// hardware pairing attempt (Bazzite, dbus-broker, 2026-11-06), which failed
/// with "org.freedesktop.DBus.Error.MatchRuleInvalid".
/// </summary>
public sealed class MatchRuleTests
{
    /// <summary>
    /// Tmds.DBus.Protocol 0.21.3 writes <see cref="MatchRule.PathNamespace"/> as
    /// "pathNamespace=", but the D-Bus spec key is "path_namespace=", so the bus
    /// rejects the whole rule. Until the library spells it correctly, the backend
    /// must not set it. Once the library is fixed this test passes on its own and
    /// the property may be used again.
    /// </summary>
    [Fact]
    public void PathNamespaceIsNotUsedWhileTheLibraryMisspellsIt()
    {
        var wire = new MatchRule { Type = MessageType.Signal, PathNamespace = "/org/bluez/hci0" }.ToString();
        if (wire.Contains("path_namespace=", StringComparison.Ordinal))
        {
            return;
        }

        var offenders = Directory.EnumerateFiles(BackendSource(), "*.cs", SearchOption.AllDirectories)
            .Where(file => File.ReadLines(file).Any(line =>
                line.Contains("PathNamespace", StringComparison.Ordinal) &&
                !line.TrimStart().StartsWith("//", StringComparison.Ordinal)))
            .Select(Path.GetFileName)
            .ToList();
        Assert.True(offenders.Count == 0, $"MatchRule.PathNamespace serialises as \"{wire}\", which the bus rejects; used in: {string.Join(", ", offenders)}");
    }

    /// <summary>The keys the backend does use are written as the spec spells them.</summary>
    [Fact]
    public void UsedKeysMatchTheSpecSpelling()
    {
        var wire = new MatchRule
        {
            Type = MessageType.Signal,
            Path = "/org/bluez/hci0/dev_2C_CF_67_A1_B2_C3",
            Interface = "org.freedesktop.DBus.Properties",
            Member = "PropertiesChanged",
            Arg0 = "org.bluez.Device1",
        }.ToString();

        foreach (var key in new[] { "type=signal", "path=/org/bluez/hci0/dev_2C_CF_67_A1_B2_C3", "interface=org.freedesktop.DBus.Properties", "member=PropertiesChanged", "arg0=org.bluez.Device1" })
        {
            Assert.Contains(key, wire, StringComparison.Ordinal);
        }
    }

    private static string BackendSource()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, "src", "PicoSwitch.Companion.Linux");
            if (Directory.Exists(candidate))
            {
                return candidate;
            }
        }

        throw new DirectoryNotFoundException("companions/linux/src/PicoSwitch.Companion.Linux not found above the test output.");
    }
}
