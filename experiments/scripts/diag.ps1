$o='D:\jsl-audio\diag.log'
bcdedit /enum '{current}' *> $o
pnputil /restart-device 'PCI\VEN_8086&DEV_4DC8&SUBSYS_72708086&REV_01\3&11583659&0&FB' *>> $o
Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 8 -ErrorAction SilentlyContinue | % { $_.TimeCreated.ToString() + ' ' + $_.Id + ' ' + $_.Message } *>> $o
Get-WinEvent -LogName System -MaxEvents 30 | ?{ $_.Message -match 'jslprobe|JSL' } | % { $_.TimeCreated.ToString() + ' ' + $_.Id + ' ' + $_.Message } *>> $o
