@echo off
rem ============================================================================
rem  vm_test.bat - 虚拟机内一键：停服务 -> 签名 -> 加载 -> 查询
rem  以【管理员】运行。前置: bcdedit /set testsigning on 且已重启
rem  目录要求: 本脚本与 tgshadow.sys / tgshadowctl.exe 在同一目录
rem ============================================================================
setlocal
set "DIR=%~dp0"
set "SYS=%DIR%tgshadow.sys"
set "SUBJECT=CN=TimeGuard Test Signing"

if not exist "%SYS%" goto no_sys

echo [1/5] 停止并删除旧服务（驱动加载时会占用 sys 文件，必须先停）...
sc stop tgshadow >nul 2>&1
sc delete tgshadow >nul 2>&1
ping -n 3 127.0.0.1 >nul

echo [2/5] 生成/复用自签名代码签名证书...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My ^| Where-Object { $_.Subject -eq $s } ^| Select-Object -First 1; if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $s -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) }; Export-Certificate -Cert $c -FilePath '%DIR%tg.cer' -Force ^| Out-Null; Write-Host ('证书指纹: ' + $c.Thumbprint)"
if errorlevel 1 goto ps_fail

echo [3/5] 导入证书到 受信任的根 / 受信任的发布者 ...
certutil -addstore -f Root "%DIR%tg.cer" >nul
certutil -addstore -f TrustedPublisher "%DIR%tg.cer" >nul

echo [4/5] 签名驱动...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s='%SUBJECT%'; $c = Get-ChildItem Cert:\CurrentUser\My ^| Where-Object { $_.Subject -eq $s } ^| Select-Object -First 1; $r = Set-AuthenticodeSignature -FilePath '%SYS%' -Certificate $c; Write-Host ('签名状态: ' + $r.Status)"
if errorlevel 1 goto sign_fail

echo [5/5] 创建并启动内核服务...
sc create tgshadow type= kernel start= demand error= normal binPath= "%SYS%"
if errorlevel 1 goto create_fail
sc start tgshadow
if errorlevel 1 goto start_fail
sc query tgshadow
echo.
echo 下一步验证命令:
echo    tgshadowctl.exe version
echo    tgshadowctl.exe volumes
echo    tgshadowctl.exe enable ^<卷号^> 32768
echo    tgshadowctl.exe status
exit /b 0

:no_sys
echo [ERROR] 当前目录没有 tgshadow.sys
exit /b 1

:ps_fail
echo [ERROR] 证书生成失败
exit /b 1

:sign_fail
echo [ERROR] 签名失败（若提示文件被占用，请确认服务已停止: sc stop tgshadow）
exit /b 1

:create_fail
echo [ERROR] sc create 失败
exit /b 1

:start_fail
echo [ERROR] sc start 失败 - 检查测试签名是否已开启并重启过
exit /b 1
