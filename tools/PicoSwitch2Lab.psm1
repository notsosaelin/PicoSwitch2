Set-StrictMode -Version Latest

function Get-Ps2LabPython {
    foreach ($candidate in @('python', 'py', 'python3')) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    return $null
}

function Set-Ps2LabText {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [AllowEmptyString()]
        [string[]]$Lines
    )
    $parent = [System.IO.Path]::GetDirectoryName(
        [System.IO.Path]::GetFullPath($Path))
    if ($parent -and -not [System.IO.Directory]::Exists($parent)) {
        [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [System.IO.File]::WriteAllLines(
        $Path, $Lines, [System.Text.UTF8Encoding]::new($false))
}

function Set-Ps2LabJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value,
        [int]$Depth = 12
    )
    Set-Ps2LabText -Path $Path -Lines @(
        $Value | ConvertTo-Json -Depth $Depth)
}

function Get-Ps2LabDigest {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path
    return [pscustomobject][ordered]@{
        path = $item.Name
        bytes = [uint64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Get-Ps2LabGit {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $git = [ordered]@{}
    try {
        $git['branch'] = (git -C $RepoRoot rev-parse --abbrev-ref HEAD).Trim()
        $git['commit'] = (git -C $RepoRoot rev-parse HEAD).Trim()
        $status = @(git -C $RepoRoot status --porcelain)
        $git['dirty'] = $status.Count -gt 0
        $git['dirty_count'] = $status.Count
        $git['dirty_files'] = @($status | Select-Object -First 40)
    } catch {
        $git['error'] = $_.Exception.Message
    }
    return $git
}

function New-Ps2LabDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$Scenario,
        [datetime]$Timestamp = (Get-Date)
    )
    $slug = ($Scenario.Trim() -replace '[^A-Za-z0-9\-_.]', '-')
    if (-not $slug) { throw 'Scenario must contain at least one filename-safe character.' }
    $root = if ([System.IO.Path]::IsPathRooted($OutputRoot)) {
        [System.IO.Path]::GetFullPath($OutputRoot)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $OutputRoot))
    }
    $directory = Join-Path $root (
        '{0}-{1}' -f $Timestamp.ToString('yyyyMMdd-HHmmss'), $slug)
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
    return $directory
}

function Get-Ps2LabPort {
    param(
        [Parameter(Mandatory = $true)][string]$Reader,
        [string]$Port
    )
    if ($Port) {
        return [pscustomobject]@{ port = $Port; name = 'explicit override' }
    }
    $reported = & $Reader -ReportPort | ConvertFrom-Json
    if (-not $reported.port) { throw 'UART helper did not report a usable port.' }
    return $reported
}

function Invoke-Ps2LabDiag {
    param(
        [Parameter(Mandatory = $true)][string]$Reader,
        [Parameter(Mandatory = $true)][string]$Port,
        [Parameter(Mandatory = $true)][string]$Command,
        [string]$OutputPath,
        [ValidateRange(100, 850)][int]$CaptureMs = 600,
        [switch]$AllowFailure
    )
    $arguments = @{
        Port = $Port
        Command = $Command
        CaptureMs = $CaptureMs
    }
    if ($OutputPath) { $arguments['OutputPath'] = $OutputPath }
    try {
        return @(& $Reader @arguments 2>&1 |
                 Where-Object { $_ -is [string] })
    } catch {
        if (-not $AllowFailure) { throw }
        return @(
            [pscustomobject][ordered]@{
                error = $_.Exception.Message
                command = $Command
            } | ConvertTo-Json -Compress)
    }
}

function Get-Ps2LabJsonReply {
    param(
        [Parameter(Mandatory = $true)][string]$Reader,
        [Parameter(Mandatory = $true)][string]$Port,
        [Parameter(Mandatory = $true)][string]$Command,
        [switch]$AllowFailure
    )
    $lines = @(Invoke-Ps2LabDiag -Reader $Reader -Port $Port `
        -Command $Command -AllowFailure:$AllowFailure)
    if ($lines.Count -ne 1) {
        throw "$Command returned $($lines.Count) lines; expected one JSON object."
    }
    return $lines[0] | ConvertFrom-Json
}

function New-Ps2LabManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string]$Scenario,
        [Parameter(Mandatory = $true)][string]$Action,
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [string]$Kind = 'observation',
        [string]$Hypothesis = '',
        [string]$Variable = '',
        [string]$Expectation = '',
        [string]$Port = '',
        [string]$Baseline = '',
        $Conditions = $null,
        $Diagnostics = $null,
        [string]$Verdict = '',
        [string[]]$ArtifactPaths = @()
    )
    $artifacts = @(
        foreach ($path in $ArtifactPaths) {
            $digest = Get-Ps2LabDigest -Path $path
            if ($digest) { $digest }
        }
    )
    return [pscustomobject][ordered]@{
        schema = 'picoswitch2-lab/v1'
        tool = $Tool
        scenario = $Scenario
        timestamp = (Get-Date).ToString('o')
        kind = $Kind
        hypothesis = $Hypothesis
        variable = $Variable
        expectation = $Expectation
        action = $Action
        port = $Port
        baseline = $Baseline
        conditions = $Conditions
        git = Get-Ps2LabGit -RepoRoot $RepoRoot
        diagnostics = $Diagnostics
        verdict = $Verdict
        artifacts = $artifacts
    }
}

Export-ModuleMember -Function @(
    'Get-Ps2LabPython',
    'Set-Ps2LabText',
    'Set-Ps2LabJson',
    'Get-Ps2LabDigest',
    'Get-Ps2LabGit',
    'New-Ps2LabDirectory',
    'Get-Ps2LabPort',
    'Invoke-Ps2LabDiag',
    'Get-Ps2LabJsonReply',
    'New-Ps2LabManifest'
)
