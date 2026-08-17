@echo off
rem Build the 4 Python modules with PyInstaller.
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
@rem clean state but PRESERVE config.key (MAC key) and policy.bak (signed backup)
if exist dist\state (
  del /q dist\state\quit.flag dist\state\core.pid dist\state\lockscreen.pid dist\state\fileguard.pid dist\state\spawn_lock dist\state\installed.flag 2>nul
  del /q dist\state\guard_*.json dist\state\usage.json 2>nul
  if exist dist\state\logs rd /s /q dist\state\logs 2>nul
)
@rem ===== end stop block =====

echo [1/6] Installing build deps (pyinstaller pystray pillow) ...
python -m pip install pyinstaller pystray pillow
if errorlevel 1 (
  echo [WARN] pip install failed, trying to continue with installed packages...
  python -c "import PyInstaller" >nul 2>&1 || ( echo [ERROR] PyInstaller not available & exit /b 1 )
)
echo [2/6] core.exe (main controller + tray)
python -m PyInstaller --noconfirm --clean --onefile --windowed --name core --paths src src\core\controller.py
if errorlevel 1 exit /b 1
echo [3/6] guardian.exe (watchdog, copied to random names at install)
python -m PyInstaller --noconfirm --clean --onefile --windowed --name guardian --paths src src\guard\watchdog.py
if errorlevel 1 exit /b 1
echo [4/6] lockscreen.exe (lock screen)
python -m PyInstaller --noconfirm --clean --onefile --windowed --name lockscreen --paths src src\lock\lockscreen.py
if errorlevel 1 exit /b 1
echo [5/6] admin.exe (parent console)
python -m PyInstaller --noconfirm --clean --onefile --windowed --name admin --paths src src\gui\admin.py
if errorlevel 1 exit /b 1
echo [6/6] Copying default config (only if missing, keep user settings)
if not exist dist\config mkdir dist\config
if not exist dist\config\policy.json copy /y config\policy.json dist\config\ >nul
echo Done: dist\core.exe guardian.exe lockscreen.exe admin.exe
