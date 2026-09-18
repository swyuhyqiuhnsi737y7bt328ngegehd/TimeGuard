@echo off
rem unload_test.bat - 停止并删除 tgshadow 服务（需管理员）
sc stop tgshadow
sc delete tgshadow
sc query tgshadow
