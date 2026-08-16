@echo off
rem 一键构建：C 自锁程序 + Python 模块
call "%~dp0build_cpp.bat" || exit /b 1
call "%~dp0build_pyinstaller.bat" || exit /b 1
echo.
echo 全部构建完成，产物在 dist\，接下来运行: python src\main.py install
