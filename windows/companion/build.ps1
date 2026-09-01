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

      -Msix              the packaged flavour. Requires .NET Framework MSBuild
                         (Visual Studio / Build Tools): the packaging task loads
                         `System.Security.Permissions`, which is absent under the
                         .NET SDK's own MSBuild. Documented in docs/README.md §4.

.EXAMPLE
    ./build.ps1                     # build + test the core half
    ./build.ps1 -App                # additionally build the WinUI shell (x64)
    ./build.ps1 -App -Platform arm64
    ./build.ps1 -Msix               # produce an unsigned MSIX
    ./build.ps1 -Core -App -Configuration Release
#>
[CmdletBinding()]
param(
    [switch]$Core,
    [switch]$App,
    [switch]$Msix,
    [switch]$NoTest,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'arm64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

# No switch given means the default: the core half.
if (-not $Core -and -not $App -and -not $Msix) { $Core = $true }

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

$msbuild = $null
if ($App -or $Msix) {
    $msbuild = Find-FrameworkMsbuild
    if (-not $msbuild) {
        throw 'The Controller Link host needs Visual C++ Build Tools; .NET Framework MSBuild was not found.'
    }

    $hostProject = 'src/PicoSwitch.ControllerLink.Host/PicoSwitch.ControllerLink.Host.vcxproj'
    Write-Host "$msbuild $hostProject" -ForegroundColor DarkGray
    & $msbuild $hostProject -restore -t:Build `
        "-p:Configuration=$Configuration" "-p:Platform=$Platform" -v:m -nologo
    if ($LASTEXITCODE -ne 0) { throw "Controller Link host build failed with exit code $LASTEXITCODE" }
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
        '-p:SkipControllerLinkHostBuild=true'
        '--nologo'
    )
}

if ($Msix) {
    # Deliberately NOT `dotnet build`. The packaging task
    # (WinAppSdkValidateAppxManifestItems) loads System.Security.Permissions,
    # which the .NET SDK's MSBuild cannot resolve; .NET Framework MSBuild can.
    # See docs/README.md §4 -- this is a Windows App SDK packaging gap, not a
    # project defect, and hiding it behind a retry would only make it puzzling.
    Write-Host "$msbuild $appProject (MSIX)" -ForegroundColor DarkGray
    & $msbuild $appProject -restore -t:Build `
        "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
        -p:SkipControllerLinkHostBuild=true `
        -p:WindowsPackageType=MSIX -p:GenerateAppxPackageOnBuild=true `
        -p:AppxPackageSigningEnabled=false -v:m -nologo
    if ($LASTEXITCODE -ne 0) { throw "MSIX packaging failed with exit code $LASTEXITCODE" }
}

Write-Host 'OK' -ForegroundColor Green
