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
    # Resolve the user's HTTP association, including Brave's versioned ProgID.
    $association = Get-ItemProperty -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\http\UserChoice' -ErrorAction SilentlyContinue
    if ($association.ProgId) {
        $commandKey = Get-Item -LiteralPath "Registry::HKEY_CLASSES_ROOT\$($association.ProgId)\shell\open\command" -ErrorAction SilentlyContinue
        if ($commandKey) {
            $command = [Environment]::ExpandEnvironmentVariables([string]$commandKey.GetValue(''))
            $executable = $null
            if ($command -match '^\s*"([^"]+\.exe)"') { $executable = $Matches[1] }
            elseif ($command -match '^\s*(.+?\.exe)(?:\s|$)') { $executable = $Matches[1] }
            if ($executable -and (Test-Path -LiteralPath $executable)) { return $executable }
        }
    }
    # Brave is the requested fallback when Windows association details are unavailable.
    $candidates = @(
        "$env:ProgramFiles\BraveSoftware\Brave-Browser\Application\brave.exe",
        "${env:ProgramFiles(x86)}\BraveSoftware\Brave-Browser\Application\brave.exe",
        "$env:LOCALAPPDATA\BraveSoftware\Brave-Browser\Application\brave.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'Could not find your default browser or Brave. Check the default HTTP browser in Windows settings.'
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
    $windowArgument = '--new-window'
    if ([IO.Path]::GetFileName($browser) -ieq 'firefox.exe') { $windowArgument = '-new-window' }
    Start-Process -FilePath $browser -ArgumentList @($windowArgument, $dashboardUrl) | Out-Null
} catch {
    if ($CheckOnly) { Write-Error $_; exit 1 }
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Device Localization Console', 'OK', 'Error') | Out-Null
    exit 1
} finally {
    if ($ownsMutex -and $launcherMutex) { $launcherMutex.ReleaseMutex() }
    if ($launcherMutex) { $launcherMutex.Dispose() }
}
