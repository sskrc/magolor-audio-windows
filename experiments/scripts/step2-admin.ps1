# Step 2 (admin, after reboot): install probe driver
pnputil /add-driver D:\jsl-audio\pkg-probe\jslprobe.inf /install | Out-File D:\jsl-audio\step2.log
