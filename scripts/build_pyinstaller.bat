@echo off
rem 用 PyInstaller 打包 4 个 Python 模块为独立 exe
setlocal
cd /d "%~dp0\.."
echo [0/5] 安装构建依赖（pyinstaller pystray pillow）...
python -m pip install pyinstaller pystray pillow
if errorlevel 1 ( echo [错误] pip 安装失败，请检查网络 & exit /b 1 )
echo [1/5] core.exe（主控制 + 托盘）
python -m PyInstaller --noconfirm --clean --onefile --windowed --name core --paths src src\core\controller.py
if errorlevel 1 exit /b 1
echo [2/5] guardian.exe（守望进程，安装时会复制成随机名副本）
python -m PyInstaller --noconfirm --clean --onefile --windowed --name guardian --paths src src\guard\watchdog.py
if errorlevel 1 exit /b 1
echo [3/5] lockscreen.exe（锁定屏幕）
python -m PyInstaller --noconfirm --clean --onefile --windowed --name lockscreen --paths src src\lock\lockscreen.py
if errorlevel 1 exit /b 1
echo [4/5] admin.exe（家长管理）
python -m PyInstaller --noconfirm --clean --onefile --windowed --name admin --paths src src\gui\admin.py
if errorlevel 1 exit /b 1
echo [5/5] 复制默认配置
if not exist dist\config mkdir dist\config
copy /y config\policy.json dist\config\ >nul
echo 完成: dist\core.exe guardian.exe lockscreen.exe admin.exe
