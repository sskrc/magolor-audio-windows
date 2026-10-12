$o='D:\jsl-audio\cmp.log'
$hd='PCI\VEN_8086&DEV_4DC8&SUBSYS_72708086&REV_01\3&11583659&0&FB'
pnputil /disable-device 'ACPI\RTL1015\0' *> $o
Start-Sleep 2
pnputil /restart-device $hd *>> $o
'--- amp disabled scan done' | Out-File $o -Append
Copy-Item D:\jsl-audio\boot.log D:\jsl-audio\boot.off.log -Force
pnputil /enable-device 'ACPI\RTL1015\0' *>> $o
Start-Sleep 2
pnputil /restart-device $hd *>> $o
Copy-Item D:\jsl-audio\boot.log D:\jsl-audio\boot.on.log -Force
