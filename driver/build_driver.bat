@echo off
rem build_driver.bat - 构建 tgshadow.sys（cl/link 直连，兼容 VS BuildTools）
setlocal enabledelayedexpansion
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set "WDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set WDK_VER=10.0.26100.0
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" goto no_vcvars
if not exist "%WDK_ROOT%\Include\%WDK_VER%\km\ntddk.h" goto no_wdk
goto env_ok

:no_vcvars
echo [ERROR] 找不到 vcvars64.bat: %VCVARS%
exit /b 1

:no_wdk
echo [ERROR] 找不到 WDK 内核头文件，请先安装 WDK %WDK_VER%
echo         安装命令: winget install Microsoft.WindowsWDK.10.0.26100
exit /b 1

:env_ok
call "%VCVARS%" >nul
if errorlevel 1 goto vcvars_fail

set "SRC=%~dp0tgshadow"
set "OUT=%~dp0build\%CONFIG%"
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%SRC%"

set CFLAGS=/nologo /c /kernel /GS- /Gm- /Zp8 /Gy /W4 /WX- /Od /Zi /Zc:wchar_t /Zc:forScope /Zc:inline /GR-
set CFLAGS=%CFLAGS% /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000000 /DWIN32=100 /DAMD64 /D_AMD64_ /utf-8
set INCS=/I"%WDK_ROOT%\Include\%WDK_VER%\km" /I"%WDK_ROOT%\Include\%WDK_VER%\km\crt" /I"%WDK_ROOT%\Include\%WDK_VER%\shared" /I"%WDK_ROOT%\Include\%WDK_VER%\ucrt"
set LIBS="%WDK_ROOT%\Lib\%WDK_VER%\km\x64\ntoskrnl.lib" "%WDK_ROOT%\Lib\%WDK_VER%\km\x64\hal.lib" "%WDK_ROOT%\Lib\%WDK_VER%\km\x64\wdm.lib" "%WDK_ROOT%\Lib\%WDK_VER%\km\x64\BufferOverflowFastFailK.lib"

echo [1/2] 编译 tgshadow.c ...
cl %CFLAGS% %INCS% /Fo"%OUT%\tgshadow.obj" /Fd"%OUT%\tgshadow.pdb" tgshadow.c
if errorlevel 1 goto compile_fail

echo [2/2] 链接 tgshadow.sys ...
link /nologo /DRIVER /SUBSYSTEM:NATIVE /NODEFAULTLIB /ENTRY:DriverEntry /MACHINE:X64 /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG /OUT:"%OUT%\tgshadow.sys" "%OUT%\tgshadow.obj" %LIBS%
if errorlevel 1 goto link_fail

echo.
echo 构建成功: %OUT%\tgshadow.sys
exit /b 0

:vcvars_fail
echo [ERROR] vcvars64 初始化失败
exit /b 1

:compile_fail
echo [ERROR] 编译失败
exit /b 1

:link_fail
echo [ERROR] 链接失败
exit /b 1
