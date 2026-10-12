$o='D:\jsl-audio\wdm.log'
sc.exe create jslwdm type= kernel binPath= D:\jsl-audio\out\jslwdm.sys *> $o
sc.exe start jslwdm *>> $o
sc.exe stop jslwdm *>> $o
sc.exe delete jslwdm *>> $o
