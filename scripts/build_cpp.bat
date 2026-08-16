@echo off
rem 编译 C 文件自锁程序 fileguard.exe（Cygwin gcc，纯 C + Windows API，无需 g++/libstdc++）
setlocal
cd /d "%~dp0\.."
if not exist dist mkdir dist
set "GCC=gcc"
where gcc >nul 2>nul
if errorlevel 1 (
  if exist "C:\cygwin64\bin\gcc.exe" set "GCC=C:\cygwin64\bin\gcc.exe"
  if exist "C:\cygwin\bin\gcc.exe"   set "GCC=C:\cygwin\bin\gcc.exe"
  if exist "D:\cygwin64\bin\gcc.exe" set "GCC=D:\cygwin64\bin\gcc.exe"
)
if not exist "%GCC%" (
  echo [错误] 找不到 gcc，请安装 Cygwin 并勾选 gcc-core 包，或把 cygwin64\bin 加入 PATH
  exit /b 1
)
echo [1/2] 编译 src\protect\fileguard.c ...
"%GCC%" -O2 -s -mwindows -o dist\fileguard.exe src\protect\fileguard.c
if errorlevel 1 ( echo [错误] 编译失败 & exit /b 1 )
echo [2/2] 复制运行时依赖 cygwin1.dll ...
for %%I in ("%GCC%") do set "GDIR=%%~dpI"
if exist "%GDIR%cygwin1.dll" copy /y "%GDIR%cygwin1.dll" dist\ >nul & echo 已复制 %GDIR%cygwin1.dll
echo 完成: dist\fileguard.exe
