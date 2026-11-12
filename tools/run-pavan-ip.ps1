[CmdletBinding()]
param([switch]$RestoreDhcp)
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'pavan-ip.ps1'
. (Join-Path $PSScriptRoot 'wifi-ssid.ps1')
$log = Join-Path (Split-Path $PSScriptRoot -Parent) '.build\pavan-network\activity.log'
$shell = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"

try {
    if (-not $RestoreDhcp) {
        $ssid = Get-ConnectedWifiSsid 'Wi-Fi'
        if ($ssid -cne 'Pavan') {
            throw "Connect the PC to Wi-Fi 'Pavan' first. Current SSID: '$ssid'."
        }
    }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        if ($RestoreDhcp) { & $script -RestoreDhcp } else { & $script }
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    } else {
        $arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}"{1}' -f `
            $script, $(if ($RestoreDhcp) { ' -RestoreDhcp' } else { '' })
        $process = Start-Process -FilePath $shell -ArgumentList $arguments `
            -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) {
            if (Test-Path $log) { Get-Content -LiteralPath $log -Tail 3 }
            throw "IP command failed (exit $($process.ExitCode))."
        }
        if (Test-Path $log) { Get-Content -LiteralPath $log -Tail 1 }
    }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    exit 1
}
