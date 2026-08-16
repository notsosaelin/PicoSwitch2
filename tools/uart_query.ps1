# Send one or more UART diagnostic commands to the adapter and print the replies.
#
# The adapter's diagnostic surface is a line protocol on the CP210x bridge. This
# exists so an investigation can query it directly instead of asking the
# maintainer to open a terminal and transcribe lines.
#
#   pwsh -File tools/uart_query.ps1 -Command 'rumble'
#   pwsh -File tools/uart_query.ps1 -Command 'rumble clear','rumble','imu'
#
# -Port defaults to the first CP210x bridge found.
param(
    [string[]]$Command = @('ping'),
    [string]$Port,
    [int]$Baud = 115200,
    [int]$TimeoutMs = 1500
)

$ErrorActionPreference = 'Stop'

if (-not $Port) {
    $cp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'CP210x.*\((COM\d+)\)' } |
        Select-Object -First 1
    if ($cp -and $cp.Name -match '\((COM\d+)\)') { $Port = $Matches[1] }
}
if (-not $Port) { throw 'No CP210x UART bridge found; pass -Port COMn' }

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = $TimeoutMs
$sp.NewLine = "`r`n"
try {
    $sp.Open()
    Start-Sleep -Milliseconds 150
    $sp.DiscardInBuffer()
    foreach ($c in $Command) {
        $sp.WriteLine($c)
        # Replies are one line, but the adapter also emits unsolicited printf
        # traffic on this port, so read until a line looks like a JSON reply or
        # the read times out.
        $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
        $answered = $false
        while ([DateTime]::UtcNow -lt $deadline) {
            try {
                $line = $sp.ReadLine()
            } catch [TimeoutException] {
                break
            }
            if ($line -match '^\s*\{') {
                "[$c] $line"
                $answered = $true
                break
            }
        }
        if (-not $answered) { "[$c] <no JSON reply within ${TimeoutMs}ms>" }
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
