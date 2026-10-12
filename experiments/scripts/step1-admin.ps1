# Step 1 (admin): trust test cert + enable test signing. Reboot afterwards.
Import-Certificate -FilePath D:\jsl-audio\jsltest.cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath D:\jsl-audio\jsltest.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
bcdedit /set testsigning on
"done: reboot, then run step2" | Out-File D:\jsl-audio\step1.log
