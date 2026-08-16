@echo off
rem One-click build: C fileguard + 4 Python exes.
setlocal
cd /d "%~dp0\.."
@rem ===== stop running TimeGuard ring before build =====
if not exist dist mkdir dist
if not exist dist\state mkdir dist\state 2>nul
powershell -NoProfile -Command "try { [IO.File]::WriteAllText('dist\state\quit.flag', ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString())) } catch {}"
echo [0/6] Stopping running TimeGuard processes...
ping -n 7 127.0.0.1 >nul
taskkill /F /IM core.exe >nul 2>&1
taskkill /F /IM lockscreen.exe >nul 2>&1
taskkill /F /IM guardian.exe >nul 2>&1
taskkill /F /IM fileguard.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
rmdir /s /q dist\state >nul 2>&1
@rem ===== end stop block =====

call "%~dp0build_cpp.bat"
if errorlevel 1 ( echo [ERROR] C build failed & exit /b 1 )
call "%~dp0build_pyinstaller.bat"
if errorlevel 1 ( echo [ERROR] PyInstaller build failed & exit /b 1 )
echo.
echo All builds finished. Output in dist\ . Next: python src\main.py install
