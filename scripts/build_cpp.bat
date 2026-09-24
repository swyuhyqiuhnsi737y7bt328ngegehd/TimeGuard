@echo off
rem Compile fileguard.exe (pure C + Win32 API) with MinGW-w64.
rem MinGW-w64 produces a self-contained exe (no extra DLL to ship),
rem unlike Cygwin which needs cygwin1.dll next to it.
setlocal
cd /d "%~dp0\.."
if not exist dist mkdir dist

rem ---- locate a MinGW-w64 gcc ----
set "GCC="
rem 1) portable toolchain bundled in the repo (no system install needed)
for %%V in (tools\w64devkit\bin\gcc.exe) do if exist "%%~fV" set "GCC=%%~fV"
rem 2) any gcc on PATH that is really MinGW (not Cygwin/MSYS - those need extra DLLs)
if not defined GCC (
  for /f "delims=" %%i in ('where gcc 2^>nul') do (
    if not defined GCC (
      "%%i" -dumpmachine 2>nul | findstr /i "mingw" >nul && set "GCC=%%i"
    )
  )
)
rem 3) common install locations
if not defined GCC (
  for %%P in (
    "C:\msys64\mingw64\bin\gcc.exe"
    "D:\msys64\mingw64\bin\gcc.exe"
    "C:\mingw64\bin\gcc.exe"
    "D:\mingw64\bin\gcc.exe"
    "C:\ProgramData\mingw64\mingw64\bin\gcc.exe"
  ) do if not defined GCC if exist %%P set "GCC=%%~P"
)

rem 4) still nothing -> fetch the portable w64devkit into tools\ (no install, no admin)
if not defined GCC call "%~dp0fetch_mingw.bat"
if exist "tools\w64devkit\bin\gcc.exe" set "GCC=%CD%\tools\w64devkit\bin\gcc.exe"

if not defined GCC (
  echo [ERROR] MinGW-w64 gcc not found.
  echo         Options:
  echo           1^) unzip a portable w64devkit into tools\w64devkit\  ^(recommended^)
  echo           2^) install MSYS2 and add C:\msys64\mingw64\bin to PATH
  echo           3^) put any MinGW-w64 gcc on PATH
  exit /b 1
)
for /f "delims=" %%i in ('"%GCC%" -dumpmachine') do set "TARGET=%%i"
echo Using gcc   : %GCC%
echo Target     : %TARGET%
echo %TARGET% | findstr /i "mingw" >nul
if errorlevel 1 (
  echo [ERROR] that gcc is not MinGW-w64 ^(target "%TARGET%"^).
  echo         Cygwin/MSYS gcc needs extra DLLs next to the exe; use MinGW-w64 instead.
  exit /b 1
)

echo [1/1] Compiling src\protect\fileguard.c ...
"%GCC%" -O2 -s -mwindows -o dist\fileguard.exe src\protect\fileguard.c
if errorlevel 1 ( echo [ERROR] compile failed & exit /b 1 )

rem 旧构建可能留下 Cygwin 的运行时 DLL，MinGW 版本不需要它，顺手清掉避免混淆
if exist dist\cygwin1.dll del /q dist\cygwin1.dll
echo Done: dist\fileguard.exe ^(self-contained, no extra DLL^)