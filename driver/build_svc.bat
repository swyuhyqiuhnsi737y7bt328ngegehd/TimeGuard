@echo off
rem build_svc.bat - 构建 P4 控制器：tgshadow_svc.exe（服务） + tgshadow_ask.exe（关机确认对话框�?
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%" >nul
if errorlevel 1 goto no_vcvars

set "SRC=%~dp0"
set "OUT=%~dp0build\Debug"
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%SRC%"

echo [1/2] 编译 tgshadow_svc.exe ...
cl /nologo /utf-8 /W3 /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"%~dp0tgshadow" /Fo"%OUT%\svc_" /Fe"%OUT%\tgshadow_svc.exe" tgshadow_svc.c /link /SUBSYSTEM:CONSOLE wtsapi32.lib userenv.lib advapi32.lib user32.lib
if errorlevel 1 goto fail

echo [2/2] 编译 tgshadow_ask.exe ...
cl /nologo /utf-8 /W3 /O2 /MD /D_CRT_SECURE_NO_WARNINGS /I"%~dp0tgshadow" /Fo"%OUT%\ask_" /Fe"%OUT%\tgshadow_ask.exe" tgshadow_ask.c /link /SUBSYSTEM:WINDOWS bcrypt.lib user32.lib gdi32.lib shell32.lib advapi32.lib
if errorlevel 1 goto fail

echo.
echo 构建成功: %OUT%\tgshadow_svc.exe
echo 构建成功: %OUT%\tgshadow_ask.exe
exit /b 0
:no_vcvars
echo [ERROR] 找不�?vcvars64.bat: %VCVARS%
exit /b 1
:fail
echo [ERROR] 构建失败
exit /b 1
