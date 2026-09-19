@echo off
rem ============================================================================
rem  install_upperfilter.bat - 把 tgshadow 注册为【卷 class 的 Upper Filter】
rem
rem  必须在虚拟机内以管理员身份运行，然后重启。
rem  前置：tgshadow.sys 已用测试证书签名（sign.bat），证书已导入受信任根。
rem
rem  ⚠️ 三条硬性要求（全部踩过坑，详见 README）：
rem    1) 驱动必须复制到 %SystemRoot%\System32\drivers\：
rem       Upper Filter 是 BOOT_START 驱动，启动早期只能访问系统盘。
rem    2) 服务 StartType 必须为 boot(0)：
rem       卷设备在 BOOT 阶段就被 partmgr 枚举，system(1) 太晚，PnP 不会加载我们
rem       （症状：sc query 显示 STOPPED + WIN32_EXIT_CODE 1077 NEVER_STARTED）。
rem    3) UpperFilters 必须【追加】而不是覆盖（默认值 volsnap 要被保留）。
rem ============================================================================
setlocal
set SRC=%~dp0tgshadow.sys
set DST=%SystemRoot%\System32\drivers\tgshadow.sys
set CLASSKEY=HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}
set LOG=%~dp0install_upperfilter.log

echo === install tgshadow as volume upper filter === > "%LOG%"
echo time: %DATE% %TIME% >> "%LOG%"

if not exist "%SRC%" goto no_sys

echo [1/5] copy driver into System32\drivers >> "%LOG%"
copy /y "%SRC%" "%DST%" >> "%LOG%" 2>&1
if errorlevel 1 goto copy_fail

echo [2/5] create/update service (BOOT_START) >> "%LOG%"
sc stop tgshadow >nul 2>&1
sc delete tgshadow >nul 2>&1
sc create tgshadow type= kernel start= boot error= normal binPath= "\SystemRoot\System32\drivers\tgshadow.sys" >> "%LOG%" 2>&1
if errorlevel 1 goto svc_fail

echo [3/5] append tgshadow to Volume class UpperFilters >> "%LOG%"
powershell -NoProfile -Command "$k='HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}'; $cur=(Get-ItemProperty -Path $k -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters; if (-not $cur) { $cur=@('volsnap') }; if ($cur -notcontains 'tgshadow') { Set-ItemProperty -Path $k -Name UpperFilters -Value (@($cur)+'tgshadow') -Type MultiString -Force }; Write-Host ('UpperFilters = ' + (((Get-ItemProperty -Path $k -Name UpperFilters).UpperFilters) -join ','))" >> "%LOG%" 2>&1

echo [4/5] verify >> "%LOG%"
sc qc tgshadow >> "%LOG%" 2>&1
reg query "%CLASSKEY%" /v UpperFilters >> "%LOG%" 2>&1

echo [5/5] done - REBOOT REQUIRED >> "%LOG%"
echo INSTALL_DONE >> "%LOG%"
echo.
echo Driver registered as volume upper filter. REBOOT the VM to activate.
echo Log: %LOG%
exit /b 0

:no_sys
echo [ERROR] tgshadow.sys not found next to this script
exit /b 1

:copy_fail
echo [ERROR] copy to drivers dir failed
exit /b 1

:svc_fail
echo [ERROR] service creation failed
exit /b 1
