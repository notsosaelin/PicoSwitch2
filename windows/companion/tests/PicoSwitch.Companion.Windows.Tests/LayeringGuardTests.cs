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
    public void NothingReintroducesTheRetiredAppContainerHost()
    {
        // Controller Link used to run as an out-of-process AppContainer app
        // service, because a HOGP peripheral role was thought to need one. That
        // was disproven on hardware -- an LE controller will not hold two
        // connections to one peer identity (0x0B ACL Connection Already Exists) --
        // and Path C carries controller state over the management link that is
        // already open instead.
        //
        // The research is preserved in docs/experiments; the runtime is not. This
        // guards the manifest specifically because an app service is invisible
        // from the app's own code: nothing would fail to compile if one came back,
        // and it would quietly re-acquire an out-of-process identity, a second
        // trust level, and a packaging surface the product no longer needs.
        var manifest = System.Xml.Linq.XDocument.Load(
            Path.Combine(CompanionRoot, "src", "PicoSwitch.Companion.App", "Package.appxmanifest"));

        Assert.Single(manifest.Descendants(), e => e.Name.LocalName == "Application");

        Assert.DoesNotContain(
            manifest.Descendants(),
            element => element.Name.LocalName == "AppService");

        Assert.DoesNotContain(
            manifest.Descendants(),
            element => element.Attribute("Category")?.Value == "windows.appService");
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

    [Fact]
    public void EveryXamlFileInTheAppIsWellFormedXml()
    {
        // The XAML compiler fails an XML-level parse error by EXITING 1 WITH NO OUTPUT:
        // no file, no line, no message, just `MSB3073 ... exited with code 1`. Cost a
        // bisect once, on a section-separator comment that contained a run of dashes —
        // which XML forbids inside a comment. This turns that silent failure into a named
        // one, and it is cheap enough to run on every file.
        var files = AppXamlFiles().ToList();

        Assert.NotEmpty(files);

        foreach (var file in files)
        {
            var problem = Record.Exception(() => System.Xml.Linq.XDocument.Load(file));
            Assert.True(problem is null, $"{Path.GetFileName(file)}: {problem?.Message}");
        }
    }

    [Fact]
    public void AGlyphOnlyButtonAlwaysCarriesAnAccessibleName()
    {
        // A ToolTip is not a name. Without AutomationProperties.Name a screen reader
        // announces a glyph button as "button", which makes a whole icon toolbar unusable
        // — and a sighted tester never notices, so §26.5's keyboard-first pass would go on
        // passing. Found exactly that way on the Touch Gamepad toolbar; kept as a rule.
        var offenders = new List<string>();

        foreach (var file in AppXamlFiles())
        {
            foreach (var element in System.Xml.Linq.XDocument.Load(file).Descendants())
            {
                if (element.Name.LocalName is not ("Button" or "ToggleButton" or
                    "AppBarButton" or "HyperlinkButton" or "RepeatButton"))
                {
                    continue;
                }

                var content = element.Attribute("Content")?.Value;

                // Segoe Fluent Icons live in the Unicode private use area, so a Content
                // that is one such character is a picture and nothing else.
                if (content is not { Length: 1 } || content[0] < '' || content[0] > '')
                {
                    continue;
                }

                if (element.Attribute("AutomationProperties.Name") is null)
                {
                    var name = element.Attributes()
                        .FirstOrDefault(attribute => attribute.Name.LocalName == "Name")?.Value;
                    offenders.Add($"{Path.GetFileName(file)}: {name ?? content}");
                }
            }
        }

        Assert.Empty(offenders);
    }

    // ------------------------------------------------------ the Touch Gamepad's surface

    /// <summary>
    /// The Touch Gamepad paints its own opaque ground.
    /// </summary>
    /// <remarks>
    /// The 2026-09-01 regression, as a rule. The surface set <c>Background</c> on the
    /// <c>UserControl</c> and left its root <c>Grid</c> unpainted, which paints nothing —
    /// and over a Mica window that meant the companion's navigation rail, the Gamepad
    /// page's cards and the status bars showed straight through the controller, with the
    /// desktop visible at the edges.
    ///
    /// Asserted on the ROOT ELEMENT specifically, because that is the one that has to
    /// carry it: a brush on any inner panel leaves the gaps between them transparent, and
    /// gaps are where the regression showed.
    /// </remarks>
    [Fact]
    public void TheTouchGamepadRootIsOpaque()
    {
        var root = TouchGamepadMarkup().Root!.Elements().First();

        Assert.Equal("Grid", root.Name.LocalName);
        Assert.Equal(
            "{ThemeResource SolidBackgroundFillColorBaseBrush}",
            root.Attribute("Background")?.Value);
    }

    /// <summary>
    /// Nothing on that surface is see-through by design.
    ///
    /// Acrylic, Mica and the Layer fills are all translucent materials: they are correct
    /// for chrome floating over an app and wrong for the ground under a controller. The
    /// scan covers the whole file rather than the root alone, because the previous version
    /// built its header and inspector out of Layer fill and those were the panels the page
    /// underneath showed through.
    /// </summary>
    [Fact]
    public void TheTouchGamepadUsesNoTranslucentMaterial()
    {
        // Attribute VALUES, not the file's text. The markup's own comment names these
        // brushes in order to forbid them, and a substring scan would fail on the
        // explanation rather than on a violation — the mirror image of the trap the
        // capability test above already documents.
        var offenders = TouchGamepadMarkup().Descendants()
            .SelectMany(element => element.Attributes())
            .Select(attribute => attribute.Value)
            .Where(value => new[]
                {
                    "Acrylic", "MicaBackdrop", "DesktopAcrylicBackdrop", "LayerFillColor",
                    "LayerOnAcrylicFillColor",
                }.Any(forbidden => value.Contains(forbidden, StringComparison.Ordinal)))
            .ToList();

        Assert.Empty(offenders);
    }

    /// <summary>
    /// Entering the mode REMOVES the shell rather than covering it.
    ///
    /// The Android companion returns early and never composes its scaffold; this is the
    /// same thing said in Windows terms. A surface layered over a live NavigationView is
    /// one mis-set brush away from the regression above, so the removal is the rule and
    /// the opacity is the belt.
    /// </summary>
    [Fact]
    public void EnteringTheTouchGamepadRemovesTheShell()
    {
        var shell = File.ReadAllText(Path.Combine(
            CompanionRoot, "src", "PicoSwitch.Companion.App", "MainWindow.xaml.cs"));

        var enter = shell[shell.IndexOf("public void ShowTouchGamepad", StringComparison.Ordinal)..];
        enter = enter[..enter.IndexOf("public void HideTouchGamepad", StringComparison.Ordinal)];

        Assert.Contains("Navigation.Visibility = Visibility.Collapsed", enter, StringComparison.Ordinal);
        Assert.Contains("SystemBackdrop = null", enter, StringComparison.Ordinal);
    }

    /// <summary>
    /// Full screen goes through the presenter, not through stretching something.
    ///
    /// <c>AppWindowPresenterKind.FullScreen</c> is the supported Windows App SDK answer and
    /// the only one that actually removes the title bar, the border and the caption
    /// buttons.
    /// </summary>
    [Fact]
    public void FullScreenUsesThePresenter()
    {
        var shell = File.ReadAllText(Path.Combine(
            CompanionRoot, "src", "PicoSwitch.Companion.App", "MainWindow.xaml.cs"));

        Assert.Contains("AppWindowPresenterKind.FullScreen", shell, StringComparison.Ordinal);
        Assert.Contains("AppWindow.SetPresenter", shell, StringComparison.Ordinal);
    }

    private static string TouchGamepadMarkupPath => Path.Combine(
        CompanionRoot, "src", "PicoSwitch.Companion.App", "Touch", "TouchGamepadView.xaml");

    private static System.Xml.Linq.XDocument TouchGamepadMarkup() =>
        System.Xml.Linq.XDocument.Load(TouchGamepadMarkupPath);

    private static IEnumerable<string> AppXamlFiles() => Directory
        .EnumerateFiles(
            Path.Combine(CompanionRoot, "src", "PicoSwitch.Companion.App"),
            "*.xaml",
            SearchOption.AllDirectories)
        .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
        .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"));

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
