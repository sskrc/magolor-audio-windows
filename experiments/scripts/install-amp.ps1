$o='D:\jsl-audio\install-amp.log'
pnputil /add-driver D:\jsl-audio\pkg-amp\jslamp.inf /install *> $o
pnputil /restart-device 'ACPI\RTL1015\0' *>> $o
Start-Sleep 2
pnputil /restart-device 'PCI\VEN_8086&DEV_4DC8&SUBSYS_72708086&REV_01\3&11583659&0&FB' *>> $o
