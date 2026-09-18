@echo off
rem build_ctl.bat - 构建用户态控制工具 tgshadowctl.exe
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%" >nul

set "SRC=%~dp0"
set "OUT=%~dp0build\Debug"
if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /W4 /Od /Zi /utf-8 /I"%SRC%tgshadow" /D_UNICODE /DUNICODE /Fo"%OUT%\tgshadowctl.obj" /Fe"%OUT%\tgshadowctl.exe" "%SRC%tgshadowctl.c"
if errorlevel 1 goto compile_fail
echo 构建成功: %OUT%\tgshadowctl.exe
exit /b 0

:no_vcvars
echo [ERROR] 找不到 vcvars64.bat
exit /b 1

:compile_fail
echo [ERROR] 编译失败
exit /b 1
