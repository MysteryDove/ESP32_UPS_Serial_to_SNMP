param(
    [Parameter(Mandatory = $true)]
    [string]$Target,

    [string]$Community = "public",

    [ValidateSet("1", "2c")]
    [string]$Version = "2c",

    [int]$TimeoutSec = 2,

    [int]$Retries = 1,

    [switch]$WalkUpsMib,

    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

function Write-Info {
    param([string]$Text)
    if (-not $Quiet) {
        Write-Host $Text
    }
}

function Get-CommandPath {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        return $null
    }
    return $cmd.Source
}

$snmpGetPath = Get-CommandPath -Name "snmpget"
if ([string]::IsNullOrWhiteSpace($snmpGetPath)) {
    Write-Error "snmpget not found in PATH. Install Net-SNMP first."
    exit 2
}

$snmpWalkPath = Get-CommandPath -Name "snmpwalk"

$oids = @(
    [PSCustomObject]@{ Name = "sysDescr"; Oid = "1.3.6.1.2.1.1.1.0" },
    [PSCustomObject]@{ Name = "sysName"; Oid = "1.3.6.1.2.1.1.5.0" },

    [PSCustomObject]@{ Name = "upsIdentManufacturer"; Oid = "1.3.6.1.2.1.33.1.1.1.0" },
    [PSCustomObject]@{ Name = "upsIdentModel"; Oid = "1.3.6.1.2.1.33.1.1.2.0" },
    [PSCustomObject]@{ Name = "upsIdentUPSSoftwareVersion"; Oid = "1.3.6.1.2.1.33.1.1.3.0" },
    [PSCustomObject]@{ Name = "upsIdentAgentSoftwareVersion"; Oid = "1.3.6.1.2.1.33.1.1.4.0" },
    [PSCustomObject]@{ Name = "upsIdentName"; Oid = "1.3.6.1.2.1.33.1.1.5.0" },
    [PSCustomObject]@{ Name = "upsIdentAttachedDevices"; Oid = "1.3.6.1.2.1.33.1.1.6.0" },

    [PSCustomObject]@{ Name = "upsBatteryStatus"; Oid = "1.3.6.1.2.1.33.1.2.1.0" },
    [PSCustomObject]@{ Name = "upsSecondsOnBattery"; Oid = "1.3.6.1.2.1.33.1.2.2.0" },
    [PSCustomObject]@{ Name = "upsEstimatedMinutesRemaining"; Oid = "1.3.6.1.2.1.33.1.2.3.0" },
    [PSCustomObject]@{ Name = "upsEstimatedChargeRemaining"; Oid = "1.3.6.1.2.1.33.1.2.4.0" },
    [PSCustomObject]@{ Name = "upsBatteryVoltage"; Oid = "1.3.6.1.2.1.33.1.2.5.0" },
    [PSCustomObject]@{ Name = "upsBatteryCurrent"; Oid = "1.3.6.1.2.1.33.1.2.6.0" },
    [PSCustomObject]@{ Name = "upsBatteryTemperature"; Oid = "1.3.6.1.2.1.33.1.2.7.0" },

    [PSCustomObject]@{ Name = "upsInputLineBads"; Oid = "1.3.6.1.2.1.33.1.3.1.0" },
    [PSCustomObject]@{ Name = "upsInputNumLines"; Oid = "1.3.6.1.2.1.33.1.3.2.0" },
    [PSCustomObject]@{ Name = "upsInputFrequency.1"; Oid = "1.3.6.1.2.1.33.1.3.3.1.2.1" },
    [PSCustomObject]@{ Name = "upsInputVoltage.1"; Oid = "1.3.6.1.2.1.33.1.3.3.1.3.1" },

    [PSCustomObject]@{ Name = "upsOutputSource"; Oid = "1.3.6.1.2.1.33.1.4.1.0" },
    [PSCustomObject]@{ Name = "upsOutputFrequency"; Oid = "1.3.6.1.2.1.33.1.4.2.0" },
    [PSCustomObject]@{ Name = "upsOutputNumLines"; Oid = "1.3.6.1.2.1.33.1.4.3.0" },
    [PSCustomObject]@{ Name = "upsOutputVoltage.1"; Oid = "1.3.6.1.2.1.33.1.4.4.1.2.1" },
    [PSCustomObject]@{ Name = "upsOutputCurrent.1"; Oid = "1.3.6.1.2.1.33.1.4.4.1.3.1" },
    [PSCustomObject]@{ Name = "upsOutputPower.1"; Oid = "1.3.6.1.2.1.33.1.4.4.1.4.1" },
    [PSCustomObject]@{ Name = "upsOutputPercentLoad.1"; Oid = "1.3.6.1.2.1.33.1.4.4.1.5.1" },

    [PSCustomObject]@{ Name = "upsConfigInputVoltage"; Oid = "1.3.6.1.2.1.33.1.9.1.0" },
    [PSCustomObject]@{ Name = "upsConfigOutputVoltage"; Oid = "1.3.6.1.2.1.33.1.9.3.0" },
    [PSCustomObject]@{ Name = "upsConfigOutputPower"; Oid = "1.3.6.1.2.1.33.1.9.6.0" },
    [PSCustomObject]@{ Name = "upsConfigLowBattTime"; Oid = "1.3.6.1.2.1.33.1.9.7.0" },
    [PSCustomObject]@{ Name = "upsConfigLowVoltageTransferPoint"; Oid = "1.3.6.1.2.1.33.1.9.9.0" },
    [PSCustomObject]@{ Name = "upsConfigHighVoltageTransferPoint"; Oid = "1.3.6.1.2.1.33.1.9.10.0" }
)

Write-Info "SNMP check target=$Target version=$Version community=$Community"
Write-Info "Checking $($oids.Count) OIDs..."

$results = @()

foreach ($item in $oids) {
    $args = @(
        "-v", $Version,
        "-c", $Community,
        "-t", "$TimeoutSec",
        "-r", "$Retries",
        $Target,
        $item.Oid
    )

    $output = & $snmpGetPath @args 2>&1
    $code = $LASTEXITCODE

    $ok = ($code -eq 0)
    $value = ""
    if ($ok) {
        $value = ($output | Out-String).Trim()
    } else {
        $value = ($output | Out-String).Trim()
    }

    $results += [PSCustomObject]@{
        Name = $item.Name
        Oid = $item.Oid
        Status = if ($ok) { "PASS" } else { "FAIL" }
        Output = $value
    }
}

$failed = ($results | Where-Object { $_.Status -eq "FAIL" }).Count
$passed = $results.Count - $failed

$results | Format-Table -AutoSize

Write-Host ""
Write-Host "Summary: PASS=$passed FAIL=$failed TOTAL=$($results.Count)"

if ($WalkUpsMib) {
    if ([string]::IsNullOrWhiteSpace($snmpWalkPath)) {
        Write-Warning "snmpwalk not found in PATH; skipping walk"
    } else {
        Write-Host ""
        Write-Host "SNMP walk: 1.3.6.1.2.1.33.1"
        & $snmpWalkPath -v $Version -c $Community -t "$TimeoutSec" -r "$Retries" $Target 1.3.6.1.2.1.33.1
    }
}

if ($failed -gt 0) {
    exit 1
}

exit 0
