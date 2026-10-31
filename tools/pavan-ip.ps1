[CmdletBinding()]
param(
    [string]$InterfaceAlias = 'Wi-Fi',
    [string]$ProfileName = 'Pavan',
    [string]$Address = '10.131.226.100',
    [switch]$Preview,
    [switch]$RestoreDhcp
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'wifi-ssid.ps1')
$stateDir = Join-Path (Split-Path $PSScriptRoot -Parent) '.build\pavan-network'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
$statePath = Join-Path $stateDir 'state.json'
$logPath = Join-Path $stateDir 'activity.log'
function Write-Activity([string]$Message) {
    $line = '{0} {1}' -f (Get-Date -Format s), $Message
    Write-Output $line
    # Bound log growth.
    if ((Test-Path $logPath) -and (Get-Item $logPath).Length -gt 262144) {
        Get-Content $logPath -Tail 300 | Set-Content (Join-Path $stateDir 'log.tmp')
        Move-Item -LiteralPath (Join-Path $stateDir 'log.tmp') -Destination $logPath -Force
    }
    Add-Content -LiteralPath $logPath -Value $line
}
function Test-SameSubnet([string]$First, [string]$Second, [int]$Prefix) {
    $a = ([ipaddress]$First).GetAddressBytes()
    $b = ([ipaddress]$Second).GetAddressBytes()
    if ($a.Length -ne 4 -or $b.Length -ne 4 -or $Prefix -lt 1 -or $Prefix -gt 30) { return $false }
    for ($i = 0; $i -lt 4; $i++) {
        $bits = [Math]::Min(8, [Math]::Max(0, $Prefix - 8 * $i))
        $mask = 256 - [Math]::Pow(2, 8 - $bits)
        if (($a[$i] -band [int]$mask) -ne ($b[$i] -band [int]$mask)) { return $false }
    }
    return $true
}
function Invoke-Netsh([string[]]$Arguments) {
    $result = & "$env:SystemRoot\System32\netsh.exe" @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "netsh failed: $result" }
}
function Restore-ManagedDhcp($State) {
    if (-not $State -or -not $State.Managed) { return }
    if ($State.InterfaceAlias -ne $InterfaceAlias) { throw 'Saved adapter does not match; refusing to change it.' }
    if ($Preview) { Write-Activity "PREVIEW: would restore DHCP/DNS on $InterfaceAlias"; return }
    $netInterface = Get-NetIPInterface -InterfaceAlias $InterfaceAlias -AddressFamily IPv4
    if ($netInterface.Dhcp -ne 'Enabled') {
        Invoke-Netsh -Arguments @('interface','ipv4','set','address',"name=$InterfaceAlias",'source=dhcp')
    }
    $currentDns = @((Get-DnsClientServerAddress -InterfaceAlias $InterfaceAlias -AddressFamily IPv4).ServerAddresses)
    if ($currentDns.Count -gt 0 -and ($currentDns -join ',') -eq (@($State.Dns) -join ',')) {
        Invoke-Netsh -Arguments @('interface','ipv4','set','dnsservers',"name=$InterfaceAlias",'source=dhcp')
    }
    $State.Managed = $false
    $State | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath
    Write-Activity "Restored automatic IPv4 and DNS on $InterfaceAlias"
}
try {
    $state = if (Test-Path $statePath) { Get-Content -Raw $statePath | ConvertFrom-Json } else { $null }
    if ($RestoreDhcp) {
        if ($state -and $state.Managed) { Restore-ManagedDhcp $state }
        else { Write-Activity 'No Pavan static address managed by this script; no change' }
        exit 0
    }
    $adapter = Get-NetAdapter -Name $InterfaceAlias -ErrorAction Stop
    $ssid = Get-ConnectedWifiSsid $InterfaceAlias
    if ($ssid -cne $ProfileName) {
        Restore-ManagedDhcp $state
        if ($Preview) { Write-Activity "PREVIEW: SSID '$ssid' is not '$ProfileName'; no static IP applied" }
        exit 0
    }
    $ips = @(Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike '169.254.*' })
    if ($state -and $state.Managed -and $state.Address -eq $Address -and
        ($ips | Where-Object { $_.IPAddress -eq $Address -and $_.AddressState -eq 'Preferred' })) {
        Write-Activity "$Address already active on Pavan; no change"
        exit 0
    }
    # Acquire hotspot subnet/gateway from DHCP before attempting a static IP.
    # Never carry a previous hotspot's static settings onto a different subnet.
    if ($state -and $state.Managed) {
        Restore-ManagedDhcp $state
        Write-Activity 'DHCP restored; will check the newly acquired hotspot settings on the next trigger'
        exit 0
    }
    $ip = $ips | Where-Object { $_.PrefixOrigin -eq 'Dhcp' } | Select-Object -First 1
    if (-not $ip) { Write-Activity 'Waiting for a DHCP lease on Pavan; no address changed'; exit 0 }
    $gateway = Get-NetRoute -InterfaceIndex $adapter.ifIndex -DestinationPrefix '0.0.0.0/0' |
        Sort-Object RouteMetric | Select-Object -First 1 -ExpandProperty NextHop
    if (-not $gateway -or -not (Test-SameSubnet $ip.IPAddress $Address $ip.PrefixLength) -or
        -not (Test-SameSubnet $gateway $Address $ip.PrefixLength) -or $gateway -eq $Address) {
        Write-Activity "REFUSED: Pavan DHCP=$($ip.IPAddress)/$($ip.PrefixLength), gateway=$gateway; target $Address does not fit. DHCP retained."
        exit 0
    }
    $dns = @((Get-DnsClientServerAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4).ServerAddresses)
    if ($dns.Count -eq 0) { throw 'No DHCP DNS addresses available; no settings changed.' }
    if ($Preview) {
        Write-Activity "PREVIEW: would use $Address/$($ip.PrefixLength), gateway=$gateway, DNS=$($dns -join ',') on Pavan"
        exit 0
    }
    if ($ip.IPAddress -ne $Address -and
        (Test-Connection -ComputerName $Address -Count 1 -Quiet -ErrorAction SilentlyContinue)) {
        throw "Target $Address answers ping and may already be in use; DHCP retained."
    }
    if ((Get-ConnectedWifiSsid $InterfaceAlias) -cne $ProfileName) {
        throw 'Wi-Fi SSID changed during validation; DHCP retained.'
    }
    # Save ownership before mutation so a failed/interrupted application is recoverable.
    $state = [pscustomobject]@{ Managed=$true; InterfaceAlias=$InterfaceAlias; Address=$Address;
        PreviousAddress=$ip.IPAddress; PrefixLength=$ip.PrefixLength; Gateway=$gateway;
        Dns=$dns; Profile=$ssid; SavedAt=(Get-Date -Format o) }
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath
    try {
        $mask = (0..3 | ForEach-Object {
            $bits = [Math]::Min(8, [Math]::Max(0, $ip.PrefixLength - 8 * $_))
            [int](256 - [Math]::Pow(2, 8 - $bits))
        }) -join '.'
        Invoke-Netsh -Arguments @('interface','ipv4','set','address',"name=$InterfaceAlias",'source=static',
            "address=$Address","mask=$mask","gateway=$gateway",'store=persistent')
        Set-DnsClientServerAddress -InterfaceIndex $adapter.ifIndex -ServerAddresses $dns
        $preferred = $false
        for ($attempt = 0; $attempt -lt 10; $attempt++) {
            Start-Sleep -Milliseconds 500
            $actual = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $Address -ErrorAction SilentlyContinue
            if ($actual.AddressState -eq 'Duplicate') { throw 'Windows detected a duplicate IP address.' }
            if ($actual.AddressState -eq 'Preferred') { $preferred = $true; break }
        }
        if (-not $preferred) { throw 'Static address did not become usable within five seconds.' }
        if ((Get-ConnectedWifiSsid $InterfaceAlias) -cne $ProfileName) {
            throw 'Wi-Fi SSID changed while setting the address.'
        }
        Write-Activity "Applied $Address/$($ip.PrefixLength) on Pavan; gateway=$gateway"
    } catch {
        Restore-ManagedDhcp $state
        throw
    }
} catch {
    Write-Activity "ERROR: $($_.Exception.Message)"
    exit 1
}
