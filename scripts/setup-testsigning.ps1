# SPDX-License-Identifier: MIT
# (Administrator) One-time setup for loading a self-signed (test-signed) driver:
#   1. create a code-signing certificate in CurrentUser\My (used by build.ps1)
#   2. trust it machine-wide (Root + TrustedPublisher)
#   3. enable test-signing mode (takes effect after a reboot)
# SECURITY: anything signed with this certificate's private key will be trusted by this PC.
#   Keep the key on this machine; never share or commit it.
# Undo: bcdedit /set testsigning off, and delete the certificate from certlm.msc / certmgr.msc.
param([string]$Subject = 'CN=JSL Audio Test')
$ErrorActionPreference = 'Stop'
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\CurrentUser\My `
            -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$cer = Join-Path $env:TEMP 'jslaudio-test.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Remove-Item $cer
bcdedit /set testsigning on
'Done. Reboot to enable test-signing mode.'
