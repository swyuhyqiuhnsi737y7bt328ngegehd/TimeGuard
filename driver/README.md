# tgshadow —— TimeGuard 磁盘影子（重启还原）驱动

> 类比"冰点还原 / Shadow Defender"：拦截受保护卷的**扇区级写入**，用写时复制（COW）
> 把改动重定向到外部影子存储；**重启默认丢弃 = 还原**；家长确认"保留"时才提交回真实卷。

> ⚠️ **动手前必读**：[EMERGENCY.md](EMERGENCY.md) —— 三条铁律：
> ① 只在虚拟机里加载驱动 ② **动手前先打 VM 快照** ③ PE 里改注册表 hive 后必须 `reg unload`。

## 当前进度：P4 主体完成（磁盘影子 + 提交/丢弃 + 重启还原，VM 实测通过）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 环境（WDK / 测试 VM / 测试签名 / 双机调试） | ✅ 完成（WDK 10.0.26100 + Win10 VM） |
| **P1** | **控制设备 + IOCTL + 卷过滤挂载（直通 + 计数，不改数据）** | **✅ VM 验证通过** |
| **P2** | **拦截写 I/O，用位图记录被改块（仍不重定向数据）** | **✅ VM 验证通过**（读 36667 / 写 2027 次 I/O 被拦截；写 10MB → 重定向块 +85） |
| **P3** | **COW 重定向：读改写全走影子 + 提交/丢弃** | **✅ VM 验证通过**（见下方 P3 验收清单：4MB 文件写后读回校验一致、重启后文件消失、保护前文件完好） |
| **P4** | **磁盘影子后端 + 提交(保留)/丢弃(还原) + 重启还原语义** | **✅ VM 实测通过**（见下方 P4 验收清单） |
| P4b | 用户态服务：关机确认弹窗（保留需密码）+ 开机兜底 | 🔄 代码已写，待联调 |
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
       powershell -Command "$s='CN=TimeGuard Debug'; $c=Get-ChildItem Cert:\CurrentUser\My ^| ?{$_.Subject -eq $s} ^| select -First 1; Set-AuthenticodeSignature -FilePath C:\tg\tgshadow.sys -Certificate $c"
       sc.exe create tgshadow type= kernel start= demand error= normal binPath= C:\tg\tgshadow.sys
       sc.exe start tgshadow
   - 成功标志：`sc.exe query tgshadow` 显示 `RUNNING`
4. 查看内核日志：**DbgView**（管理员，勾选 Capture Kernel）或 WinDbg，过滤 `[TgShadow]`
5. **卸载**：`sc.exe stop tgshadow` + `sc.exe delete tgshadow`（或 `unload_test.bat`）

### P4 验收清单（磁盘影子 —— 已实测通过）

架构：影子放在**另一块卷（S:）上的 4GB 预分配文件**，而不是内存。原因见下方"两次把虚拟机搞坏"。

| 指标 | 启用后 | 写 4MB 文件后 |
|---|---|---|
| 受保护卷 | 3（自动识别系统卷） | 3 |
| 已重定向块数 | 0 | **146** |
| 写重定向成功 | 0 | **100** |
| COW 失败透传 | 0 | **0** |
| 影子已用 | 0 | 9,568,256 字节（≈4MB 文件 + 元数据） |

验收步骤与结果：

1. `tgshadowctl enabledisk 0 3666 S:\tgshadow.bin` → exit=0，保护启用。
2. 写入 4MB 已知模式文件 `C:\rt.bin` → 读回逐字节校验 `verify=True`。
3. **保持保护开启**直接重启（这一点很关键，见下）。
4. 重启后：`RESULT: rt.bin GONE - RESTORE OK`；同时写入的 `f10.bin` 也已消失。
5. 系统正常启动、无蓝屏；驱动 `RUNNING`；保护状态回到"未启用"（影子随本次会话结束而作废）。
6. 佐证：写测试脚本放在 S: 盘，重启后**依然存在** —— 还原只作用于受保护卷。

**重要语义（踩过坑）**：还原路径下**绝不能在关机前 disable 保护**。一旦 disable，
文件系统的关机刷盘会直接落到真实卷，想"还原"的改动反而被持久化了。
正确做法是让保护一直开着到系统关闭：关机刷盘同样被重定向进影子，随重启一起作废。

### ⚠️ 两次把虚拟机搞坏（必须记住）

**事故 1：开机瞬间就启用保护。** 服务在启动早期对系统卷挂上写时复制，且一次性分配
512MB 非分页内存 —— 系统连续启动失败，被 Windows 判定"未正确启动"进入自动修复，
虚拟机彻底起不来。**对策**：延迟启用（默认开机 120 秒后且需有用户登录）、
影子内存不超过物理内存 25%、连续 3 轮没跑满 180 秒就自锁停止自动启用、
安全模式跳过。

**事故 2：内存影子用满后"部分透传"。** 128MB 内存影子很快被系统写入吃满，
COW 失败后按"保守策略"退回透传 —— 结果一部分写入进了影子、一部分落到真实卷，
元数据半新半旧；此时崩溃/硬重启会让卷不一致，又进自动修复。
**对策**：改用磁盘影子（容量足够，4GB），并且**影子卷必须被排除在 COW 之外**
（否则写影子会再次触发 COW，无限递归）。

**事故 3（小）：快照把新盘设成 independent-persistent。** 该模式的磁盘不参与快照，
导致 `revertToSnapshot` 报"恢复虚拟磁盘出错"卡住。新增磁盘不要设这个模式。

### P3 验收清单（重启还原语义 —— 已实测通过）

实测数据（tgshadow 0.1.0-p3.1，内存影子 256MB，受保护卷 = 卷 3）：

| 指标 | 保护前 | 写 4MB 文件后 |
|---|---|---|
| 影子已用 | 655,360 字节 | 4,915,200 字节（+4.06MB = 64 块，正好是文件大小） |
| 已重定向块数 | 10 | 75 |
| 写重定向成功 | 461 | 523 |
| 读影子命中 | 1 | 1 |
| COW 失败透传 | 0 | 0 |
| 高 IRQL 写 I/O | 0 | 0 |

验收步骤与结果：

1. `tgshadowctl enable 3 256` → 保护开启。
2. 写入 4MB 已知模式文件 `C:\p3fix.bin`（字节 i % 251）→ **成功**。
3. 立即读回逐字节校验 → `verify=True`（数据来自影子，不是真实卷）。
4. 重启（`shutdown /r`）→ `C:\p3fix.bin` **不复存在**（`RESULT: p3fix.bin GONE - RESTORE OK`）。
5. 保护开启**之前**写入的 60+ 个既有文件全部完好 → 还原不会误删旧数据。
6. 保护期间写入的其他脚本（`rb1.bat`/`rchk.bat`）重启后同样消失 → 还原对任意路径生效。
7. 全过程无蓝屏，驱动重启后 `STATE: 4 RUNNING`。

**结论：写时复制 + 重启还原的核心语义在真实内核里成立。**

### P3 踩坑记录（关键，两个都是"静默失效"）

**坑 1：COW 恒不触发 —— 存储后端判据写错**

`TgShadowCowWrite` / `TgShadowCowRead` 的入口守卫写的是：

```c
if (g_BlockMap == NULL || g_ShadowFile == NULL) return STATUS_INVALID_DEVICE_STATE;
```

P3 用的是**内存影子**（`g_ShadowMemory`），`g_ShadowFile` 恒为 NULL → 每次 COW 都立刻返回错误，
上层再"保守地落回透传"。症状极具迷惑性：`enable` 成功、`进入 COW 判定` 计数正常增长、
**但 `已重定向块数` 恒为 0，且没有任何报错**（因为失败被设计成静默降级）。

正确判据是"存储后端至少有一个可用"：

```c
if (g_BlockMap == NULL || (g_ShadowMemory == NULL && g_ShadowFile == NULL))
```

**教训**：任何"失败即降级"的设计都必须配一个失败计数器，否则故障是完全不可见的。
本次为此新增 `写重定向成功` / `读影子命中` / `COW 失败透传` / `最后失败状态` 四个诊断字段。

**坑 2：.bat 里的 `%` 会吞掉后面的数字**

用 `powershell -Command "..."` 内联写测试数据时：

```bat
... [byte]($i % 251) ...
```

cmd 会把 `%2` 当成"第二个参数"，表达式被破坏 → PowerShell 语法错误 → 命令**静默失败**，
日志里连一行都没有（现象：测试日志缺行、影子块几乎没涨）。

**约定**：测试脚本一律写成独立 `.ps1` 文件、用 `-File` 调用，
不要在内联 `-Command` 里写 `%`。若必须内联，`%` 要写成 `%%`。

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
| VM / 自用调试 | `sign_test.bat`：`testsigning on` + 自签名证书 | 免费 |
| **分发给真实用户（本项目采用）** | **自签名证书 + 用户手动关掉驱动签名强制**（见下） | 免费，但要用户动手 |
| 想让用户零折腾 | EV 代码签名证书 + Microsoft Partner Center attestation 签名 | 约 ¥2000-4000/年（**本项目没有买**） |

内核驱动没有正式签名，正常 Windows 就会拒绝加载 —— 这是硬性门槛。
本项目**不买证书**，因此走免费路线：程序自动自签名，用户手动关掉驱动签名强制。

### 免费路线的完整步骤（管理界面已自动化，这里是手工等价操作）

1. 生成自签名代码签名证书并信任它：

       certutil -user -f -createSelfSignedCertificate "CN=TimeGuard Debug" \
                -sz 2048 -e 1.3.6.1.5.5.7.3.3 -exportCert tg.cer
       certutil -user -addstore -f Root tg.cer
       certutil -user -addstore -f TrustedPublisher tg.cer
       certutil -addstore -f Root tg.cer            :: 本机存储也要，内核签名检查走本机

2. 给驱动签名（`signtool` 随 Windows SDK 安装；没有就用 PowerShell 的 `Set-AuthenticodeSignature`）：

       signtool sign /fd sha256 /sha1 <证书指纹> /pa /s My tgshadow.sys

3. **关掉驱动签名强制**（三条都可能需要）：

       bcdedit /set testsigning on        :: 需管理员；提示受 Secure Boot 保护就先关 Secure Boot

   - **Secure Boot**：开着的话 `bcdedit` 会被拒绝 → 重启进 BIOS/UEFI 关掉；
   - **内存完整性（HVCI）**：开着的话**测试模式也救不了**自签名驱动 →
     Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 → 关；
   - 重启。桌面右下角出现「测试模式」水印是正常的。

> 证书主题名统一为 `CN=TimeGuard Debug`（`sign.bat` / `sign_test.bat` / `vm_test.bat` / `shadow.py` 一致）。
> 签名验证的是"证书在受信任存储里 + 文件没被改过"，名字本身不重要，但要统一免得重复建证书。

> 部署失败时管理界面会**明确告诉你卡在哪一步**，并且**不会**写入卷类 UpperFilters ——
> 因为一个加载不了的 UpperFilter 指向卷设备可能影响卷的启动路径，代价远高于"功能装不上"。

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
