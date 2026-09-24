@echo off
rem ============================================================================
rem  vm_test.bat [source_dir]  -  deploy + sign + load the tgshadow driver
rem
rem  Run as Administrator inside the VM.
rem  Prereq: bcdedit /set testsigning on   (then reboot once)
rem
rem  source_dir (optional): folder containing the freshly built
rem      tgshadow.sys / tgshadowctl.exe.
rem    It is copied AFTER the service is stopped, because a loaded driver
rem    LOCKS the .sys file - copying while it runs fails (Explorer may just
rem    show an error you can miss), leaving the VM on an old build.
rem    Example:  vm_test.bat "\\vmware-host\Shared Folders\share\Debug"
rem ============================================================================
setlocal
set "DIR=%~dp0"
set "SYS=%DIR%tgshadow.sys"
set "SUBJECT=CN=TimeGuard Debug"
set "CER=%DIR%tg.cer"

echo [0/6] Stopping old service (releases the .sys file lock)...
sc stop tgshadow >nul 2>&1
sc delete tgshadow >nul 2>&1
ping -n 3 127.0.0.1 >nul

if "%~1"=="" goto skip_copy
echo [0/6] Copying new binaries from %~1 ...
if not exist "%~1\tgshadow.sys" goto src_missing
copy /y "%~1\tgshadow.sys" "%SYS%" >nul
if exist "%~1\tgshadowctl.exe" copy /y "%~1\tgshadowctl.exe" "%DIR%tgshadowctl.exe" >nul
echo        copied OK
:skip_copy

if not exist "%SYS%" goto no_sys

echo        current driver file:
for %%F in ("%SYS%") do echo          %%~nxF  size=%%~zF  modified=%%~tF

echo [1/6] Creating or reusing self-signed code-signing certificate...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $s -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; if (-not $c) { Write-Host 'ERROR: certificate creation failed'; exit 1 }; Export-Certificate -Cert $c -FilePath '%CER%' -Force | Out-Null; Write-Host ('cert thumbprint: ' + $c.Thumbprint)"
if errorlevel 1 goto cert_fail

echo [2/6] Importing certificate into Trusted Root and Trusted Publisher...
certutil -addstore -f Root "%CER%" >nul
certutil -addstore -f TrustedPublisher "%CER%" >nul

echo [3/6] Signing driver...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $s } | Select-Object -First 1; if (-not $c) { Write-Host 'ERROR: certificate not found'; exit 1 }; $r = Set-AuthenticodeSignature -FilePath '%SYS%' -Certificate $c; Write-Host ('sign status: ' + $r.Status); if ($r.Status -ne 'Valid') { Write-Host 'ERROR: signature invalid (file locked? stop the service first)'; exit 1 }"
if errorlevel 1 goto sign_fail

echo [4/6] Creating kernel service...
sc create tgshadow type= kernel start= demand error= normal binPath= "%SYS%"
if errorlevel 1 goto create_fail

echo [5/6] Starting service...
sc start tgshadow
if errorlevel 1 goto start_fail

echo [6/6] Verifying...
sc query tgshadow | findstr /C:"STATE"
reg query "HKLM\SOFTWARE\TimeGuard" /v TgShadowLastStep 2>nul
echo.
echo OK. Next:
echo    tgshadowctl.exe version
echo    tgshadowctl.exe enable ^<volume^> 32768 attachonly    ^(safe step^)
echo    tgshadowctl.exe status
exit /b 0

:src_missing
echo [ERROR] %~1\tgshadow.sys not found
exit /b 1

:no_sys
echo [ERROR] tgshadow.sys not found in current directory
exit /b 1

:cert_fail
echo [ERROR] certificate creation failed
exit /b 1

:sign_fail
echo [ERROR] signing failed
exit /b 1

:create_fail
echo [ERROR] sc create failed
exit /b 1

:start_fail
echo [ERROR] sc start failed - testsigning on? file signed? See EMERGENCY.md
exit /b 1
