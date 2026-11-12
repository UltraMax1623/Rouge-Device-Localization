function Get-ConnectedWifiSsid([string]$InterfaceAlias) {
    $lines = & "$env:SystemRoot\System32\netsh.exe" wlan show interfaces 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Cannot read the connected Wi-Fi SSID: $lines" }

    $name = $null
    $ssid = $null
    foreach ($line in $lines) {
        if ($line -match '^\s*Name\s*:\s*(.*?)\s*$') {
            if ($name -eq $InterfaceAlias -and $ssid) { return $ssid }
            $name = $Matches[1]
            $ssid = $null
        } elseif ($line -match '^\s*SSID\s*:\s*(.*?)\s*$') {
            $ssid = $Matches[1]
        }
    }
    if ($name -eq $InterfaceAlias) { return $ssid }
    return $null
}
