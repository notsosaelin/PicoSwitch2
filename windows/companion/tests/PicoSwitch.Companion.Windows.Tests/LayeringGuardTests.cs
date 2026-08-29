using Xunit;

namespace PicoSwitch.Companion.Windows.Tests;

/// <summary>
/// The layering rules from WINDOWS_PASS.md §10.2, enforced as tests.
///
/// The two Core projects are guarded structurally by their target framework — a
/// Windows API reference there is a compile error. The rules THIS file covers
/// cannot be expressed that way, because every project it scans legitimately
/// targets Windows:
///
/// - the App project must not open a <c>GattCharacteristic</c>, build a
///   management command string, or compose report bytes (§12.5);
/// - the Services project must not contain XAML or WinUI types (§10.2).
///
/// The row people break is the first one. It is worth a test rather than a
/// review convention, because the failure it prevents is a second management
/// session opened from a ViewModel — the exact shape of the 2026-08-23 Android
/// defect that motivated <c>ManagementOwner</c>.
/// </summary>
public sealed class LayeringGuardTests
{
    private static readonly string CompanionRoot = FindCompanionRoot();

    [Fact]
    public void TheAppProjectNeverTouchesTheBluetoothStackDirectly()
    {
        var source = ConcatenatedSources("src/PicoSwitch.Companion.App");
        foreach (var forbidden in new[]
                 {
                     "Windows.Devices.Bluetooth",
                     "GattCharacteristic",
                     "GattDeviceService",
                     "GattServiceProvider",
                     "BluetoothLEDevice",
                     "BluetoothLEAdvertisementWatcher",
                 })
        {
            Assert.DoesNotContain(forbidden, source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void TheAppProjectNeverComposesAManagementCommandOrAReportByte()
    {
        // The UI calls AdapterRepository and nothing below it. A command string
        // built in a ViewModel is a protocol implementation nobody is testing.
        var source = ConcatenatedSources("src/PicoSwitch.Companion.App");
        foreach (var forbidden in new[]
                 {
                     "ManagementProtocol.",
                     "ManagementCommands.",
                     "ControllerReportEncoder.",
                     "BleManagementContract.",

                     // Nor may it name a transport IMPLEMENTATION. Composition
                     // lives in Services (AdapterConnectionService.CreateDefault)
                     // precisely so a page cannot reach the object that owns the
                     // GATT session -- which is one refactor away from opening a
                     // characteristic, and two away from a second live session.
                     "BleGattManagementTransport",
                     "IManagementTransport",
                 })
        {
            Assert.DoesNotContain(forbidden, source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void TheServicesProjectContainsNoPresentation()
    {
        // Services composes the platform layer and owns application-level state.
        // A WinUI type here would make that state impossible to test without a
        // XAML runtime, and impossible to reuse from anything but this shell.
        var source = ConcatenatedSources("src/PicoSwitch.Companion.Services");
        foreach (var forbidden in new[]
                 {
                     "Microsoft.UI.Xaml",
                     "Microsoft.UI.Windowing",
                     "DependencyObject",
                     "ObservableCollection",
                 })
        {
            Assert.DoesNotContain(forbidden, source, StringComparison.Ordinal);
        }

        Assert.Empty(Directory.EnumerateFiles(
            Path.Combine(CompanionRoot, "src", "PicoSwitch.Companion.Services"),
            "*.xaml",
            SearchOption.AllDirectories));
    }

    [Fact]
    public void OnlyTheWindowsProjectIsAllowedToReferenceWinRtBluetooth()
    {
        // Stated positively so the rule is discoverable from the guard: this is
        // the project the transport is SUPPOSED to live in, and nothing above it
        // may reach past it.
        var projects = new[]
        {
            "src/PicoSwitch.Bridge.Core",
            "src/PicoSwitch.Management.Core",
            "src/PicoSwitch.Companion.Services",
            "src/PicoSwitch.Companion.App",
        };

        foreach (var project in projects)
        {
            Assert.DoesNotContain(
                "Windows.Devices.Bluetooth",
                ConcatenatedSources(project),
                StringComparison.Ordinal);
        }
    }

    [Fact]
    public void TheAppDeclaresExactlyTheCapabilitiesItNeeds()
    {
        // There is deliberately no internetClient: the app talks to an adapter
        // over Bluetooth and to nothing else, and the permanent Wi-Fi prohibition
        // applies to this host too. No broad file-system capability either.
        //
        // `runFullTrust` is here because MakeAppx REQUIRES it, not because the app
        // wants a privilege. WINDOWS_PASS.md §27.3 specifies "exactly `bluetooth`,
        // nothing else"; that was written before the manifest had been packaged,
        // and a WinUI 3 desktop app declaring Executable/EntryPoint is rejected
        // without it (`error 80080204`). It marks the package as a full-trust
        // Win32 app rather than a UWP one and does NOT imply elevation, so §27.5
        // is untouched. Asserted as an exact set so a third capability cannot be
        // added quietly on the back of this one.
        //
        // Read as XML rather than as text so the comment that EXPLAINS the rule
        // does not satisfy it — a substring scan here would pass on a manifest
        // that merely talks about capabilities.
        var manifest = System.Xml.Linq.XDocument.Load(
            Path.Combine(CompanionRoot, "src", "PicoSwitch.Companion.App", "Package.appxmanifest"));
        var declared = manifest.Descendants()
            .Where(element => element.Name.LocalName is "Capability" or "DeviceCapability")
            .Select(element => element.Attribute("Name")?.Value)
            .Order(StringComparer.Ordinal)
            .ToList();

        Assert.Equal(["bluetooth", "runFullTrust"], declared);
    }

    [Fact]
    public void SingleInstanceActivationIsWiredBeforeTheXamlRuntimeStarts()
    {
        // A second PROCESS is the Windows form of the stacked-Activity defect: two
        // processes would hold two GATT sessions to an adapter that admits exactly
        // one management client. Only redirecting activation prevents that, and it
        // has to happen before Application.Start.
        var app = File.ReadAllText(
            Path.Combine(CompanionRoot, "src", "PicoSwitch.Companion.App", "App.xaml.cs"));
        Assert.Contains("FindOrRegisterForKey", app, StringComparison.Ordinal);
        Assert.Contains("RedirectActivationToAsync", app, StringComparison.Ordinal);

        var project = File.ReadAllText(Path.Combine(
            CompanionRoot, "src", "PicoSwitch.Companion.App", "PicoSwitch.Companion.App.csproj"));
        Assert.Contains("DISABLE_XAML_GENERATED_MAIN", project, StringComparison.Ordinal);
    }

    private static string ConcatenatedSources(string relativeProject)
    {
        var directory = Path.Combine(
            CompanionRoot,
            relativeProject.Replace('/', Path.DirectorySeparatorChar));
        return string.Join(
            "\n",
            Directory.EnumerateFiles(directory, "*.cs", SearchOption.AllDirectories)
                .Concat(Directory.EnumerateFiles(directory, "*.xaml", SearchOption.AllDirectories))
                .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
                .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"))
                .Order(StringComparer.Ordinal)
                .Select(File.ReadAllText));
    }

    private static string FindCompanionRoot()
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            var candidate = Path.Combine(cursor.FullName, "windows", "companion", "Directory.Build.props");
            if (File.Exists(candidate))
            {
                return Path.GetDirectoryName(candidate)!;
            }

            cursor = cursor.Parent;
        }

        throw new DirectoryNotFoundException(
            $"Cannot find windows/companion above {AppContext.BaseDirectory}");
    }
}

public sealed class WindowsPlatformTests
{
    [Fact]
    public void TheSupportedFloorMatchesTheProjectSetting()
    {
        // The floor is 10.0.22000.0 and the SDK targeting version is higher. The
        // two are different questions and must not be conflated: the second is
        // what makes APIs visible, the first is what the app promises to run on.
        var props = File.ReadAllText(Path.Combine(
            FindCompanionRoot(), "Directory.Build.props"));
        Assert.Contains(
            "<PicoSwitchSupportedOSPlatformVersion>10.0.22000.0</PicoSwitchSupportedOSPlatformVersion>",
            props,
            StringComparison.Ordinal);
        Assert.Equal(
            new Version(10, 0, 22000, 0),
            Companion.Windows.Platform.WindowsPlatform.MinimumSupported);
    }

    [Fact]
    public void TheRunningMachineIsDescribedRatherThanAssumed()
    {
        // A support bundle from a machine that has never connected still has to
        // explain why, and "this build predates the API that feature needs" is one
        // of the answers it must be able to give.
        Assert.True(Companion.Windows.Platform.WindowsPlatform.CurrentVersion.Major >= 6);
        Assert.Equal(
            Companion.Windows.Platform.WindowsPlatform.CurrentVersion >=
            Companion.Windows.Platform.WindowsPlatform.MinimumSupported,
            Companion.Windows.Platform.WindowsPlatform.MeetsMinimum);
    }

    private static string FindCompanionRoot()
    {
        var cursor = new DirectoryInfo(AppContext.BaseDirectory);
        while (cursor is not null)
        {
            var candidate = Path.Combine(cursor.FullName, "windows", "companion", "Directory.Build.props");
            if (File.Exists(candidate))
            {
                return Path.GetDirectoryName(candidate)!;
            }

            cursor = cursor.Parent;
        }

        throw new DirectoryNotFoundException("Cannot find windows/companion");
    }
}
