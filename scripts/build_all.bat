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
@rem clean state but PRESERVE config.key (MAC key) and policy.bak (signed backup)
if exist dist\state (
  del /q dist\state\quit.flag dist\state\core.pid dist\state\lockscreen.pid dist\state\fileguard.pid dist\state\spawn_lock dist\state\installed.flag 2>nul
  del /q dist\state\guard_*.json dist\state\usage.json 2>nul
  if exist dist\state\logs rd /s /q dist\state\logs 2>nul
)
@rem remove leftover random-named guardian copies from old sessions
powershell -NoProfile -Command "Get-ChildItem 'dist\*.exe' | Where-Object { $_.BaseName -match '^[a-z0-9]{8}$' } | Remove-Item -Force -ErrorAction SilentlyContinue"
@rem ===== end stop block =====

call "%~dp0build_cpp.bat"
if errorlevel 1 ( echo [ERROR] C build failed & exit /b 1 )
call "%~dp0build_pyinstaller.bat"
if errorlevel 1 ( echo [ERROR] PyInstaller build failed & exit /b 1 )
echo.
echo All builds finished. Output in dist\ . Next: python src\main.py install
@rem ===== 内核驱动 + 影子服务：复制到 dist\driver（deploy() 从这里取） =====
if not exist "dist\driver" mkdir "dist\driver" 2>nul
set "DRVSRC=driver\build\Debug"
if not exist "%DRVSRC%\tgshadow.sys" (
  echo [WARN] 未找到 %DRVSRC%\tgshadow.sys —— 先运行 driver\build_driver.bat
) else (
  copy /y "%DRVSRC%\tgshadow.sys" "dist\driver\tgshadow.sys" >nul
  echo   已复制 tgshadow.sys
)
if exist "%DRVSRC%\tgshadow_svc.exe" (
  copy /y "%DRVSRC%\tgshadow_svc.exe" "dist\driver\tgshadow_svc.exe" >nul
  copy /y "%DRVSRC%\tgshadow_ask.exe" "dist\driver\tgshadow_ask.exe" >nul
  echo   已复制 tgshadow_svc.exe / tgshadow_ask.exe
) else (
  echo [WARN] 未找到 %DRVSRC%\tgshadow_svc.exe —— 先运行 driver\build_svc.bat
)
@rem 注意：tgshadow.sys 必须已用受信任证书签名，否则系统会拒绝加载
