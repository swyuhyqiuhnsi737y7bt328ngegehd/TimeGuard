@echo off
rem Alternative build with Nuitka (slower, optional).
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

echo [1/6] Installing build deps (nuitka zstandard pystray pillow) ...
python -m pip install nuitka zstandard pystray pillow
if errorlevel 1 (
  echo [WARN] pip install failed, trying to continue with installed packages...
  python -c "import nuitka" >nul 2>&1 || ( echo [ERROR] Nuitka not available & exit /b 1 )
)
set "NUI=python -m nuitka --onefile --windows-console-mode=disable --enable-plugin=tk-inter --assume-yes-for-downloads --output-dir=dist --remove-output"
echo [2/6] core.exe
%NUI% --output-filename=core.exe src\core\controller.py || exit /b 1
echo [3/6] guardian.exe
%NUI% --output-filename=guardian.exe src\guard\watchdog.py || exit /b 1
echo [4/6] lockscreen.exe
%NUI% --output-filename=lockscreen.exe src\lock\lockscreen.py || exit /b 1
echo [5/6] admin.exe
%NUI% --output-filename=admin.exe src\gui\admin.py || exit /b 1
echo [6/6] Copying default config (only if missing, keep user settings)
if not exist dist\config mkdir dist\config
if not exist dist\config\policy.json copy /y config\policy.json dist\config\ >nul
echo Done: 4 exes in dist
