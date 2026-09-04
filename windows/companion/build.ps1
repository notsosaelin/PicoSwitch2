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

      -Zip               the PORTABLE release artifact: the unpackaged app,
                         zipped, extracting to one folder. No installer, no
                         administrator rights, no system changes.

                         SELF-CONTAINED by default, which is the whole promise of
                         a portable build: extract and run, with no prerequisite
                         to chase. It is the larger of the two downloads and that
                         is the trade being offered.

      -Installer         the INSTALLER release artifact, a single Setup .exe.
                         Framework-dependent on .NET, so the payload is a third
                         the size, and the installer detects the runtime and
                         fetches Microsoft's own redistributable when it is
                         missing. Needs Inno Setup 6.3+.

      -ReleaseArtifacts  both of the above, plus SHA256SUMS.txt, into artifacts/.
                         This is what a GitHub release is made from.

      -FrameworkDependent
                         with -Zip: require the .NET runtime instead of bundling
                         it. Measured 2026-09-04, both pruned identically:

                           bundling it     280 files, 4 dirs, 65.9 MB zipped
                           requiring it     96 files, 4 dirs, 31.7 MB zipped

                         Not the default for the portable build: a portable
                         archive that stops with "install a runtime first" is not
                         portable. The installer takes that trade instead,
                         because an installer can resolve it for the user.

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
    ./build.ps1 -ReleaseArtifacts -Configuration Release     # both, for a GitHub release
    ./build.ps1 -Zip -Configuration Release                  # portable only
    ./build.ps1 -Installer -Configuration Release            # installer only
    ./build.ps1 -Msix               # signed if credentials resolve, unsigned otherwise
    ./build.ps1 -Core -App -Configuration Release
#>
[CmdletBinding()]
param(
    [switch]$Core,
    [switch]$App,
    [switch]$Msix,
    [switch]$Zip,
    [switch]$Installer,
    [switch]$ReleaseArtifacts,
    [switch]$FrameworkDependent,
    [switch]$NoTest,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'arm64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

# -ReleaseArtifacts is the two release outputs together, and the version, hashes
# and artifacts directory only make sense when both were produced from one run.
if ($ReleaseArtifacts) { $Zip = $true; $Installer = $true }

# No switch given means the default: the core half.
if (-not $Core -and -not $App -and -not $Msix -and -not $Zip -and -not $Installer) { $Core = $true }

# -Zip and -Installer publish the app themselves, so neither also needs the -App
# build; they write to different directories and doing both would only duplicate
# the work.

# Where release artifacts land. Deliberately NOT the publish tree: a GitHub
# release directory should hold the two files a user downloads and nothing else
# -- no Debug output, no PDBs, no AppPackages, no MSIX.
$artifactsDir = Join-Path $PSScriptRoot 'artifacts'

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
    # Out-Host, not bare output: called from inside a function that RETURNS a
    # value, dotnet's stdout would otherwise be part of that return value and the
    # caller would get an array of build log lines with the real result at the
    # end of it.
    & dotnet @Arguments | Out-Host
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

# THE authoritative product version: Package.appxmanifest's Identity.
#
# One source, read here and passed to everything downstream -- the archive name,
# the installer name, and the installer's own AppVersion. Duplicating it into a
# script is how an installer ends up claiming a version the binaries do not have.
function Get-ProductVersion {
    $manifest = Join-Path $PSScriptRoot 'src/PicoSwitch.Companion.App/Package.appxmanifest'
    ([xml](Get-Content -LiteralPath $manifest)).Package.Identity.Version
}

# The same version as the release is tagged with.
#
# The manifest must carry four parts -- that is the MSIX identity format -- but a
# release is tagged v2.5.0, so an artifact called ...-2.5.0.0-x64.exe sitting
# under it reads like a different build. The fourth component is dropped for
# FILENAMES only; the installer's own AppVersion keeps all four, because
# VersionInfoVersion is a numeric Windows resource.
function Get-DisplayVersion {
    $parts = (Get-ProductVersion) -split '\.'
    if ($parts.Count -eq 4 -and $parts[3] -eq '0') {
        return ($parts[0..2] -join '.')
    }
    return (Get-ProductVersion)
}

# Inno Setup's command-line compiler.
#
# Registry first, because that is where an install of either scope records
# itself and it is the only mechanism that survives the user choosing a
# non-default directory. The fixed paths are a fallback for a copy that was
# unpacked rather than installed. winget installs per-user by default when it
# runs unelevated, which is why HKCU is checked before HKLM.
function Find-InnoSetup {
    foreach ($key in @(
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1'
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1'
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1'
    )) {
        $location = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).InstallLocation
        if ($location) {
            $candidate = Join-Path $location 'ISCC.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }

    foreach ($candidate in @(
        "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    $onPath = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    return $null
}

# One publish, used by both release artifacts.
#
# The two differ in exactly one property -- whether the .NET runtime is carried
# -- so they share everything else rather than drifting into two recipes that
# produce subtly different applications.
function Publish-App {
    param([bool]$Bundled)

    Invoke-Dotnet @(
        'publish', $appProject
        '-c', $Configuration
        "-p:Platform=$Platform"
        '-r', "win-$($Platform.ToLowerInvariant())"
        '-p:WindowsPackageType=None'
        "-p:SelfContained=$($Bundled.ToString().ToLowerInvariant())"
        '-p:WindowsAppSDKSelfContained=true'
        '-p:DebugType=none'          # no .pdb: of no use to a user
        '--nologo'
    )

    $published = Join-Path $PSScriptRoot "src/PicoSwitch.Companion.App/bin/$Platform/$Configuration/net9.0-windows10.0.22621.0/win-$($Platform.ToLowerInvariant())/publish"
    if (-not (Test-Path $published)) { throw "no publish output at $published" }
    return $published
}

# Copy a publish tree to $Destination, dropping what a user never needs.
#
# `dotnet publish` emits a directory per WinUI language -- af-ZA through zh-TW,
# two .mui files each -- for languages this app does not ship in, plus the
# .winmd WinRT metadata, which is a COMPILE-time input. Removing both takes the
# self-contained tree from 492 files across 89 directories to 280 across 4.
#
# Both prunes were verified 2026-09-04 by RUNNING the pruned folder rather than
# by reasoning about it. Nothing else is removed: PublishTrimmed is deliberately
# not used, because WinUI's XAML loading is reflection-heavy and a trimmed build
# fails on the page nobody tested.
function Copy-PrunedPayload {
    param([string]$From, [string]$Destination)

    Copy-Item -LiteralPath $From -Destination $Destination -Recurse

    $langs = Get-ChildItem -LiteralPath $Destination -Directory | Where-Object {
        $_.Name -ne 'en-us' -and
        (Get-ChildItem -LiteralPath $_.FullName -Filter '*.mui' -ErrorAction SilentlyContinue)
    }
    $langs | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }

    $winmd = Get-ChildItem -LiteralPath $Destination -Recurse -Filter '*.winmd'
    $winmd | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }

    $files = Get-ChildItem -LiteralPath $Destination -Recurse -File
    $dirs = Get-ChildItem -LiteralPath $Destination -Recurse -Directory
    Write-Host "  pruned $($langs.Count) language packs and $($winmd.Count) winmd" -ForegroundColor DarkGray
    Write-Host "  payload: $($files.Count) files in $($dirs.Count) directories, $([math]::Round((($files | Measure-Object -Property Length -Sum).Sum) / 1MB, 1)) MB" -ForegroundColor DarkGray
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

# ------------------------------------------------------------- release artifacts
#
# TWO artifacts, deliberately using DIFFERENT runtime models, because they are
# offering the user different trades:
#
#   installer   framework-dependent .NET. A third the payload, and the installer
#               resolves the missing runtime itself -- which an installer can do
#               and an archive cannot.
#
#   portable    self-contained. Larger, and in exchange it needs nothing, asks
#               nothing, and changes nothing on the machine. An archive that
#               stopped with "install a runtime first" would not be portable.
#
# The Windows App SDK runtime is bundled in BOTH. Making it a prerequisite too
# would reach 42 files and 10.8 MB, but users do not have it by default and its
# absence is not self-explanatory the way a missing .NET runtime is.
#
# Measured 2026-09-04, all pruned identically:
#
#   bundling both        280 files,  4 dirs, 166.6 MB -> 65.9 MB zipped
#   requiring .NET        96 files,  4 dirs,  92.4 MB -> 31.7 MB zipped
#   requiring both        42 files,  0 dirs,  38.7 MB -> 10.8 MB zipped
#
# WHY NEITHER IS A SINGLE FILE. Tried 2026-09-04, and it does not work here.
# PublishSingleFile produces one 68 MB exe that dies on launch:
#
#   FileNotFoundException 0x8007007E at WinRT.ActivationFactory.Get
#   -> Microsoft.Windows.AppLifecycle.AppInstance.FindOrRegisterForKey
#
# WinRT activation resolves the Windows App SDK's native DLLs by filename beside
# the exe, and a bundle has no filenames. Leaving the natives loose with
# IncludeNativeLibrariesForSelfExtract=false is refused by the SDK's own
# Microsoft.WindowsAppSDK.SingleFile.targets, which requires
# IncludeAllContentForSelfExtract=true. Both directions are closed. The
# INSTALLER is one file; the installed application is not, and does not need to
# be.
# NOT $producedArtifacts. PowerShell variable names are case-INSENSITIVE, so that
# name is the -ReleaseArtifacts switch parameter, and assigning a list to it
# fails the whole script at binding with an error that names neither the
# variable nor the line:
#   Cannot convert "System.Object[]" to "SwitchParameter"
$producedArtifacts = @()

if ($Zip -or $Installer) {
    $version = Get-ProductVersion            # four parts, for the installer's AppVersion
    $displayVersion = Get-DisplayVersion     # three, for the filenames a release links to
    if (-not (Test-Path $artifactsDir)) { New-Item -ItemType Directory -Path $artifactsDir | Out-Null }
}

if ($Zip) {
    Write-Host "portable: publishing ($(if ($FrameworkDependent) { 'framework-dependent' } else { 'self-contained' }))" -ForegroundColor Cyan
    $published = Publish-App -Bundled (-not $FrameworkDependent)

    $archive = Join-Path $artifactsDir "PicoSwitch2-Companion-$displayVersion-$Platform-portable.zip"
    if (Test-Path $archive) { Remove-Item -LiteralPath $archive -Force }

    # Staged under a NAMED folder, and the folder is what gets zipped, so
    # extracting produces one directory rather than emptying hundreds of files
    # into whatever the user unzipped into. Windows' own Extract All adds no
    # containing folder, so the archive has to carry it.
    $root = Join-Path ([System.IO.Path]::GetTempPath()) "picoswitch-zip-$([guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Path $root | Out-Null
        Copy-PrunedPayload -From $published -Destination (Join-Path $root 'PicoSwitch2 Companion')
        Compress-Archive -Path (Join-Path $root 'PicoSwitch2 Companion') `
            -DestinationPath $archive -CompressionLevel Optimal
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }

    $producedArtifacts += $archive
}

if ($Installer) {
    $iscc = Find-InnoSetup
    if (-not $iscc) {
        throw @'
Inno Setup 6.3 or later is required to build the installer, and ISCC.exe was not found.

Install it with:

    winget install --id JRSoftware.InnoSetup --exact

or from https://jrsoftware.org/isdl.php, then run this again. build.ps1 finds it
through the uninstall registry key (either scope), the default per-user and
Program Files locations, and PATH.
'@
    }
    Write-Host "installer: using $iscc" -ForegroundColor DarkGray

    # Framework-dependent: the installer can resolve a missing runtime, so it
    # takes the smaller payload and does exactly that.
    Write-Host 'installer: publishing (framework-dependent)' -ForegroundColor Cyan
    $published = Publish-App -Bundled $false

    $root = Join-Path ([System.IO.Path]::GetTempPath()) "picoswitch-setup-$([guid]::NewGuid().ToString('N'))"
    $payload = Join-Path $root 'payload'
    try {
        New-Item -ItemType Directory -Path $root | Out-Null
        Copy-PrunedPayload -From $published -Destination $payload

        $baseName = "PicoSwitch2-Companion-Setup-$displayVersion-$Platform"
        & $iscc `
            "/DAppVersion=$version" `
            "/DPayloadDir=$payload" `
            "/DOutputDir=$artifactsDir" `
            "/DOutputBaseFilename=$baseName" `
            '/Qp' `
            (Join-Path $PSScriptRoot 'installer/PicoSwitch2Companion.iss')
        if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }

        $producedArtifacts += Join-Path $artifactsDir "$baseName.exe"
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($producedArtifacts.Count -gt 0) {
    Write-Host ''
    Write-Host 'artifacts' -ForegroundColor Green
    foreach ($artifact in $producedArtifacts) {
        $item = Get-Item -LiteralPath $artifact
        $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Host ("  {0,-52} {1,7:N1} MB" -f $item.Name, ($item.Length / 1MB))
        Write-Host ("  {0,-52} sha256 {1}" -f '', $hash) -ForegroundColor DarkGray
    }

    # SHA256SUMS.txt is written ONLY for a full release build.
    #
    # It describes a release, and a release is both files. Writing it from a
    # -Zip-only or -Installer-only run would silently replace a complete
    # manifest with a partial one that still looks authoritative -- the worst
    # possible failure for a file whose entire job is to be trusted.
    if ($Zip -and $Installer) {
        $lines = foreach ($artifact in $producedArtifacts) {
            $item = Get-Item -LiteralPath $artifact
            "$((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()) *$($item.Name)"
        }
        # The conventional `hash *name` shape, so `sha256sum -c` reads it. Names
        # only, no paths: it sits beside the artifacts it describes.
        $sums = Join-Path $artifactsDir 'SHA256SUMS.txt'
        Set-Content -LiteralPath $sums -Value $lines -Encoding ASCII
        Write-Host "  SHA256SUMS.txt" -ForegroundColor Green
    } else {
        $stale = Join-Path $artifactsDir 'SHA256SUMS.txt'
        if (Test-Path $stale) { Remove-Item -LiteralPath $stale -Force }
        Write-Host '  (no SHA256SUMS.txt: that is written by -ReleaseArtifacts, which builds both)' -ForegroundColor DarkGray
    }
    Write-Host "  in $artifactsDir" -ForegroundColor DarkGray
    Write-Host ''
    if ($Installer) {
        Write-Host 'installer requires: .NET 9 Desktop Runtime, detected and fetched from' -ForegroundColor DarkGray
        Write-Host '                    Microsoft if absent. Windows App SDK is bundled.' -ForegroundColor DarkGray
    }
    if ($Zip -and -not $FrameworkDependent) {
        Write-Host 'portable requires:  nothing -- both runtimes are bundled' -ForegroundColor DarkGray
    } elseif ($Zip) {
        Write-Host 'portable requires:  .NET 9 Desktop Runtime (x64) on the user machine' -ForegroundColor Yellow
    }
}

Write-Host 'OK' -ForegroundColor Green
