# TimeGuard 电脑时间控制软件

家长控制 / 学习管理用电脑时间控制软件：按每日配额与禁止时段限制电脑使用，
超时自动锁定屏幕（或结束指定进程 / 注销 / 定时关机），家长凭密码解锁加时。

采用 Python 3.12 + C（Windows API）实现，模块完全分离，支持
PyInstaller / Nuitka 两种方式打包 Python 部分；C 部分用 MinGW-w64 gcc 编译。

## 一、架构与自我保护设计

    ┌──────────────────────────────┐
    │       家长（人）              │
    │  admin.exe 管理界面 / 托盘     │
    └──────────────┬───────────────┘
                   │ policy.json / extra_req
    ┌──────────────▼────────────────────────────────┐
    │ core.exe 主控制：策略执行、用量累计、防改时间、锁定调度 │
    └──────┬────────────────────┬───────────────────┘
           │ 互相拉起            │ 互相拉起
    ┌──────▼─────────┐  ┌───────▼─────────┐  ┌───────▼──────────┐
    │ guardian 副本1  │  │ guardian 副本2  │  │ guardian 副本3   │
    │ x7k2p9q1.exe   │◄─┤ 4f9a1b2c.exe   ├─►│ m3n8vx2z.exe      │
    └──────┬─────────┘  └───────┬─────────┘  └───────┬──────────┘
           │ 监视并拉起          │                    │
    ┌──────▼────────────────────▼────────────────────▼──────────┐
    │ lockscreen.exe 锁定屏幕（全屏置顶，密码解锁 = 加时）        │
    │ fileguard.exe  文件自锁（C 程序，占用 exe/配置句柄防删改）  │
    └───────────────────────────────────────────────────────────┘

### 进程自我保护（笨方法）
1. 互相监视：安装时把 guardian.exe 复制成 3 份随机名副本（如 x7k2p9q1.exe），任务管理器里名字随机、互不相同；
   - 副本数量恒定：登记表（state/guardians.json）丢失时优先回收目录里的旧副本复用，不会越积越多；
     build_all.bat 构建时也会自动清理历史遗留的随机副本；
2. 被结束即互相启动：每个副本每 1 秒检查其它副本、core、lockscreen、fileguard 是否存活，谁死了就由同伴重新拉起；副本文件被删，就现场再造一份随机名新副本补位；
3. 最后防线：C 程序 fileguard 即使其它进程全灭，也会检测 core 死亡并直接拉起它（仅已安装模式）；
4. 拉起去重：4 秒闸门防止多个守望者同时拉起同一进程；同名副本启动时自检去重，避免重复实例。

### 锁屏加固（src/lock/winlock.py，Windows 原生 API）
- 锁定期间隐藏任务栏（含多显示器副任务栏），解锁/退出时自动恢复；
- 鼠标限制在锁屏区域（ClipCursor），并周期性重新施加，防止被移出；
- 锁窗持续置顶压制（SetWindowPos + SetForegroundWindow 轮询），防止被其它程序窗口盖住；
- 低级键盘钩子屏蔽逃生键：Alt+Tab / Alt+F4 / Win 键 / 菜单键 / Ctrl+Esc /
  Ctrl+Shift+Esc（任务管理器）等；Ctrl+Alt+Del 为系统保留的“安全注意序列”，无法屏蔽；
- 未锁定时周期性自愈：进程被强杀后重启时自动恢复任务栏与鼠标，不留残留状态；
- 配置/状态文件读取兼容带 BOM 的 UTF-8（防记事本等编辑器写坏配置）。

### 文件自我保护（占用自己）
- fileguard.exe（C 程序）：以共享模式 FILE_SHARE_READ | FILE_SHARE_WRITE（不含 FILE_SHARE_DELETE）打开安装目录下所有 .exe 与 config/policy.json 并一直持有句柄——存活期间这些文件无法被删除/改名，但仍可正常读写、程序仍可运行。每 5 秒扫描一次，新出现的文件（如新建的随机名副本）也会被锁上；
- Python 版兜底（share/lockfile.py）：core 启动时占用 policy.json / config.key / policy.bak；每个守护副本占用自己的 exe。lockscreen / admin 不占用文件（它们的 exe 本身已被系统映像占用）；
- 运行中的 exe 本身被系统映像占用、天然不可删除，fileguard 补齐了进程被杀后文件依然删不掉这个空档。

> 说明：这不是 rootkit。管理员权限下仍可停止进程（结束任务 / taskkill），因此卸载功能按正常流程提供。Windows Defender 可能误报自保护行为，建议在 Defender 中添加排除目录。

## 二、目录结构（模块分离）

| 模块 | 文件 | 职责 | 打包产物 |
|---|---|---|---|
| 入口/工具 | src/main.py | install/uninstall/dev/status/hashpwd/resetpw | 不打包 |
| 共享层 | src/share/ | 路径、日志、进程枚举、单实例、文件锁(ctypes) | 随各包 |
| 主控 | src/core/controller.py | 策略执行主循环、托盘、拉起保护组件 | core.exe |
| 策略 | src/core/policy.py | 配额/禁止时段/密码模型 | 随包 |
| 计时 | src/core/clock.py | 每日用量、跨天重置、防改时间、加时请求签名校验 | 随包 |
| 完整性 | src/share/configmac.py | 配置 HMAC 签名/验签、密钥与备份的文件+注册表镜像 | 随包 |
| 旧版限制 | src/share/policies.py | 已弃用的注册表组策略方案；生产路径只剩卸载时的 clear_all() | 随包 |
| 执行器 | src/core/enforcer.py | 锁定/杀进程/注销/关机动作 | 随包 |
| 守望 | src/guard/watchdog.py | 进程互守、随机名副本、拉起服务 | guardian.exe（安装时复制成随机名 x3） |
| 锁定 | src/lock/lockscreen.py | 全屏锁定界面、密码解锁加时 | lockscreen.exe |
| 管理 | src/gui/admin.py | 家长设置界面、立即锁定、卸载 | admin.exe |
| 系统限制 | src/share/restrictor.py | 系统功能限制（进程级拦截 + Win+R 钩子，不依赖注册表：禁任务管理器/注册表/CMD+PowerShell+终端/运行/控制面板与设置） | 随包 |
| 文件自锁 | src/protect/fileguard.c | C 程序：目录文件句柄占用 + 拉起 core | fileguard.exe |
| 配置 | config/policy.json | 策略（家长密码为空时限制不生效） | 复制到 dist |
| 运行状态 | state/ | usage.json、lock.flag、守护注册、日志 | 运行时生成 |

## 二点五、预编译版（免 Python，推荐普通用户）

从 [Releases](https://github.com/swyuhyqiuhnsi737y7bt328ngegehd/TimeGuard/releases/latest) 下载
`TimeGuard-v1.0.2-win64.zip`，解压到任意目录（路径避免中文）后：

1. 双击 `core.exe` —— 主控启动，自动写入开机自启动并部署守护副本；
2. 双击 `admin.exe` 设置家长密码（密码为空时限制不生效），并按需勾选系统功能限制；
3. 到时间自动全屏锁定，输入家长密码解锁即加时；卸载用 admin.exe -> 卸载。

> 360/Defender 可能误报进程自我保护行为，请把目录加入信任区（本程序非恶意软件，源码全公开）。

## 三、构建

### 3.1 环境要求
- Python 3.9+（开发机已验证 3.12）
- **MinGW-w64 gcc**（不要用 Cygwin：Cygwin 编出来的 exe 必须额外带 cygwin1.dll，在没装 Cygwin 的机器上跑不起来；MinGW-w64 编出来的是自包含 exe）
- 网络（pip 装 PyInstaller/Nuitka/pystray/pillow）

> 自动下载的便携工具链放在仓库的 `tools/`（已被 .gitignore 忽略，约 700MB 含完整 GCC）。
> 删掉它不影响其它功能，下次构建会重新下载。

C 工具链**不用手动装**：`scripts/build_cpp.bat` 会按顺序查找 gcc，
1) 仓库内 `tools/w64devkit/bin/gcc.exe`（便携版，找不到就自动下载官方 w64devkit，无需安装、不需要管理员）；
2) PATH 上的 gcc（会校验 `-dumpmachine` 含 mingw，Cygwin/MSYS 的会被拒绝并提示）；
3) 常见安装位置（如 `C:\msys64\mingw64\bin`）。

### 3.2 一键构建（推荐）
双击运行 scripts/build_all.bat，依次：
1. MinGW-w64 gcc 编译 dist/fileguard.exe（产物自包含，不需要额外 DLL）；
2. PyInstaller 打包 4 个 exe 到 dist/：core / guardian / lockscreen / admin；
3. 复制默认配置 dist/config/policy.json。

### 3.3 分开构建
- C 部分：scripts/build_cpp.bat（MinGW-w64 gcc -O2 -s -mwindows）
- Python 部分（PyInstaller）：scripts/build_pyinstaller.bat
- Python 部分（Nuitka，备选）：scripts/build_nuitka.bat

## 四、安装与使用

### 4.1 安装
    python src/main.py install

- 复制配置、写 HKCU 开机自启动（TimeGuard -> dist/core.exe）、启动 fileguard + core（core 会自动创建 3 份随机名守望副本）；
- 首次使用：双击 dist/admin.exe，按要求设置家长密码（密码为空时不做任何限制）。

### 4.2 日常
- 托盘（core.exe）：打开家长设置 / 立即锁定 / 退出程序（需密码）；
- 锁定：超配额或禁止时段自动全屏锁定；输入家长密码解锁 = 加时（默认 30 分钟/次，可在设置里改）；
- admin.exe：改配额、禁止时段、执行动作、提醒、加时、卸载。

### 4.3 状态查看
    python src/main.py status

### 4.4 卸载
    python src/main.py uninstall

或 admin.exe -> 卸载并退出（先写退出标记 -> 停 fileguard -> 停全部进程 -> 移除自启动 -> 清理文件）。

### 4.5 忘记家长密码
    python src/main.py resetpw

清空密码后重新用 admin.exe 设置。（需在项目目录运行，操作 dist 配置。）

### 4.6 源码模式调试
    python src/main.py dev

守望副本以 python -m guard.watchdog 形式运行（随机 token 区分实例），适合改代码调试。
注意与打包版的两处差异：dev 不写 installed.flag、不注册开机自启动；
另外如果之前跑过 install/admin（写过 HKCU 的 MacKey），源码模式的 config/policy.json
会因为缺少 mac 签名被判成"外部篡改"而从备份恢复 —— 表现为"改了配置没反应"。
遇到这种情况：要么用 admin 保存一次让它重新签名，要么清掉 HKCU\Software\TimeGuard 下的 MacKey。

## 五、策略配置（config/policy.json）

| 字段 | 说明 |
|---|---|
| parent_password_hash | 家长密码 SHA-256；为空 = 限制不生效 |
| daily_quota | 工作日/周末每日可用分钟数 |
| forbidden_windows | 禁止时段，支持跨午夜（如 22:30-07:30），最多 3 组 |
| enforce_action | lock=锁屏 / kill=结束指定进程 / logoff=注销 / shutdown=定时关机 |
| kill_processes | action=kill 时要结束的进程名列表 |
| remind_minutes | 剩余这么多分钟时托盘提醒 |
| extra_minutes_per_unlock | 密码解锁一次加时分钟数 |
| tamper_penalty_minutes | 检测到系统时间回拨时从额度中扣减的分钟数 |
| check_interval_seconds | 策略检查间隔（默认 5 秒） |
| system_restrictions | 系统功能限制列表（admin 勾选）：禁用任务管理器/注册表编辑器/命令提示符+PowerShell+Windows 终端/运行(Win+R)/控制面板与设置(Win+I)。由 core 的限制执行器实施：进程级拦截（1~2 秒内结束被禁程序）+ Win+R 键盘钩子，不依赖注册表，仅 core 运行期间生效，卸载即消失 |
| restriction_poll_seconds | 限制执行器轮询周期（秒，默认 1）：core 每周期枚举进程并结束被禁程序的实例 |
| shadow_enabled | 磁盘还原（影子保护）开关，默认 false。由 admin 写入；驱动/服务实际读的是 HKLM\SOFTWARE\TimeGuard\Shadow |
| shadow_mb | 影子文件容量（MB，默认 2048）；影子文件必须放在**非受保护卷**上 |

> 配置文件里还有一个 `mac` 字段（完整性签名）与 `version` 字段，由程序自动维护，
> 不要手工编辑 —— 见下一节。

> **锁定期间强制封杀破解入口**：只要处于锁定状态（配额用完/禁止时段/手动锁定），
> 限制执行器会**无条件**结束以下 11 个进程（不看家长是否勾选 system_restrictions），
> 解锁后自动恢复（见 `share/restrictor.py` 的 `LOCKDOWN_TARGETS`）：
>
> `taskmgr.exe`、`cmd.exe`、`powershell.exe`、`pwsh.exe`、`wt.exe`、
> `WindowsTerminal.exe`、`regedit.exe`、`mmc.exe`、`wscript.exe`、`cscript.exe`、`mshta.exe`。
>
> 这是为了堵死"Ctrl+Alt+Del -> 任务管理器 -> 运行新任务 -> taskkill 杀锁屏与守护"
> 这条破解链路。

## 五点五、完整性保护与防篡改（机制说明）

家长控制的真正防线在于"孩子能不能改掉限制"。除了进程/文件自保护，还有四道数据层面的防线：

| 机制 | 位置 | 防的是什么 |
|---|---|---|
| 配置签名 | `share/configmac.py`：`policy.json` 带 `mac` = HMAC-SHA256(密钥, 除 mac 外全部字段) | 直接编辑 policy.json 改配额/密码。验签失败 → 拒绝采用被改的值，从备份恢复 |
| 密钥与备份镜像 | `state/config.key` + `HKCU\Software\TimeGuard\MacKey`；`state/policy.bak` + `HKCU\...\PolicyBackup` | 删掉密钥文件或备份文件。文件与注册表互为镜像，缺一个还能从另一个补齐 |
| 加时请求签名 | `core/clock.py`：`state/extra_req.json` 必须带 HMAC | **提权通道**：core 会把里面的 minutes 直接加到当日额度。无签名/篡改/错密钥/过期/超限一律拒绝 |
| 计时防篡改 | `core/clock.py`：`tick()` 用单调时钟校验时间连续性 | 改系统时间重置额度；`state/usage.json` 另在 `HKCU\...\UsageMirror` 有镜像，删文件/改成 0 会被顶回 |

### 计时与防改时间的具体判据

* 合法跨天必须同时满足：**单调时钟确实走了 ≥300 秒**，且与墙钟增量差 ≤120 秒
  （覆盖正常午夜、睡眠/休眠恢复；NTP 校时这类微调不误报）；
* 其余任何日期跳变（往前调、往回拨、只改墙钟）一律按篡改处理：
  扣减 `tamper_penalty_minutes`、**保留累计用量**、篡改计数累加且跨天不清零；
* 计费间隔也用单调时钟算，改系统时间不影响计费口径。

### 信任边界（必须说清楚，不要误以为这是密码学级防护）

以上机制防的是"直接编辑配置文件/删文件/改系统时间"这类**普通绕过**。
密钥文件与注册表镜像都在同一用户权限下，孩子若能同时改写密钥与备份，仍然可以绕过 ——
这与本项目"笨方法"的定位一致。要真正挡住，需要把密钥放到孩子无法写入的位置
（例如管理员账户专属目录 / 独立服务）。
## 六、测试

    python tests/smoke_test.py          # 逻辑冒烟（配额/禁止时段/密码/随机名）
    python tests/test_clock.py          # 计时与防改时间（改系统时间不清零、删计时文件不重置）
    python tests/test_extra_req.py      # 加时请求签名校验（无签名/篡改/错密钥/过期/越界全部拒绝）
    python tests/test_policy_merge.py   # 策略合并：DEFAULTS 之外的键不许丢
    python tests/test_configmac.py      # 配置完整性：签名、篡改恢复、备份兜底
    python tests/test_policies.py       # 旧版注册表限制的 apply/clear 语义（默认走测试键，加 --real 才动真实键）
    python tests/test_restrictor.py     # 限制执行器：进程拦截链路 + Win+R 钩子安装/卸载
    python tests/test_shadow.py         # 磁盘影子：结构体布局/IOCTL 码/影子文件预分配（无驱动也能跑）
    python tests/test_filelock.py       # 文件锁：删除/改名被拒、可写入、释放后可删
    python tests/test_winlock.py        # 锁屏加固：任务栏隐藏/恢复、鼠标限制、键盘钩子装卸
    python tests/integration_test.py    # 守望互拉：杀一个守望者，自动补位

以上均为**脚本式**测试（直接 python 运行，不需要 pytest），每项通过时打印 PASS。
已在 Windows 11 + Python 3.12 验证通过；fileguard.exe（MinGW-w64 gcc 编译，自包含）实测锁定行为正确。

## 六点五、磁盘还原（影子保护，可选功能）

给"想让孩子随便折腾、重启就恢复原样"的场景：内核驱动 tgshadow 作为**卷过滤驱动**
挂在系统卷之上，把所有写入用写时复制重定向到**另一块盘上的影子文件**，
关机时弹窗询问是否保留；不确认（或超时、没有家长密码）就一律还原。

### 组成

| 文件 | 作用 |
|---|---|
| `tgshadow.sys` | 内核驱动：卷过滤 + 64KB 块级写时复制 + 提交/丢弃 |
| `tgshadow_svc.exe` | 后台服务：开机延迟启用、关机前弹窗、提交或丢弃 |
| `tgshadow_ask.exe` | 关机确认窗口（倒计时 + 家长密码，跨会话显示在用户桌面） |
| `src/share/shadow.py` | 主程序侧控制模块（DeviceIoControl + 配置 + 部署） |

### 前置条件：手动关掉驱动签名强制（没有代码签名证书的免费路线）

**磁盘还原是可选功能，默认不启用；不做这一步，其余所有功能都不受影响。**

> **这一节是本功能唯一"麻烦"的地方，其余功能都不需要。** 不看这节直接装驱动，
> 重启后驱动会被系统拒绝加载，功能静默失效。

本项目的作者**没有购买代码签名证书**（EV 证书约 ¥2000-4000/年），因此 `tgshadow.sys`
用的是**自签名证书**。Windows 默认不加载这种驱动，必须手动关掉驱动签名强制：

1. **开启测试签名模式**（管理员 CMD，执行后必须重启）：

       bcdedit /set testsigning on

   - 提示 *"受 Secure Boot 策略保护，无法修改或删除"* → 先重启进 BIOS/UEFI **关闭 Secure Boot**，再执行一次；
   - 提示被 BitLocker 保护 → 先在系统里暂停 BitLocker 保护，再执行；
   - 重启后桌面右下角会出现 **「测试模式」水印**，这是正常的。

2. **关闭「内存完整性」（HVCI）**：Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 → 关，然后重启。
   （HVCI 开启时，**连测试模式也救不了**自签名驱动，照样被拒绝加载。）

3. 回到管理界面 → 「磁盘还原」→ **安装驱动与服务**（需要管理员权限）。
   程序会自动：用自签名证书给驱动签名 → 把证书导入受信任存储（用户 + 本机）→
   检查测试签名模式（没开就帮你开）→ 注册卷过滤驱动与服务。
   若还有拦路条件，界面会**直接告诉你卡在哪一步**，并且不会写入卷过滤注册表（可放心重试）。

**代价（请自行权衡）**：测试模式会让这台机器"不再强制驱动签名"，Secure Boot 与内存完整性
需要保持关闭 —— 系统整体的驱动安全防线降低了。所以**磁盘还原是可选功能、默认关闭**：
不打开它就完全用不到驱动，其余所有功能（配额/锁屏/时段/系统限制）都不受影响。

### 怎么用

0. 先完成上面的「前置条件」（只做一次）；
1. 管理界面 → 「磁盘还原」区域 → **安装驱动与服务**（需要管理员权限，装完重启生效）；
2. 勾选"启用影子保护"、设置影子容量（默认 2048MB，需要另一块盘有足够空间），保存；
3. 之后每次开机 120 秒后自动进入保护；关机时弹出确认窗口：
   - 想要保留改动 → 输入家长密码（与主程序家长密码相同）；
   - 什么都不做 / 超时 → 自动还原，重启后回到本次开机前的状态。

### 设计要点与安全阀（血的教训）

* 影子必须放在**非受保护卷**上：驱动会识别影子卷并把它排除在写时复制之外，
  否则"写影子→再触发写时复制"会无限递归；放在同一卷会被驱动直接拒绝。
* **开机延迟 120 秒再启用**：开机瞬间 I/O 最密集，此时挂写时复制曾导致系统
  连续启动失败并进入"自动修复"。
* **连续失败自锁**：连续 3 轮"启用后没跑满 180 秒就重启"会停止自动启用，
  必须人工排查（删除 HKLM\SOFTWARE\TimeGuard\Shadow 下的 BootAttempts 解锁）。
* 安全模式下绝不启用（`GetSystemMetrics(SM_CLEANBOOT)`）；影子容量由用户设定、真实预分配，容量用满后不再保护（不会半透传）。
* **还原路径下绝不能在关机前停用保护**：一旦停用，文件系统的关机刷盘会直接落到
  真实卷，本该还原的改动反而被持久化。

### 当前限制

* **驱动必须是正式签名的才免折腾**：本项目没有买代码签名证书（EV 证书约 ¥2000-4000/年），
  所以只能用自签名 + 测试签名模式，代价是**必须手动关掉驱动签名强制**（详见上文「前置条件」）：
  开启测试模式（桌面出现水印）、关闭 Secure Boot、关闭内存完整性（HVCI）。
  想免掉这些代价，唯一办法是买 EV 证书并走 Microsoft Partner Center 做 attestation 签名 ——
  在那之前，**磁盘还原只是可选功能，默认关闭，不装驱动完全不影响其它功能**；
* 影子文件是原样预分配的，容量决定本次开机最多能改多少数据（超出后不再保护）；
* 内存映射文件（mmap）的写入不受保护；页面文件/休眠文件的处理见 driver/README.md；
* 与 BitLocker / 第三方磁盘加密的共存尚未验证；
* 不含驱动的构建可直接用 zip 里的 exe，无需任何签名相关操作。

## 七、已知限制
- 系统功能限制由主控进程（core）实施：core 未运行时限制不生效（卸载后即完全消失，
  无注册表残留）；改名复制的被禁程序（如 cmd.exe 复制为 x.exe）无法识别；
  管理员权限的同类进程无法被普通权限结束（孩子无管理员权限，影响有限）；
- 早期的注册表组策略方案已弃用（360 等安全软件会拦截 Policies 键写入、DisableCMD
  挡不住 PowerShell、卸载易误删用户原有策略值），卸载仍会清理旧版本残留；
- 互守是笨方法：**管理员权限下循环 taskkill 全部进程**（core + 3 个随机名守护 +
  fileguard + lockscreen）仍可解除——这是用户态防护的固有边界（需管理员权限、
  多进程循环操作，普通用户难以完成）；守护进程已孤儿化（不在 core 进程树内）、
  巡检 1 秒一次、锁屏期间封杀任务管理器/命令行入口，把破解成本抬到很高但非绝对；
- 锁定界面基于 tkinter 全屏置顶，仅覆盖主显示器；鼠标被限制在主屏内，副屏不可操作；
- Ctrl+Alt+Del 无法被用户态程序屏蔽（系统安全注意序列）；
- 用量按开机在线时间累计，不区分实际敲键/空闲（简单可靠）；
- 系统休眠/睡眠造成的时长缺口不累计；
- 防篡改是"防普通绕过"级别：密钥与备份都在同一用户权限下（见"五点五"的信任边界），
  孩子若能同时改写 `state/config.key` 与注册表镜像仍可绕过；
- 被禁程序改名复制（如 cmd.exe 复制成 x.exe）识别不了；
- 日志轮转在多进程同时写时可能失败（`app.log` 被占用导致改名失败），
  失败是静默的，极端情况下日志会持续增长；
- 卸载时按"安装目录前缀"匹配进程，若旁边存在同前缀目录（如 TimeGuardBackup）
  理论上可能误伤，建议安装目录名保持唯一；
- 建议 NTFS + 给家长账户设密码。

## 八、许可证

本项目采用 **MIT License**：

    Copyright (c) 2026 swyuhyqiuhnsi737y7bt328ngegehd

允许任何人免费获取本软件及文档，不受限制地使用、复制、修改、合并、发布、
分发、再授权和/或销售本软件副本，唯一要求是保留上述版权声明与本许可声明
（完整协议见仓库根目录 [LICENSE](LICENSE)，预编译包内亦随附该文件）。

本软件按“原样”提供，不附带任何明示或暗示的担保，包括但不限于适销性、
特定用途适用性和非侵权担保。作者或版权持有人不对任何索赔、损害或其它
责任负责。
