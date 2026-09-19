@echo off
rem ============================================================================
rem  sign.bat - sign tgshadow.sys with the self-signed test certificate
rem  Run as Administrator.
rem  Use this EVERY TIME you replace tgshadow.sys: the signature covers the
rem  file hash, so a new file has no valid signature -> sc start fails with 577.
rem  If the driver is currently loaded the file is locked: stop it first
rem  (sc.exe stop tgshadow) or use vm_test.bat which does everything.
rem ============================================================================
setlocal
set "DIR=%~dp0"
set "SYS=%DIR%tgshadow.sys"
set "SUBJECT=CN=TimeGuard Test Signing"

if not exist "%SYS%" goto no_sys

powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $s -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; $cer = '%DIR%tg.cer'; Export-Certificate -Cert $c -FilePath $cer -Force | Out-Null; certutil -addstore -f Root $cer >nul 2>&1; certutil -addstore -f TrustedPublisher $cer >nul 2>&1; $r = Set-AuthenticodeSignature -FilePath '%SYS%' -Certificate $c; Write-Host ('sign status: ' + $r.Status); if ($r.Status -ne 'Valid') { exit 1 }"
if errorlevel 1 goto sign_fail

echo.
echo OK: driver signed. Next:  sc.exe start tgshadow
exit /b 0

:no_sys
echo [ERROR] tgshadow.sys not found in current directory
exit /b 1

:sign_fail
echo [ERROR] signing failed - if the driver is loaded the file is locked;
echo         run  sc.exe stop tgshadow  then retry.
exit /b 1
