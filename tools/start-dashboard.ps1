param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path -Parent $PSScriptRoot
$uiPath = Join-Path $projectPath 'UI'
$port = 3000
if ($env:PORT) {
    $port = 0
    if (-not [int]::TryParse($env:PORT, [ref]$port) -or $port -lt 1 -or $port -gt 65535) {
        throw 'PORT must be a number from 1 to 65535.'
    }
}
$dashboardUrl = "http://127.0.0.1:$port/"
function Find-Browser {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\BraveSoftware\Brave-Browser\Application\brave.exe",
        "$env:LOCALAPPDATA\BraveSoftware\Brave-Browser\Application\brave.exe",
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'A supported browser was not found. Install Edge, Brave, or Chrome.'
}
function Get-DashboardState {
    try {
        $health = Invoke-WebRequest -Uri ($dashboardUrl + 'api/health') -UseBasicParsing -TimeoutSec 2
        if ($health.Content.Trim() -ne 'Backend is running.') { return 'other' }
        $page = Invoke-WebRequest -Uri $dashboardUrl -UseBasicParsing -TimeoutSec 2
        if ($page.Content -notmatch '<title>Device Localization Console</title>') { return 'other' }
        return 'ready'
    } catch {
        if ($_.Exception.Response) { return 'other' }
        return 'offline'
    }
}
$launcherMutex = $null
$ownsMutex = $false
try {
    $node = (Get-Command node.exe -ErrorAction Stop).Source
    $browser = Find-Browser
    foreach ($required in @('server.js', 'node_modules\ws\package.json', 'node_modules\cytoscape\package.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $uiPath $required))) {
            throw 'Dashboard dependencies are missing. Run npm install in the UI folder first.'
        }
    }
    if ($CheckOnly) {
        Write-Output "Project: $projectPath"
        Write-Output "Node: $node"
        Write-Output "Browser: $browser"
        Write-Output "URL: $dashboardUrl"
        exit 0
    }
    $launcherMutex = New-Object System.Threading.Mutex($false, 'Local\RougeDeviceLocalizationDashboardLauncher')
    $ownsMutex = $launcherMutex.WaitOne(0)
    if (-not $ownsMutex) { exit 0 }
    $status = Get-DashboardState
    if ($status -eq 'other') { throw "Port $port is being used by another application. Close it or set a different PORT." }
    if ($status -ne 'ready') {
        $logPath = Join-Path $projectPath '.build\launcher'
        New-Item -ItemType Directory -Force -Path $logPath | Out-Null
        Start-Process -FilePath $node -ArgumentList 'server.js' -WorkingDirectory $uiPath -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $logPath 'server.log') `
            -RedirectStandardError (Join-Path $logPath 'server-error.log') | Out-Null
        $deadline = [DateTime]::UtcNow.AddSeconds(25)
        do {
            Start-Sleep -Milliseconds 350
            $status = Get-DashboardState
        } while ($status -eq 'offline' -and [DateTime]::UtcNow -lt $deadline)
        if ($status -ne 'ready') {
            throw "The server could not start on port $port. Check .build\launcher\server-error.log in the project folder."
        }
    }
    # The visible browser window is explicitly requested; the server stays hidden.
    Start-Process -FilePath $browser -ArgumentList @('--new-window', $dashboardUrl) | Out-Null
} catch {
    if ($CheckOnly) { Write-Error $_; exit 1 }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Device Localization Console', 'OK', 'Error') | Out-Null
    exit 1
} finally {
    if ($ownsMutex -and $launcherMutex) { $launcherMutex.ReleaseMutex() }
    if ($launcherMutex) { $launcherMutex.Dispose() }
}
