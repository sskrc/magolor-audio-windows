# SPDX-License-Identifier: MIT
# (Administrator) Install pkg\jslaudio.inf on the Jasper Lake audio DSP (PCI\VEN_8086&DEV_4DC8),
# replacing any previous jslaudio package, then restart the device and print its state.
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$inf  = Join-Path $root 'pkg\jslaudio.inf'
if (-not (Test-Path $inf)) { throw "build first: $inf not found" }

$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_4DC8*' } | Select-Object -First 1
if (-not $dev) { throw 'Jasper Lake audio DSP (PCI\VEN_8086&DEV_4DC8) not found' }

$old = (pnputil /enum-drivers) -join "`n" |
    Select-String -AllMatches -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+jslaudio\.inf' |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value }
foreach ($x in $old) { pnputil /delete-driver $x /uninstall /force }

pnputil /add-driver $inf /install
pnputil /restart-device $dev.InstanceId
Start-Sleep 5
$d = Get-PnpDevice -InstanceId $dev.InstanceId
"Device: $($d.FriendlyName)  Status=$($d.Status)  Problem=$($d.Problem)"
Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction SilentlyContinue | ForEach-Object { "Endpoint: $($_.FriendlyName)  Status=$($_.Status)" }
