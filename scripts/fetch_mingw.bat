@echo off
rem Fetch a portable MinGW-w64 toolchain (w64devkit) into tools\w64devkit .
rem No installer, no admin rights: it is a self-extracting 7z archive.
rem Called automatically by build_cpp.bat when no MinGW gcc is found.
setlocal
cd /d "%~dp0\.."
set "VER=2.10.0"
set "NAME=w64devkit-x64-%VER%.7z.exe"
set "URL=https://github.com/skeeto/w64devkit/releases/download/v%VER%/%NAME%"
set "DL=tools\%NAME%"
if not exist tools mkdir tools

if exist "tools\w64devkit\bin\gcc.exe" (
  echo [mingw] already present: tools\w64devkit\bin\gcc.exe
  exit /b 0
)

where curl >nul 2>&1
if errorlevel 1 (
  echo [ERROR] curl not found; cannot auto-download w64devkit.
  echo         Download manually from %URL%
  echo         and extract it so that tools\w64devkit\bin\gcc.exe exists.
  exit /b 1
)

rem 上次下载可能中断，留下一个截断的文件（解压会失败）—— 小于 50MB 一律重下
for %%F in ("%DL%") do if %%~zF LSS 52428800 del /q "%DL%" 2>nul

if not exist "%DL%" (
  echo [mingw] downloading %NAME% ^(~64 MB, one time^) ...
  rem -C - resumes a previously interrupted download
  curl -L --fail --retry 5 --retry-delay 3 -C - -o "%DL%" "%URL%"
  if errorlevel 1 (
    echo [ERROR] download failed. Check the network, or fetch it manually from
    echo         %URL%
    exit /b 1
  )
)

echo [mingw] extracting to tools\w64devkit ...
rem 注意：7z 自解压包的 -o 是相对【当前目录】的，且包内顶层就是 w64devkit\。
rem 之前写成 pushd tools + -otools 会解到 tools\tools\w64devkit（多一层）。
"%DL%" -y -otools >nul
set "RC=%errorlevel%"
if not "%RC%"=="0" (
  echo [ERROR] extraction failed ^(exit %RC%^).
  exit /b 1
)
if not exist "tools\w64devkit\bin\gcc.exe" (
  echo [ERROR] extracted, but tools\w64devkit\bin\gcc.exe is missing.
  exit /b 1
)
echo [mingw] ready: tools\w64devkit\bin\gcc.exe
rem 安装包本身不再需要，删掉省 64MB（下次要用会重新下）
del /q "%DL%" 2>nul
exit /b 0