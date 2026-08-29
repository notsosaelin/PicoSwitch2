using System.Runtime.Versioning;

namespace PicoSwitch.Companion.Windows.Platform;

/// <summary>
/// OS-version gating for APIs above the supported floor.
///
/// The project's <c>TargetFramework</c> is <c>net9.0-windows10.0.22621.0</c>,
/// which is the SDK TARGETING version and is what makes newer WinRT types
/// visible. It is NOT the minimum supported OS — that is
/// <c>SupportedOSPlatformVersion</c>, currently 10.0.22000.0 (WINDOWS_PASS.md
/// §11.1, §28.1). Anything called above the floor has to be guarded, or the app
/// crashes on a machine it claims to support.
///
/// Centralised here so a guard is a named question with a documented answer
/// rather than a version literal buried at each call site.
/// </summary>
[SupportedOSPlatform("windows10.0.22000.0")]
public static class WindowsPlatform
{
    /// <summary>The minimum OS this application supports. Matches the csproj floor.</summary>
    public static readonly Version MinimumSupported = new(10, 0, 22000, 0);

    /// <summary>
    /// Windows 11 22H2 and later.
    ///
    /// The build the SDK targets. Anything only present from here must be behind
    /// this check, not assumed from the target framework.
    /// </summary>
    public static bool IsWindows11_22H2OrLater =>
        OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22621);

    /// <summary>
    /// The running OS version, for the support bundle.
    ///
    /// A bundle from a machine that has never connected still has to explain why
    /// (WINDOWS_PASS.md §31 Phase 7), and "this build predates the API that
    /// feature needs" is one of the answers it must be able to give.
    /// </summary>
    public static Version CurrentVersion => Environment.OSVersion.Version;

    public static bool MeetsMinimum => CurrentVersion >= MinimumSupported;
}
