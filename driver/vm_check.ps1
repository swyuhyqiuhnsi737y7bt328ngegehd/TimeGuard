# vm_check.ps1 - tgshadow 环境自检（在【加载了驱动的 VM】里以管理员运行）
# 一次性输出：驱动文件指纹 / 服务状态 / 注册表埋点 / 设备可打开性

$ErrorActionPreference = "Continue"
$sys = "C:\tg\tgshadow.sys"
$ctl = "C:\tg\tgshadowctl.exe"

Write-Host "===== 1) driver file =====" -ForegroundColor Cyan
if (Test-Path $sys) {
    Get-Item $sys | Select-Object FullName, Length, LastWriteTime | Format-List
    "SHA256 = " + (Get-FileHash $sys -Algorithm SHA256).Hash
} else {
    Write-Host "NOT FOUND: $sys" -ForegroundColor Red
}

Write-Host "===== 2) service config & state =====" -ForegroundColor Cyan
sc.exe qc tgshadow
sc.exe query tgshadow

Write-Host "===== 3) registry trace (crash locator) =====" -ForegroundColor Cyan
reg query "HKLM\SOFTWARE\TimeGuard" /v TgShadowLastStep 2>&1
Write-Host "(if not found: either wrong machine, or driver not loaded, or build has no tracing)"

Write-Host "===== 4) can we open the device? =====" -ForegroundColor Cyan
if (Test-Path $ctl) {
    & $ctl version
} else {
    Write-Host "NOT FOUND: $ctl" -ForegroundColor Red
}

Write-Host "===== 5) current protection status =====" -ForegroundColor Cyan
if (Test-Path $ctl) { & $ctl status }

Write-Host "===== 6) recent bugchecks =====" -ForegroundColor Cyan
Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001} -MaxEvents 2 -ErrorAction SilentlyContinue |
    Format-List TimeCreated, Message
