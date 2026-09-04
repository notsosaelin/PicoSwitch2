<#
.SYNOPSIS
    Build and test the PicoSwitch2 Windows companion.

.DESCRIPTION
    Mirrors the repository's root build.ps1 convention: one entry point, explicit
    switches, no hidden state.

    The solution splits into two halves with DIFFERENT toolchain requirements,
    and this script keeps that split visible rather than letting a missing
    Visual Studio component look like a broken repository:

      -Core   (default)  every project except the WinUI shell. Builds and tests
                         with the standalone .NET SDK alone. This is the half
                         that carries the protocol contracts, so it is the half
                         CI must always run.

      -App               the unpackaged WinUI 3 shell. Needs the Windows App SDK
                         build components; see docs/README.md §4.

      -Zip               the distributable archive: the unpackaged app, zipped.
                         THIS IS THE ONE TO PUT ON GITHUB. It needs no
                         certificate and no administrator rights -- the user
                         extracts it and runs the exe.

                         Requires the .NET 9 Desktop Runtime on the user's
                         machine, which halves the download. Measured
                         2026-09-04, both pruned identically:

                           bundling it     280 files, 4 dirs, 65.9 MB zipped
                           requiring it     96 files, 4 dirs, 31.7 MB zipped

                         The Windows App SDK runtime is always BUNDLED. Dropping
                         that too would reach 42 files and 10.8 MB, but unlike
                         the .NET runtime it is not something a user is likely to
                         already have, and its absence is far harder for them to
                         diagnose.

      -SelfContained     with -Zip: bundle the .NET runtime as well, so the user
                         installs nothing at all. The 65.9 MB flavour.

      -Msix              the packaged flavour -- the installer. Requires .NET
                         Framework MSBuild (Visual Studio / Build Tools): the
                         packaging task loads `System.Security.Permissions`,
                         which is absent under the .NET SDK's own MSBuild.
                         Documented in docs/README.md §4.

                         Signs the package when credentials resolve, and produces
                         an UNSIGNED one when they do not. See Get-SigningValue.

.EXAMPLE
    ./build.ps1                     # build + test the core half
    ./build.ps1 -App                # additionally build the WinUI shell (x64)
    ./build.ps1 -App -Platform arm64
    ./build.ps1 -Zip -Configuration Release                  # the GitHub download
    ./build.ps1 -Zip -SelfContained -Configuration Release   # ...bundling .NET too
    ./build.ps1 -Msix               # signed if credentials resolve, unsigned otherwise
    ./build.ps1 -Core -App -Configuration Release
#>
[CmdletBinding()]
param(
    [switch]$Core,
    [switch]$App,
    [switch]$Msix,
    [switch]$Zip,
    [switch]$SelfContained,
    [switch]$NoTest,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'arm64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

# No switch given means the default: the core half.
if (-not $Core -and -not $App -and -not $Msix -and -not $Zip) { $Core = $true }

# -Zip publishes the app itself, so it does not also need the -App build; the
# two write to different directories and doing both would only double the work.

# ---------------------------------------------------------------- signing
#
# The Android arrangement, followed exactly (WINDOWS_PASS.md §27.2).
#
# Resolution order: `signing.properties` beside this script, then environment
# variables. Both are external by construction -- no certificate, password or
# thumbprint is ever stored in the repository, and both paths are gitignored.
#
# ABSENT CREDENTIALS ARE NOT AN ERROR. The package still builds and comes out
# unsigned. That keeps CI and ordinary contributors working while making it
# obvious that a publishable artifact needs the real certificate: a build
# failure here would tell a contributor their checkout is broken, which it is
# not.
$signingProperties = @{}
$signingFile = Join-Path $PSScriptRoot 'signing.properties'
if (Test-Path $signingFile) {
    foreach ($line in Get-Content -LiteralPath $signingFile) {
        $trimmed = $line.Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
        $split = $trimmed.IndexOf('=')
        if ($split -lt 1) { continue }
        $signingProperties[$trimmed.Substring(0, $split).Trim()] =
            $trimmed.Substring($split + 1).Trim()
    }
}

function Get-SigningValue {
    param([string]$Key, [string]$EnvName)
    if ($signingProperties.ContainsKey($Key) -and $signingProperties[$Key]) {
        return $signingProperties[$Key]
    }
    $value = [Environment]::GetEnvironmentVariable($EnvName)
    if ($value) { return $value }
    return $null
}

# Two ways to name a certificate, and they are mutually exclusive by design:
# a PFX on disk (with its password), or a thumbprint already installed in the
# machine's certificate store. A CI runner usually has the first; a signing
# workstation usually has the second.
$certFile       = Get-SigningValue 'certificateFile'       'PICOSWITCH_MSIX_CERT'
$certPassword   = Get-SigningValue 'certificatePassword'   'PICOSWITCH_MSIX_CERT_PASSWORD'
$certThumbprint = Get-SigningValue 'certificateThumbprint' 'PICOSWITCH_MSIX_THUMBPRINT'

if ($certFile -and -not [System.IO.Path]::IsPathRooted($certFile)) {
    $certFile = Join-Path $PSScriptRoot $certFile
}
if ($certFile -and -not (Test-Path -LiteralPath $certFile)) {
    Write-Host "signing: certificateFile '$certFile' does not exist; ignoring it" -ForegroundColor Yellow
    $certFile = $null
}

# Where a released .appinstaller will live. Absent means no .appinstaller is
# emitted at all -- an update manifest pointing at a URL nobody publishes to is
# worse than none, because App Installer will report the failure to the user as
# a broken app rather than as a missing release.
$appInstallerUrl = Get-SigningValue 'appInstallerUrl' 'PICOSWITCH_APPINSTALLER_URL'

$coreProjects = @(
    'src/PicoSwitch.Bridge.Core/PicoSwitch.Bridge.Core.csproj'
    'src/PicoSwitch.Management.Core/PicoSwitch.Management.Core.csproj'
    'src/PicoSwitch.Companion.Windows/PicoSwitch.Companion.Windows.csproj'
    'src/PicoSwitch.Companion.Services/PicoSwitch.Companion.Services.csproj'
)

$testProjects = @(
    'tests/PicoSwitch.Bridge.Core.Tests/PicoSwitch.Bridge.Core.Tests.csproj'
    'tests/PicoSwitch.Management.Core.Tests/PicoSwitch.Management.Core.Tests.csproj'
    'tests/PicoSwitch.Companion.Windows.Tests/PicoSwitch.Companion.Windows.Tests.csproj'
    'tests/PicoSwitch.Companion.Services.Tests/PicoSwitch.Companion.Services.Tests.csproj'
)

function Invoke-Dotnet {
    param([string[]]$Arguments)
    Write-Host "dotnet $($Arguments -join ' ')" -ForegroundColor DarkGray
    & dotnet @Arguments
    if ($LASTEXITCODE -ne 0) { throw "dotnet $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

function Find-FrameworkMsbuild {
    Get-ChildItem -Path @(
        "${env:ProgramFiles}\Microsoft Visual Studio"
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    ) -Recurse -Filter 'MSBuild.exe' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\Bin\amd64\MSBuild.exe' } |
        Select-Object -First 1 -ExpandProperty FullName
}

# Only MSIX packaging still needs .NET Framework MSBuild. The unpackaged app
# builds with the plain .NET SDK now that the C++/WinRT Controller Link host is
# gone -- see the HOGP retirement in docs/experiments.
$msbuild = $null
if ($Msix) {
    $msbuild = Find-FrameworkMsbuild
    if (-not $msbuild) {
        throw 'MSIX packaging needs .NET Framework MSBuild (Visual Studio / Build Tools); it was not found.'
    }
}

if ($Core) {
    foreach ($project in $coreProjects) {
        Invoke-Dotnet @('build', $project, '-c', $Configuration, '--nologo')
    }

    if (-not $NoTest) {
        foreach ($project in $testProjects) {
            Invoke-Dotnet @('test', $project, '-c', $Configuration, '--nologo')
        }
    }

    # The cross-language descriptor guard is part of "did this build stay
    # honest", not a separate chore: it is the only thing that catches a
    # descriptor edited in one language and not the others.
    Write-Host 'python ../../tools/check_android_descriptor_parity.py' -ForegroundColor DarkGray
    & python ../../tools/check_android_descriptor_parity.py
    if ($LASTEXITCODE -ne 0) { throw 'descriptor parity check failed' }
}

$appProject = 'src/PicoSwitch.Companion.App/PicoSwitch.Companion.App.csproj'

if ($App) {
    Invoke-Dotnet @(
        'build'
        $appProject
        '-c', $Configuration
        "-p:Platform=$Platform"
        '--nologo'
    )
}

# The update manifest App Installer polls (WINDOWS_PASS.md §27.4).
#
# Generated from the package rather than checked in, because two of its three
# facts -- the version and the architecture -- come from the build, and a
# hand-maintained copy would silently disagree with the package the moment
# either changed. The third, the URL, is deployment configuration and is the
# only thing a human supplies.
#
# The identity is read back out of the built package's own manifest, so this
# cannot drift from Package.appxmanifest either.
function New-AppInstaller {
    param($Package, [string]$Url)

    if (-not $Url) {
        Write-Host 'appinstaller: no appInstallerUrl configured; not generating one' -ForegroundColor DarkGray
        Write-Host '              set appInstallerUrl in signing.properties, or PICOSWITCH_APPINSTALLER_URL' -ForegroundColor DarkGray
        return
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Package.FullName)
    try {
        $entry = $zip.GetEntry('AppxManifest.xml')
        if (-not $entry) { throw "no AppxManifest.xml inside $($Package.Name)" }
        $reader = New-Object System.IO.StreamReader($entry.Open())
        try { $manifestXml = [xml]$reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $zip.Dispose() }

    $identity = $manifestXml.Package.Identity
    $base = $Url.TrimEnd('/')
    $target = Join-Path $Package.DirectoryName "$($identity.Name).appinstaller"

    # OnLaunch with HoursBetweenUpdateChecks=0 means "check every launch", which
    # is what a companion app tied to a firmware contract wants: an adapter and
    # a companion that disagree about the management protocol is exactly the
    # failure an update is meant to prevent.
    $doc = @"
<?xml version="1.0" encoding="utf-8"?>
<AppInstaller
    xmlns="http://schemas.microsoft.com/appx/appinstaller/2018"
    Uri="$base/$($identity.Name).appinstaller"
    Version="$($identity.Version)">
  <MainPackage
      Name="$($identity.Name)"
      Publisher="$($identity.Publisher)"
      Version="$($identity.Version)"
      ProcessorArchitecture="$($identity.ProcessorArchitecture)"
      Uri="$base/$($Package.Name)" />
  <UpdateSettings>
    <OnLaunch HoursBetweenUpdateChecks="0" />
  </UpdateSettings>
</AppInstaller>
"@

    Set-Content -LiteralPath $target -Value $doc -Encoding UTF8
    Write-Host "appinstaller: $target" -ForegroundColor Green
}

if ($Msix) {
    # Deliberately NOT `dotnet build`. The packaging task
    # (WinAppSdkValidateAppxManifestItems) loads System.Security.Permissions,
    # which the .NET SDK's MSBuild cannot resolve; .NET Framework MSBuild can.
    # See docs/README.md §4 -- this is a Windows App SDK packaging gap, not a
    # project defect, and hiding it behind a retry would only make it puzzling.
    $signingArgs = @()
    if ($certThumbprint) {
        Write-Host "signing: certificate store thumbprint $($certThumbprint.Substring(0, 8))..." -ForegroundColor DarkGray
        $signingArgs = @(
            '-p:AppxPackageSigningEnabled=true'
            "-p:PackageCertificateThumbprint=$certThumbprint"
        )
    } elseif ($certFile) {
        # Observed 2026-09-03, with a throwaway self-signed certificate: the
        # packaging task cannot import a PASSWORD-PROTECTED pfx and fails with
        #   APPX0105  Cannot import the key file ... may be password protected
        #   APPX0107  The certificate specified is not valid for signing
        # which reads like a bad certificate rather than a supported-input
        # problem. Said plainly here instead, because the thumbprint route below
        # is the documented answer and works.
        if ($certPassword) {
            throw @"
MSIX packaging cannot use a password-protected .pfx (it fails as APPX0105/APPX0107).

Import the certificate once, then sign by thumbprint:

  Import-PfxCertificate -FilePath '$certFile' -CertStoreLocation Cert:\CurrentUser\My ``
      -Password (Read-Host -AsSecureString)

then set certificateThumbprint in signing.properties (or PICOSWITCH_MSIX_THUMBPRINT)
and clear certificateFile/certificatePassword.
"@
        }
        Write-Host "signing: certificate file $certFile" -ForegroundColor DarkGray
        $signingArgs = @(
            '-p:AppxPackageSigningEnabled=true'
            "-p:PackageCertificateKeyFile=$certFile"
        )
        # Only pass a password when there is one: an empty value is not the same
        # as an absent one, and MSBuild treats it as a wrong password.
        if ($certPassword) { $signingArgs += "-p:PackageCertificatePassword=$certPassword" }
    } else {
        Write-Host 'signing: no credentials resolved -- the package will be UNSIGNED' -ForegroundColor Yellow
        Write-Host '         set certificateFile/certificateThumbprint in signing.properties,' -ForegroundColor DarkGray
        Write-Host '         or PICOSWITCH_MSIX_CERT / PICOSWITCH_MSIX_THUMBPRINT' -ForegroundColor DarkGray
        $signingArgs = @('-p:AppxPackageSigningEnabled=false')
    }

    Write-Host "$msbuild $appProject (MSIX)" -ForegroundColor DarkGray
    & $msbuild $appProject -restore -t:Build `
        "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
        -p:WindowsPackageType=MSIX -p:GenerateAppxPackageOnBuild=true `
        @signingArgs -v:m -nologo
    if ($LASTEXITCODE -ne 0) { throw "MSIX packaging failed with exit code $LASTEXITCODE" }

    $package = Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -Filter '*.msix' `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*$Configuration*" } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($package) {
        Write-Host "package: $($package.FullName)" -ForegroundColor Green
        New-AppInstaller -Package $package -Url $appInstallerUrl
    }
}

if ($Zip) {
    # The distributable archive: `dotnet publish`, pruned, zipped.
    #
    # WHY THIS AND NOT AN MSIX, for a free download: an MSIX cannot be installed
    # unsigned -- that is a platform rule, not a setting -- so it needs either a
    # paid CA certificate or a self-signed one. Self-signed means asking every
    # user to install a root certificate as Administrator before they can even
    # begin, which is a larger security ask than running an unsigned executable
    # and buys nothing: SmartScreen is driven by reputation, not by the mere
    # presence of a signature.
    #
    # WHY NOT A SINGLE FILE. Tried, and it does not work here (2026-09-04).
    # PublishSingleFile produces one 68 MB exe that dies on launch:
    #
    #   FileNotFoundException 0x8007007E at WinRT.ActivationFactory.Get
    #   -> Microsoft.Windows.AppLifecycle.AppInstance.FindOrRegisterForKey
    #
    # WinRT activation resolves the Windows App SDK's native DLLs by filename
    # beside the exe, and a bundle has no filenames. The obvious fix -- leave the
    # natives loose with IncludeNativeLibrariesForSelfExtract=false -- is refused
    # by the SDK's own Microsoft.WindowsAppSDK.SingleFile.targets, which requires
    # IncludeAllContentForSelfExtract=true. Both directions are closed, so the
    # loose-file layout is what this configuration supports.
    # THE .NET RUNTIME IS A PREREQUISITE, THE WINDOWS APP SDK IS NOT.
    #
    # They are separately switchable and the asymmetry is deliberate. The .NET 9
    # Desktop Runtime is one well-known Microsoft download that a lot of machines
    # already have, and when it is missing the apphost says so and links to it.
    # The Windows App SDK runtime is neither: users do not have it by default and
    # its absence is not self-explanatory, so it stays bundled.
    #
    # Measured 2026-09-04, all three pruned identically:
    #
    #   bundling both        280 files,  4 dirs, 166.6 MB -> 65.9 MB zipped
    #   requiring .NET        96 files,  4 dirs,  92.4 MB -> 31.7 MB zipped
    #   requiring both        42 files,  0 dirs,  38.7 MB -> 10.8 MB zipped
    #
    # The last was measured on a machine that HAS the Windows App SDK runtime
    # installed, so its size is trustworthy and its "it runs" is not evidence
    # about a clean machine.
    Invoke-Dotnet @(
        'publish', $appProject
        '-c', $Configuration
        "-p:Platform=$Platform"
        '-r', "win-$($Platform.ToLowerInvariant())"
        '-p:WindowsPackageType=None'
        "-p:SelfContained=$($SelfContained.IsPresent.ToString().ToLowerInvariant())"
        '-p:WindowsAppSDKSelfContained=true'
        '-p:DebugType=none'          # no .pdb: ~0.4 MB of no use to a user
        '--nologo'
    )

    $stage = Join-Path $PSScriptRoot "src/PicoSwitch.Companion.App/bin/$Platform/$Configuration/net9.0-windows10.0.22621.0/win-$($Platform.ToLowerInvariant())/publish"
    if (-not (Test-Path $stage)) { throw "no publish output at $stage" }

    $version = ([xml](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'src/PicoSwitch.Companion.App/Package.appxmanifest'))).Package.Identity.Version
    $archive = Join-Path $PSScriptRoot "PicoSwitch2-Companion-$version-$Platform.zip"
    if (Test-Path $archive) { Remove-Item -LiteralPath $archive -Force }

    # Staged under a NAMED folder, and the folder is what gets zipped, so
    # extracting produces one directory rather than emptying 278 files into
    # whatever the user unzipped into. Windows' own "Extract All" does not add a
    # containing folder, so the archive has to carry it.
    $root = Join-Path ([System.IO.Path]::GetTempPath()) "picoswitch-zip-$([guid]::NewGuid().ToString('N'))"
    $staging = Join-Path $root 'PicoSwitch2 Companion'
    try {
        New-Item -ItemType Directory -Path $root | Out-Null
        Copy-Item -LiteralPath $stage -Destination $staging -Recurse

        # The publish output is 492 files across 89 directories, and 85 of those
        # directories are WinUI's own localized strings -- two .mui files each,
        # for languages this app does not ship in. Removing all but the English
        # one, and the WinRT metadata (.winmd is a COMPILE-time input), takes it
        # to 280 files in 4 directories.
        #
        # Both prunes were verified 2026-09-04 by running the pruned folder, not
        # by reasoning about it: the window comes up.
        $langs = Get-ChildItem -LiteralPath $staging -Directory | Where-Object {
            $_.Name -ne 'en-us' -and
            (Get-ChildItem -LiteralPath $_.FullName -Filter '*.mui' -ErrorAction SilentlyContinue)
        }
        $langs | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }

        $winmd = Get-ChildItem -LiteralPath $staging -Recurse -Filter '*.winmd'
        $winmd | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

        Write-Host "pruned: $($langs.Count) language packs, $($winmd.Count) winmd" -ForegroundColor DarkGray

        Compress-Archive -Path $staging -DestinationPath $archive `
            -CompressionLevel Optimal

        $kept = Get-ChildItem -LiteralPath $staging -Recurse -File
        $dirs = Get-ChildItem -LiteralPath $staging -Recurse -Directory
        Write-Host "contents: $($kept.Count) files in $($dirs.Count) directories" -ForegroundColor DarkGray
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }

    $mb = [math]::Round((Get-Item -LiteralPath $archive).Length / 1MB, 1)
    Write-Host "archive: $archive ($mb MB)" -ForegroundColor Green
    if ($SelfContained) {
        Write-Host 'requires: nothing -- the .NET runtime is bundled' -ForegroundColor DarkGray
    } else {
        Write-Host 'requires: .NET 9 Desktop Runtime (x64) on the user machine' -ForegroundColor Yellow
        Write-Host '          https://dotnet.microsoft.com/download/dotnet/9.0' -ForegroundColor DarkGray
    }
}

Write-Host 'OK' -ForegroundColor Green
