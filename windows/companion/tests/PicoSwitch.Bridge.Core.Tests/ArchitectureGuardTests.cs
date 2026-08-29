using Xunit;

namespace PicoSwitch.Bridge.Tests;

/// <summary>
/// The C# twin of the Kotlin bridge-core <c>ArchitectureGuardTest</c>.
///
/// The target framework is the STRUCTURAL guard — <c>net9.0</c> means a Windows
/// API reference cannot compile. The source scans catch what a framework cannot:
/// report bytes composed outside the protocol layer, and platform vocabulary
/// leaking into a layer whose whole value is that it does not have any.
/// </summary>
public sealed class ArchitectureGuardTests
{
    private static readonly string ProjectRoot = SourceTree.ProjectDirectory("src/PicoSwitch.Bridge.Core");

    [Fact]
    public void BridgeCoreContainsNoWindowsPlatformReferences()
    {
        var source = SourceTree.ConcatenatedSources(ProjectRoot);
        foreach (var forbidden in new[]
                 {
                     "using Windows.",
                     "Windows.Devices",
                     "Windows.Gaming",
                     "Windows.Foundation",
                     "Microsoft.UI",
                     "WinRT",
                     "System.Windows",
                 })
        {
            Assert.DoesNotContain(forbidden, source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void ModuleTargetsThePortableFramework()
    {
        var lines = File.ReadAllLines(Path.Combine(ProjectRoot, "PicoSwitch.Bridge.Core.csproj"));
        var targetFramework = Assert.Single(
            lines,
            line => line.Contains("<TargetFramework>", StringComparison.Ordinal));
        Assert.Contains("$(PicoSwitchPortableTargetFramework)", targetFramework, StringComparison.Ordinal);

        // Scanned per line so the comment that EXPLAINS the rule does not trip it.
        Assert.DoesNotContain(
            lines,
            line => !line.TrimStart().StartsWith("//", StringComparison.Ordinal) &&
                    line.Contains("net9.0-windows", StringComparison.Ordinal) &&
                    line.Contains("<", StringComparison.Ordinal));
        Assert.DoesNotContain(
            lines,
            line => line.Contains("<PackageReference", StringComparison.Ordinal));
    }

    [Fact]
    public void BridgeCoreDoesNotDependOnManagementCore()
    {
        // Management and Controller Link are two INDEPENDENT relationships with the
        // same adapter. Conflating them is a documented past defect.
        var references = File.ReadAllLines(
                Path.Combine(ProjectRoot, "PicoSwitch.Bridge.Core.csproj"))
            .Where(line => line.Contains("<ProjectReference", StringComparison.Ordinal))
            .ToList();
        Assert.DoesNotContain(
            references,
            line => line.Contains("PicoSwitch.Management.Core", StringComparison.Ordinal));
    }

    [Fact]
    public void OnlyTheProtocolLayerKnowsTheWireLayout()
    {
        // `docs/bridge/PLATFORM_BACKEND.md` §4: platform backends never build
        // report bytes. Neither does the Core model -- there is exactly one
        // encoder, so a second, subtly different encoding cannot appear.
        var coreDirectory = Path.Combine(ProjectRoot, "Core");
        var core = SourceTree.ConcatenatedSources(coreDirectory);
        foreach (var forbidden in new[] { "PayloadSizeV2", "OffGyro", "FlagMotionValid", "0x85" })
        {
            Assert.DoesNotContain(forbidden, core, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void TheDescriptorExistsInExactlyOnePlace()
    {
        // A second copy is a copy that can drift, and the failure mode of a drifted
        // descriptor is silent: v1 input keeps working while every v2 capability
        // disappears at once.
        var sources = Directory
            .EnumerateFiles(ProjectRoot, "*.cs", SearchOption.AllDirectories)
            .Where(path => File.ReadAllText(path).Contains("0xA1, 0x01, 0x85", StringComparison.Ordinal))
            .Select(Path.GetFileName)
            .ToList();
        Assert.Equal(["BridgeHidDescriptor.cs"], sources);
    }

    [Fact]
    public void TheTwoFaceMappersAreNeverCollapsedIntoOne()
    {
        // Collapsing them broke both origins in turn: one shared mapper can only be
        // right for one origin at a time. The names are asserted so a "simplifying"
        // refactor has to delete a test rather than silently merge them.
        var layout = File.ReadAllText(Path.Combine(ProjectRoot, "Core", "ControllerLayout.cs"));
        Assert.Contains("MapPhysicalFaceKey", layout, StringComparison.Ordinal);
        Assert.Contains("MapTouchFacePosition", layout, StringComparison.Ordinal);
    }
}

/// <summary>Locates project sources relative to the test binary.</summary>
public static class SourceTree
{
    public static string CompanionRoot { get; } = Find("windows/companion/Directory.Build.props");

    public static string ProjectDirectory(string relativePath) =>
        Path.Combine(CompanionRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));

    public static string ConcatenatedSources(string directory) =>
        string.Join(
            "\n",
            Directory.EnumerateFiles(directory, "*.cs", SearchOption.AllDirectories)
                .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
                .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"))
                .Order(StringComparer.Ordinal)
                .Select(File.ReadAllText));

    private static string Find(string relativePath)
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            var candidate = Path.Combine(cursor.FullName, relativePath);
            if (File.Exists(candidate))
            {
                return Path.GetDirectoryName(candidate)!;
            }

            cursor = cursor.Parent;
        }

        throw new DirectoryNotFoundException(
            $"Cannot find {relativePath} above {AppContext.BaseDirectory}");
    }
}
