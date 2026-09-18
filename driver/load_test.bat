@echo off
rem load_test.bat - 在虚拟机内加载 tgshadow 驱动（需管理员 + 测试签名已开启）
setlocal
set "SYS=%~dp0build\Debug\tgshadow.sys"
if not exist "%SYS%" goto no_sys

echo [1/3] 清理旧服务实例...
sc stop tgshadow >nul 2>&1
sc delete tgshadow >nul 2>&1

echo [2/3] 创建并启动内核服务...
sc create tgshadow type= kernel start= demand error= normal binPath= "%SYS%"
if errorlevel 1 goto create_fail
sc start tgshadow
if errorlevel 1 goto start_fail

echo [3/3] 状态:
sc query tgshadow
echo.
echo 提示: 用 DbgView(管理员) 或 WinDbg 查看 [TgShadow] 前缀的内核日志
exit /b 0

:no_sys
echo [ERROR] 找不到驱动: %SYS%
exit /b 1

:create_fail
echo [ERROR] sc create 失败
exit /b 1

:start_fail
echo [ERROR] sc start 失败 - 常见原因:
echo   1) 未开启测试签名: bcdedit /set testsigning on 后需重启
echo   2) 驱动未签名或证书未导入 受信任的根/发布者
echo   3) DriverEntry 返回错误（WinDbg/DbgView 看 DbgPrint）
exit /b 1
