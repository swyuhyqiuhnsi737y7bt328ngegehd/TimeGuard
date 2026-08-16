@echo off
rem Compile fileguard.exe (pure C + Win32 API) with Cygwin gcc.
setlocal
cd /d "%~dp0\.."
if not exist dist mkdir dist
set "GCC="
for /f "delims=" %%i in ('where gcc 2^>nul') do if not defined GCC set "GCC=%%i"
if not defined GCC (
  if exist "D:\cygwin64\bin\gcc.exe" set "GCC=D:\cygwin64\bin\gcc.exe"
  if exist "C:\cygwin64\bin\gcc.exe" set "GCC=C:\cygwin64\bin\gcc.exe"
  if exist "C:\cygwin\bin\gcc.exe"   set "GCC=C:\cygwin\bin\gcc.exe"
)
if not defined GCC (
  echo [ERROR] gcc not found. Install Cygwin with gcc-core, or add cygwin64\bin to PATH.
  exit /b 1
)
echo Using gcc: %GCC%
echo [1/2] Compiling src\protect\fileguard.c ...
"%GCC%" -O2 -s -mwindows -o dist\fileguard.exe src\protect\fileguard.c
if errorlevel 1 ( echo [ERROR] compile failed & exit /b 1 )
echo [2/2] Copying cygwin1.dll ...
for %%I in ("%GCC%") do set "GDIR=%%~dpI"
if exist "%GDIR%cygwin1.dll" copy /y "%GDIR%cygwin1.dll" dist\ >nul
echo Done: dist\fileguard.exe
