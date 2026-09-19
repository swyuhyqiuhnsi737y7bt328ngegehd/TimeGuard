@echo off
rem ============================================================================
rem  sign.bat - sign tgshadow.sys with the self-signed test certificate
rem  Run as Administrator.
rem  Use this EVERY TIME you replace tgshadow.sys: the signature covers the file
rem  hash, so a new file has no valid signature -> sc start fails with 577.
rem  If the driver is loaded the file is locked: stop it first
rem      sc.exe stop tgshadow
rem  or just use vm_test.bat which does stop+sign+load in one go.
rem
rem  NOTE (shell syntax): inside "powershell -Command ..." the redirection must
rem  be PowerShell syntax (| Out-Null / > $null). Writing cmd's ">nul" there
rem  fails with "FileStream ... not a file" - keep certutil calls in the .bat
rem  layer where ">nul" is correct.
rem ============================================================================
setlocal
set "DIR=%~dp0"
set "SYS=%DIR%tgshadow.sys"
set "SUBJECT=CN=TimeGuard Test Signing"
set "CER=%DIR%tg.cer"

if not exist "%SYS%" goto no_sys

echo [1/3] Creating or reusing self-signed code-signing certificate...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $s -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; if (-not $c) { Write-Host 'ERROR: certificate creation failed'; exit 1 }; Export-Certificate -Cert $c -FilePath '%CER%' -Force | Out-Null; Write-Host ('cert thumbprint: ' + $c.Thumbprint)"
if errorlevel 1 goto cert_fail

echo [2/3] Importing certificate into Trusted Root and Trusted Publisher...
certutil -addstore -f Root "%CER%" >nul
certutil -addstore -f TrustedPublisher "%CER%" >nul

echo [3/3] Signing driver...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { Write-Host 'ERROR: certificate not found'; exit 1 }; $r = Set-AuthenticodeSignature -FilePath '%SYS%' -Certificate $c; Write-Host ('sign status: ' + $r.Status); if ($r.Status -ne 'Valid') { exit 1 }"
if errorlevel 1 goto sign_fail

echo.
echo OK: driver signed. Next:  sc.exe start tgshadow
exit /b 0

:no_sys
echo [ERROR] tgshadow.sys not found in current directory
exit /b 1

:cert_fail
echo [ERROR] certificate creation failed
exit /b 1

:sign_fail
echo [ERROR] signing failed - if the driver is loaded the file is locked;
echo         run  sc.exe stop tgshadow  then retry.
exit /b 1
