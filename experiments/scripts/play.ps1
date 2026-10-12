$o='D:\jsl-audio\play.log'
pnputil /add-driver D:\jsl-audio\pkg-boot2\jslboot2.inf /install *> $o
pnputil /restart-device 'PCI\VEN_8086&DEV_4DC8&SUBSYS_72708086&REV_01\3&11583659&0&FB' *>> $o
