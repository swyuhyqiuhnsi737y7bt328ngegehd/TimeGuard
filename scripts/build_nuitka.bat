@echo off
rem 用 Nuitka 打包（备选方案；比 PyInstaller 慢，但免杀/体积表现更好）
setlocal
cd /d "%~dp0\.."
echo [0/5] 安装构建依赖（nuitka zstandard pystray pillow）...
python -m pip install nuitka zstandard pystray pillow
if errorlevel 1 ( echo [错误] pip 安装失败，请检查网络 & exit /b 1 )
set "NUI=python -m nuitka --onefile --windows-console-mode=disable --enable-plugin=tk-inter --assume-yes-for-downloads --output-dir=dist --remove-output"
echo [1/5] core.exe
%NUI% --output-filename=core.exe src\core\controller.py || exit /b 1
echo [2/5] guardian.exe
%NUI% --output-filename=guardian.exe src\guard\watchdog.py || exit /b 1
echo [3/5] lockscreen.exe
%NUI% --output-filename=lockscreen.exe src\lock\lockscreen.py || exit /b 1
echo [4/5] admin.exe
%NUI% --output-filename=admin.exe src\gui\admin.py || exit /b 1
echo [5/5] 复制默认配置
if not exist dist\config mkdir dist\config
copy /y config\policy.json dist\config\ >nul
echo 完成: dist 目录已生成 4 个 exe
