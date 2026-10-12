# usage: deploy.ps1 <name>   (builds, stamps, catalogs, signs D:\jsl-audio\pkg-<name>)
$n=$args[0]; $p="D:\jsl-audio\pkg-$n"
powershell -NoProfile -File D:\jsl-audio\build.ps1 "D:\jsl-audio\$n\$n.c" "jsl$n"; if($LASTEXITCODE){exit 1}
New-Item -ItemType Directory -Force $p | Out-Null
Copy-Item "D:\jsl-audio\out\jsl$n.sys" $p -Force; Copy-Item "D:\jsl-audio\$n\jsl$n.inf" $p -Force
$w='D:\work\nuget\Microsoft.Windows.WDK.x64\c\bin\10.0.26100.0'
& "$w\x64\stampinf.exe" -f "$p\jsl$n.inf" -a amd64 -k 1.15 -v '*' | Out-Null
& "$w\x86\Inf2Cat.exe" /driver:$p /os:10_x64 | Select -Last 1
$cert=Get-ChildItem Cert:\CurrentUser\My | ?{ $_.Subject -eq 'CN=JSL Audio Test' } | select -First 1
$st='D:\work\nuget\Microsoft.Windows.SDK.CPP\c\bin\10.0.26100.0\x64\signtool.exe'
& $st sign /fd SHA256 /sha1 $cert.Thumbprint "$p\jsl$n.sys" "$p\jsl$n.cat" | Select -Last 2
