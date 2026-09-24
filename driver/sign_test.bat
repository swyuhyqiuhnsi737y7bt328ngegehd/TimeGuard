@echo off
rem sign_test.bat - 自签名测试证�?+ 签名 tgshadow.sys（虚拟机内管理员运行�?
setlocal
set "SYS=%~dp0build\Debug\tgshadow.sys"
set SUBJECT=CN=TimeGuard Debug
set "CER=%~dp0TimeGuardTest.cer"
if not exist "%SYS%" goto no_sys

echo [1/3] 生成自签名代码签名证书（若不存在�?..
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $cer='%CER%'; $c = Get-ChildItem Cert:\CurrentUser\My ^| Where-Object { $_.Subject -eq $s } ^| Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $s -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; Write-Host ('证书指纹: ' + $c.Thumbprint); Export-Certificate -Cert $c -FilePath $cer -Force ^| Out-Null"
if errorlevel 1 goto ps_fail

echo [2/3] 导入�?受信任的根证书颁发机�?/ 受信任的发布�?...
certutil -addstore -f Root "%CER%" >nul
certutil -addstore -f TrustedPublisher "%CER%" >nul

echo [3/3] 签名驱动 ...
signtool sign /v /fd sha256 /a /n "%SUBJECT%" "%SYS%"
if errorlevel 1 goto sign_fail
signtool verify /v /pa "%SYS%"
echo.
echo 签名完成: %SYS%
exit /b 0

:no_sys
echo [ERROR] 找不到驱�? %SYS% - 请先运行 build_driver.bat
exit /b 1

:ps_fail
echo [ERROR] 证书生成失败
exit /b 1

:sign_fail
echo [ERROR] 签名失败
exit /b 1
