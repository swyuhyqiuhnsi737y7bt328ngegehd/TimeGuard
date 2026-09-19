@echo off
rem ============================================================================
rem  vm_test.bat - one-click: stop service -> sign -> load -> query
rem  Run as Administrator. Prereq: bcdedit /set testsigning on  (then reboot)
rem  Files in same dir: tgshadow.sys
rem  NOTE: output is English on purpose - a UTF-8 .bat printed on a GBK console
rem        shows mojibake, which looked like a bug of its own.
rem ============================================================================
setlocal
set "DIR=%~dp0"
set "SYS=%DIR%tgshadow.sys"
set "SUBJECT=CN=TimeGuard Test Signing"

if not exist "%SYS%" goto no_sys

echo [1/5] Stopping and deleting old service (a loaded driver locks the .sys file)...
sc stop tgshadow >nul 2>&1
sc delete tgshadow >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo [2/5] Creating or reusing self-signed code-signing certificate...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $s -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; if (-not $c) { Write-Host 'ERROR: certificate creation failed'; exit 1 }; Export-Certificate -Cert $c -FilePath '%DIR%tg.cer' -Force | Out-Null; Write-Host ('cert thumbprint: ' + $c.Thumbprint)"
if errorlevel 1 goto ps_fail

echo [3/5] Importing certificate into Trusted Root and Trusted Publisher...
certutil -addstore -f Root "%DIR%tg.cer" >nul
certutil -addstore -f TrustedPublisher "%DIR%tg.cer" >nul

echo [4/5] Signing driver...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { Write-Host 'ERROR: certificate not found'; exit 1 }; $r = Set-AuthenticodeSignature -FilePath '%SYS%' -Certificate $c; Write-Host ('sign status: ' + $r.Status); if ($r.Status -ne 'Valid') { Write-Host 'ERROR: signature invalid (file locked? stop the service first)'; exit 1 }"
if errorlevel 1 goto sign_fail

echo [5/5] Creating and starting kernel service...
sc create tgshadow type= kernel start= demand error= normal binPath= "%SYS%"
if errorlevel 1 goto create_fail
sc start tgshadow
if errorlevel 1 goto start_fail
echo.
sc query tgshadow
echo.
echo OK. Next:  tgshadowctl.exe version ^| volumes ^| enable ^<volume^> 32768 ^| status
exit /b 0

:no_sys
echo [ERROR] tgshadow.sys not found in current directory
exit /b 1

:ps_fail
echo [ERROR] certificate creation failed
exit /b 1

:sign_fail
echo [ERROR] signing failed - if the file is locked, stop the service first
exit /b 1

:create_fail
echo [ERROR] sc create failed
exit /b 1

:start_fail
echo [ERROR] sc start failed - check testsigning is on and the machine was rebooted
exit /b 1
