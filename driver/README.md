# tgshadow —— TimeGuard 磁盘影子（重启还原）驱动

> 类比"冰点还原 / Shadow Defender"：拦截受保护卷的**扇区级写入**，用写时复制（COW）
> 把改动重定向到外部影子存储；**重启默认丢弃 = 还原**；家长确认"保留"时才提交回真实卷。

> ⚠️ **动手前必读**：[EMERGENCY.md](EMERGENCY.md) —— 三条铁律：
> ① 只在虚拟机里加载驱动 ② **动手前先打 VM 快照** ③ PE 里改注册表 hive 后必须 `reg unload`。

## 当前进度：P2 完成（扇区级过滤已打通）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 环境（WDK / 测试 VM / 测试签名 / 双机调试） | ✅ 完成（WDK 10.0.26100 + Win10 VM） |
| **P1** | **控制设备 + IOCTL + 卷过滤挂载（直通 + 计数，不改数据）** | **✅ VM 验证通过** |
| **P2** | **拦截写 I/O，用位图记录被改块（仍不重定向数据）** | **✅ VM 验证通过**（读 36667 / 写 2027 次 I/O 被拦截；写 10MB → 重定向块 +85） |
| P3 | COW 重定向：读改写全走影子 + 提交/丢弃 | ⏳ |
| P4 | 用户态服务：关机确认弹窗（保留需密码）+ 开机兜底 | ⏳ |
| P5 | 可靠性：崩溃一致性、休眠/快速启动、与 360/BitLocker 共存 | ⏳ |
| P6 | EV 签名 + 安装器集成 | ⏳ |

## 目录结构

    driver/
    ├── tgshadow/
    │   ├── tgshadow.h       用户态/内核共享接口（IOCTL、结构体）
    │   └── tgshadow.c       P1 驱动主体（WDM 卷过滤）
    ├── install_upperfilter.bat  注册为卷 class Upper Filter（需重启生效）
    ├── build_driver.bat     构建（cl/link 直连，兼容 VS BuildTools）
    ├── sign_test.bat        自签名测试证书 + 签名驱动
    ├── load_test.bat        创建并启动内核服务（VM 内）
    ├── unload_test.bat      停止并删除服务
    └── README.md            本文件

## 开发环境

| 组件 | 要求 |
|---|---|
| WDK | 10.0.26100（与已装 SDK 匹配）：`winget install Microsoft.WindowsWDK.10.0.26100` |
| 编译器 | VS2022 BuildTools（MSVC 14.4x，`vcvars64.bat`）——已就位 |
| 测试 VM | Windows 10/11 x64（VMware），**切勿在物理机首次调试内核驱动** |

## 挂载方式（关键架构决策）

驱动以【卷 class 的 Upper Filter】方式挂载（与 volsnap 并列）：

    HKLM\SYSTEM\CurrentControlSet\Control\Class\{71a27cdd-812a-11d0-bec7-08002be2092f}\UpperFilters
      = volsnap \0 tgshadow

**三条硬性要求**（缺一不可，全部实测踩过）：

| 要求 | 原因 | 违反时的症状 |
|---|---|---|
| 驱动放 `%SystemRoot%\System32\drivers\` | Upper Filter 是 BOOT_START 驱动，启动早期只能访问系统盘 | 驱动加载失败 |
| 服务 `start= boot`（0） | 卷设备在 BOOT 阶段就被 partmgr 枚举，`system`(1) 太晚 | `sc query` → STOPPED + `1077 NEVER_STARTED` |
| UpperFilters 必须【追加】 | 默认值 `volsnap` 必须保留 | 卷影/快照功能失效 |

安装：管理员运行 `install_upperfilter.bat`，然后**重启**。

## 踩坑记录（三个致命错误，均有 minidump 佐证）

1. **运行时动态挂载无效**：`IoAttachDevice` 挂到已挂载的卷 → 文件系统挂载时已缓存下层设备指针，
   其 I/O 绕过过滤器（症状：读写计数恒为 0）。
2. **`IoAttachDeviceToDeviceStack` 在运行时使用会崩**：它把设备挂到"整个栈顶"（NTFS 之上），
   于是 `FileObject->DeviceObject` 指向非文件系统设备，`nt!IopWriteFile` 访问其卷字段时
   空指针崩溃 —— 0x3B + 0xC0000005，反汇编证据：
   ```
   mov rax,[rdx+8]      ; FileObject->DeviceObject = 过滤设备（错位）
   mov rcx,[rax+50h]    ; 取到无效字段
   mov r14,[rcx+18h]    ; ACCESS_VIOLATION
   ```
3. **过滤设备必须继承卷字段**：`Vpb` / `AlignmentRequirement` / `SectorSize`，
   否则在上述路径同样崩溃。

**结论**：扇区级过滤器只能在**文件系统挂载之前**（BOOT 阶段）由 PnP 管理器经 UpperFilter 插入，
不能运行时补挂。

## 构建

    driver\build_driver.bat Debug      # 产物: driver\build\Debug\tgshadow.sys

## VM 内测试（依次执行）

> **推荐**：把 `build\Debug\tgshadow.sys`、`tgshadowctl.exe`、`driver\vm_test.bat` 放进 VM 的
> 同一目录（如 `C:\tg\`），以**管理员**运行 `vm_test.bat` —— 它按正确顺序完成
> 停服务 → 签名 → 导入证书 → 加载 → 查询，避免下面两个常见坑。

### ⚠️ 两个必踩的坑（已处理）

1. **顺序必须"先签名、后加载"**：驱动一旦 `sc start` 进内核，`.sys` 文件就被占用，
   再签名会报 *"文件正由另一进程使用"*。改代码重签必须先
   `sc.exe stop tgshadow` + `sc.exe delete tgshadow`（或重启 VM）。
2. **PowerShell 中必须写 `sc.exe`**：PowerShell 里 `sc` 是 `Set-Content` 的别名，
   直接敲 `sc create ...` 会报 *"找不到接受实际参数 type= 的位置形式参数"*。
   在 CMD 中 `sc` 正常；在 PowerShell 中一律用 `sc.exe`。

### 手动步骤

1. **开启测试签名**（管理员 CMD，之后重启 VM）：
       bcdedit /set testsigning on
2. 复制 `tgshadow.sys` / `tgshadowctl.exe` / `vm_test.bat` 到 VM 同一目录（如 `C:\tg\`）
3. **签名 + 加载**（管理员运行 `vm_test.bat`，或手动）：
       sc.exe stop tgshadow
       sc.exe delete tgshadow
       powershell -Command "$s='CN=TimeGuard Test Signing'; $c=Get-ChildItem Cert:\CurrentUser\My ^| ?{$_.Subject -eq $s} ^| select -First 1; Set-AuthenticodeSignature -FilePath C:\tg\tgshadow.sys -Certificate $c"
       sc.exe create tgshadow type= kernel start= demand error= normal binPath= C:\tg\tgshadow.sys
       sc.exe start tgshadow
   - 成功标志：`sc.exe query tgshadow` 显示 `RUNNING`
4. 查看内核日志：**DbgView**（管理员，勾选 Capture Kernel）或 WinDbg，过滤 `[TgShadow]`
5. **卸载**：`sc.exe stop tgshadow` + `sc.exe delete tgshadow`（或 `unload_test.bat`）

### P2 验收清单（写入记录）

驱动加载 + enable 后：

- [ ] status 里 **已重定向块数** 从 0 开始（位图已按卷容量建立）
- [ ] 往受保护卷写数据后，**已重定向块数**与**影子已用字节**同步增长
      （预期：写入量 ÷ 64KB ≈ 增长块数）
- [ ] 反复读写大文件，系统稳定、无蓝屏
- [ ] disable 后位图释放，status 各项归零

验证示例（VM 内管理员，CMD）：

    tgshadowctl.exe enable 3 32768
    tgshadowctl.exe status                      :: 记下"已重定向块数"
    powershell -Command "$b = New-Object byte[] (10485760); [IO.File]::WriteAllBytes('C:\tgtest.bin', $b)"
    tgshadowctl.exe status                      :: 块数应约等于 10MB/64KB = 160 块

### P1 验收清单

- [ ] 驱动加载成功，DbgView 能看到 `[TgShadow] DriverEntry` 与 `control device ready`
- [ ] `sc stop` / `sc start` 反复 20 次无异常
- [ ] VM 重启 20 次，无蓝屏、无启动变慢
- [ ] 未挂载卷时对 `\\.\TgShadow` 做读操作不会蓝屏（防御路径）

## 双机内核调试（推荐，比 DbgView 强大得多）

**VMware 侧**：虚拟机设置 → 添加 → 串口 → 使用命名管道 → 名称 `\\.\pipe\tgkd`，
"该端是服务器"不勾选（VM 为客户端）。

**VM 内**（管理员，之后重启）：

    bcdedit /debug on
    bcdedit /dbgsettings serial debugport:1 baudrate:115200

**宿主 WinDbg**：File → Kernel Debug → COM → Pipe，Pipe name `\\.\pipe\tgkd`，baud 115200

常用命令：

    .sympath srv*C:\Symbols*https://msdl.microsoft.com/download/symbols
    .reload
    lm                      # 模块列表，确认 tgshadow 已加载
    bp tgshadow!DriverEntry # 断在入口
    g                       # 继续
    !analyze -v             # 蓝屏后分析
    ed tgshadow!g_TgShadow.ReadCount 0   # 查看/改全局变量

## ⚠️ 签名说明（重要）

| 场景 | 方案 | 成本 |
|---|---|---|
| VM / 自用调试 | `testsigning on` + 自签名证书（sign_test.bat 自动完成） | 免费 |
| **分发给真实用户** | **EV 代码签名证书** + Microsoft Partner Center（$99）做 attestation 签名 | 约 ¥2000-4000/年 |

没有正式签名的驱动在正常 Windows 上**无法加载**（除非用户手动开测试签名）。
这是硬性门槛，决定本功能能否产品化。

## 📝 编码约定（踩过的坑）

| 位置 | 约定 | 原因 |
|---|---|---|
| 源码文件 | 统一 **UTF-8（无 BOM）**，编译加 `/utf-8` | 中文注释与字符串正确解析 |
| **用户态程序输出** | 启动时调用 **`SetConsoleOutputCP(CP_UTF8)`** | Windows 控制台默认代码页 936(GBK) 会把 UTF-8 字节解释成乱码（如 `椹卞姩鐗堟湰`），这不是"显示问题"而是用户可见的 bug |
| **内核 `DbgPrint`** | **只用 ASCII/英文** | 内核调试输出没有编码协商，中文在 DbgView/WinDbg 里必然乱码 |
| 脚本（.bat） | **CRLF 换行** | LF 换行会让 cmd 解析带括号块/标签的 bat 出错 |

验证乱码是否真被修复：把程序输出重定向到文件，检查前几个字节——
正常中文在 UTF-8 下是 3 字节序列（如 `错误` = `E9 94 99 E8 AF AF`）；
若看到 `E9 94 99` 被显示成 `閿`，说明解码端仍按 GBK 处理。

## ⚠️ 风险与铁律

1. **只在 VM 里开发调试**。扇区级驱动的 bug 会毁真实数据；物理机测试前必须先做 VM 快照。
2. **影子存储不能放在被保护的卷上**（P3 起强制校验），否则递归重定向 = 灾难。
3. **快速启动（Fast Startup）必须关闭**：否则"关机"其实是休眠，影子状态会被冻结，还原语义错乱。
4. 与 BitLocker / 杀软文件过滤驱动共存需要谨慎设计挂载顺序（P5 处理）。
