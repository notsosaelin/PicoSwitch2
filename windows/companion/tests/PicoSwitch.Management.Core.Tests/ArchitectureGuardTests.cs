using Xunit;

namespace PicoSwitch.Management.Tests;

/// <summary>
/// The C# twin of the Kotlin <c>ArchitectureGuardTest</c>.
///
/// Two different mechanisms, deliberately kept together. The target framework is
/// the STRUCTURAL guard — <c>net9.0</c> means a Windows API reference cannot
/// compile, exactly as the missing Android SDK makes an Android import a build
/// failure in <c>:management-core</c>. The source scans catch what a classpath
/// cannot: presentation vocabulary leaking into the domain, and BLE carrier
/// mechanics leaking into the carrier-neutral protocol file.
/// </summary>
public sealed class ArchitectureGuardTests
{
    private static readonly string ProjectRoot =
        SourceTree.ProjectDirectory("src/PicoSwitch.Management.Core");

    [Fact]
    public void ManagementCoreContainsNoWindowsPlatformReferences()
    {
        var source = SourceTree.ConcatenatedSources(ProjectRoot);
        foreach (var forbidden in new[]
                 {
                     "using Windows.",
                     "Windows.Devices",
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
    public void ModuleTargetsThePortableFrameworkAndReferencesNothingWindows()
    {
        var lines = File.ReadAllLines(
            Path.Combine(ProjectRoot, "PicoSwitch.Management.Core.csproj"));
        var targetFramework = Assert.Single(
            lines,
            line => line.Contains("<TargetFramework>", StringComparison.Ordinal));
        Assert.Contains("$(PicoSwitchPortableTargetFramework)", targetFramework, StringComparison.Ordinal);

        // Scanned per element so a comment that EXPLAINS the rule does not trip it.
        var elements = lines
            .Where(line => line.Contains('<', StringComparison.Ordinal))
            .ToList();
        Assert.DoesNotContain(
            elements,
            line => line.Contains("net9.0-windows", StringComparison.Ordinal));
        Assert.DoesNotContain(
            elements,
            line => line.Contains("UseWindowsForms", StringComparison.Ordinal) ||
                    line.Contains("UseWPF", StringComparison.Ordinal));

        // The portable framework property itself must stay portable, or the guard
        // above would pass while every project silently gained the Windows SDK.
        var props = File.ReadAllText(
            Path.Combine(SourceTree.CompanionRoot, "Directory.Build.props"));
        Assert.Contains(
            "<PicoSwitchPortableTargetFramework>net9.0</PicoSwitchPortableTargetFramework>",
            props,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ManagementCoreDoesNotDependOnBridgeCore()
    {
        // Management and Controller Link are two INDEPENDENT relationships with
        // the same adapter. Conflating them is a documented past defect, not a
        // hypothetical one, and a project reference is how it would come back.
        // Scanned per line so the csproj comment that EXPLAINS this rule does not
        // trip it.
        var references = File.ReadAllLines(
                Path.Combine(ProjectRoot, "PicoSwitch.Management.Core.csproj"))
            .Where(line => line.Contains("<ProjectReference", StringComparison.Ordinal))
            .ToList();
        Assert.DoesNotContain(
            references,
            line => line.Contains("PicoSwitch.Bridge.Core", StringComparison.Ordinal));
    }

    [Fact]
    public void PortableDomainCarriesNoAppPresentationLabelsOrColorPacking()
    {
        var domain = File.ReadAllText(Path.Combine(ProjectRoot, "Domain.cs"));

        // Screen titles and ARGB packing belong to the App project. `PeerNaming`
        // stays here because a display-name PRECEDENCE rule is domain logic: it
        // decides which of several claims about a device is trustworthy, which is
        // not a rendering decision.
        Assert.DoesNotContain("string Title { get;", domain, StringComparison.Ordinal);
        Assert.DoesNotContain("Argb(", domain, StringComparison.Ordinal);
        Assert.DoesNotContain("0xFF000000", domain, StringComparison.Ordinal);
        Assert.Contains("PeerNaming", domain, StringComparison.Ordinal);
    }

    [Fact]
    public void LogicalProtocolFileContainsNoBleCarrierMechanics()
    {
        // The same logical newline/JSON contract is spoken over UART. Carrier
        // constants in this file would be a claim that it is a BLE protocol.
        var protocol = File.ReadAllText(Path.Combine(ProjectRoot, "ManagementProtocol.cs"));
        Assert.DoesNotContain("ServiceUuid", protocol, StringComparison.Ordinal);
        Assert.DoesNotContain("CommandChunks", protocol, StringComparison.Ordinal);
        Assert.DoesNotContain("MaxReplyPayloadBytes", protocol, StringComparison.Ordinal);
    }

    [Fact]
    public void TheCoreTakesNoPackageDependencies()
    {
        // A portable core with a NuGet dependency is one upgrade away from not
        // being portable. Everything it needs -- JSON, CRC-32 -- is either in the
        // BCL or written out in the project.
        var project = File.ReadAllText(
            Path.Combine(ProjectRoot, "PicoSwitch.Management.Core.csproj"));
        Assert.DoesNotContain("<PackageReference", project, StringComparison.Ordinal);
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
