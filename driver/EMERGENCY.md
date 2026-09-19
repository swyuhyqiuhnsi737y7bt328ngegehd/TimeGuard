# 驱动调试应急手册（EMERGENCY）

> 本文件记录内核驱动调试中**真实踩过的坑**与救援步骤。血泪教训，请先读完再动手。

## 三条铁律

1. **只在虚拟机里加载驱动**。物理机首次调试内核驱动 = 拿数据赌运气。
2. **动手前先打 VM 快照**（VMware: 虚拟机 → 快照 → 拍摄）。出问题 10 秒回滚，
   不要用 PE 去救——救援本身才是最大的风险源。
3. **在 PE 里改注册表 hive 后必须 `reg unload`**。漏掉这一步，hive 会被标记为
   "正在使用"，系统启动时读不了 SYSTEM hive → 蓝屏/无法启动。

## 症状 → 原因对照

| 症状 | 原因 | 处理 |
|---|---|---|
| `Set-AuthenticodeSignature`：文件正由另一进程使用 | **驱动已加载**，.sys 被内核占用 | `sc.exe stop tgshadow` + `sc.exe delete tgshadow`（或重启）后再签名 |
| PowerShell：`找不到接受实际参数 type=` | PowerShell 里 `sc` 是 `Set-Content` 别名 | 一律用 `sc.exe` |
| `sc start` 失败 | 未开测试签名 / 未签名 / DriverEntry 返回错误 | `bcdedit /set testsigning on` + 重启；查 DbgView 的内核日志 |
| 修改 .sys 后无法启动 | PE 里改 hive 未 `reg unload`（最常见）；或误删引导文件 | 见下文"PE 救援" |
| `reg query` 查不到驱动的埋点值 | **没用管理员权限**（内核创建的键普通用户读不到） | 用管理员 CMD/PowerShell 重查；确认服务已 `sc.exe start`（demand 启动，重启后不会自动加载） |

## cmd 与 PowerShell 语法混用的坑（全部实际踩过）

在 `.bat` 里嵌 `powershell -Command "..."` 时两边语法不同，极易互相踩：

| 写法 | 出错场景 | 症状 | 正确做法 |
|---|---|---|---|
| `^|` | 写在 `powershell -Command "..."` **双引号内** | PowerShell 报找不到参数 `^` | 双引号内直接写 `|`（`^` 只在 cmd 引号**外**用于转义） |
| `>nul` | 同上（写在 PowerShell 命令内） | `Out-File : 要求 FileStream 打开一个不是文件的设备` | PowerShell 内用 `| Out-Null`；或把该命令挪到 bat 层执行 |
| `sc` | 在 PowerShell 里执行 | `Set-Content : 找不到接受实际参数 type=`（`sc` 是 Set-Content 别名） | 一律写 `sc.exe` |
| 反斜杠 | 生成 C 源码/bat 的路径 | `error C4129`（路径悄悄少一个反斜杠，如 `L"\.HarddiskVolume"`） | 生成后按字节核对 `5C` 个数，或用 `[char]92` 拼串 |
| LF 换行 | .bat 文件 | `'xxx' is not recognized as an internal or external command`（行被截断） | .bat 必须 **CRLF** |
| UTF-8 中文 | .bat 输出 | 控制台乱码 | bat 输出用英文；用户态程序用 `SetConsoleOutputCP(CP_UTF8)` |

## PE 救援：移除驱动服务并恢复启动

前提：能用 U 盘/ISO 启动 PE，系统盘识别为 `C:`（按实际情况调整）。

    :: 1) 挂载 SYSTEM hive（用 reg.exe，不要用图形化注册表编辑器）
    reg load HKLM\TGSYS C:\Windows\System32\config\SYSTEM

    :: 2) 查并删除驱动服务（两个 ControlSet 都处理）
    reg query  HKLM\TGSYS\ControlSet001\Services\tgshadow
    reg delete HKLM\TGSYS\ControlSet001\Services\tgshadow /f
    reg query  HKLM\TGSYS\ControlSet002\Services\tgshadow
    reg delete HKLM\TGSYS\ControlSet002\Services\tgshadow /f

    :: 3) ★关键★ 卸载 hive —— 漏掉这步会导致起不来
    reg unload HKLM\TGSYS

    :: 4) 删除驱动文件
    del C:\Windows\System32\drivers\tgshadow.sys

    :: 5) 检查磁盘后重启
    chkdsk C: /f
    wpeutil reboot

**若提示 hive 无法 unload**：说明有进程仍占用（例如 PE 里开着注册表编辑器）。
关闭所有编辑器/资源管理器窗口，重试 `reg unload HKLM\TGSYS`；必要时重启 PE 再操作。

**若已无法启动且疑似 hive 损坏**，按代价从低到高尝试：

1. WinRE → 疑难解答 → 启动修复
2. 系统还原点回滚
3. 从 `C:\Windows\System32\config\RegBack\` 恢复 SYSTEM（Win10 1803+ 默认该目录为空）
4. **VM：直接回滚快照**（最快的路，所以铁律 2 最重要）
5. 重装系统（物理机上的最后手段）

## 关于本驱动

`tgshadow` 以 `start= demand`（按需启动）注册，**不在引导链上**：
即使服务注册残留或文件被删，也只会导致"驱动加载失败"，本身不会阻止系统启动。
真正让系统起不来的，几乎总是**救援过程中的 PE 操作**（尤其是 hive 未卸载）。

## 卸载驱动的干净流程（在能正常启动的系统里）

    sc.exe stop tgshadow
    sc.exe delete tgshadow
    del C:\Windows\System32\drivers\tgshadow.sys

若曾开启测试签名，调试结束后建议关闭：

    bcdedit /set testsigning off
    :: 重启后生效
