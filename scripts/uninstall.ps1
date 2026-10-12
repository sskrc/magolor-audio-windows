# SPDX-License-Identifier: MIT
# (Administrator) Remove every jslaudio driver package and restart the audio DSP device.
# Windows then falls back to whatever other driver matches (e.g. Intel's, if present in the driver store).
$old = (pnputil /enum-drivers) -join "`n" |
    Select-String -AllMatches -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+jslaudio\.inf' |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value }
foreach ($x in $old) { pnputil /delete-driver $x /uninstall /force }
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_4DC8*' } | Select-Object -First 1
if ($dev) { pnputil /scan-devices; Get-PnpDevice -InstanceId $dev.InstanceId | Format-List FriendlyName, Status, Problem }
