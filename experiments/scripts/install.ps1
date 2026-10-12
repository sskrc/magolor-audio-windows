# elevated: install package <name>, restart device, dump results
$n=$args[0]; $o="D:\jsl-audio\install-$n.log"; $id='PCI\VEN_8086&DEV_4DC8&SUBSYS_72708086&REV_01\3&11583659&0&FB'
pnputil /enum-drivers | Select-String -Context 0,6 "jsl$n.inf" | % { if($_.Line -match "(oem\d+\.inf)"){} }
$old = (pnputil /enum-drivers) -join "`n" | Select-String -AllMatches -Pattern "Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+jsl(?:boot\d*|probe)\.inf" | % { $_.Matches } | % { $_.Groups[1].Value }
foreach($x in $old){ pnputil /delete-driver $x /uninstall /force *>> $o }
pnputil /add-driver "D:\jsl-audio\pkg-$n\jsl$n.inf" /install *> $o
pnputil /restart-device $id *>> $o
Start-Sleep 3
$d=Get-PnpDevice -InstanceId $id; "Status=$($d.Status) Problem=$($d.Problem)" *>> $o
Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters" | fl | Out-String -Width 200 *>> $o
Get-WinEvent -LogName System -MaxEvents 6 | ?{ $_.Id -in 219,411 } | %{ "$($_.Id) $($_.Message)" } *>> $o



