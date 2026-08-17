# TimeGuard 电脑时间控制软件

家长控制 / 学习管理用电脑时间控制软件：按每日配额与禁止时段限制电脑使用，
超时自动锁定屏幕（或结束指定进程 / 注销 / 定时关机），家长凭密码解锁加时。

采用 Python 3.12 + C（Windows API）实现，模块完全分离，支持
PyInstaller / Nuitka / Cygwin gcc 三种方式打包。

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
2. 被结束即互相启动：每个副本每 3 秒检查其它副本、core、lockscreen、fileguard 是否存活，谁死了就由同伴重新拉起；副本文件被删，就现场再造一份随机名新副本补位；
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
- Python 版兜底（share/lockfile.py）：每个进程启动时也占用自己的 exe 与策略文件；
- 运行中的 exe 本身被系统映像占用、天然不可删除，fileguard 补齐了进程被杀后文件依然删不掉这个空档。

> 说明：这不是 rootkit。管理员权限下仍可停止进程（结束任务 / taskkill），因此卸载功能按正常流程提供。Windows Defender 可能误报自保护行为，建议在 Defender 中添加排除目录。

## 二、目录结构（模块分离）

| 模块 | 文件 | 职责 | 打包产物 |
|---|---|---|---|
| 入口/工具 | src/main.py | install/uninstall/dev/status/hashpwd/resetpw | 不打包 |
| 共享层 | src/share/ | 路径、日志、进程枚举、单实例、文件锁(ctypes) | 随各包 |
| 主控 | src/core/controller.py | 策略执行主循环、托盘、拉起保护组件 | core.exe |
| 策略 | src/core/policy.py | 配额/禁止时段/密码模型 | 随包 |
| 计时 | src/core/clock.py | 每日用量、跨天重置、防改时间 | 随包 |
| 执行器 | src/core/enforcer.py | 锁定/杀进程/注销/关机动作 | 随包 |
| 守望 | src/guard/watchdog.py | 进程互守、随机名副本、拉起服务 | guardian.exe（安装时复制成随机名 x3） |
| 锁定 | src/lock/lockscreen.py | 全屏锁定界面、密码解锁加时 | lockscreen.exe |
| 管理 | src/gui/admin.py | 家长设置界面、立即锁定、卸载 | admin.exe |
| 系统限制 | src/share/policies.py | 系统功能限制（禁任务管理器/注册表/CMD/运行/控制面板/自动运行） | 随包 |
| 文件自锁 | src/protect/fileguard.c | C 程序：目录文件句柄占用 + 拉起 core | fileguard.exe |
| 配置 | config/policy.json | 策略（家长密码为空时限制不生效） | 复制到 dist |
| 运行状态 | state/ | usage.json、lock.flag、守护注册、日志 | 运行时生成 |

## 三、构建

### 3.1 环境要求
- Python 3.9+（开发机已验证 3.12）
- Cygwin（gcc-core 包即可，不需要 g++/libstdc++，fileguard 是纯 C）；或任意能编 Windows API 的 C 编译器
- 网络（pip 装 PyInstaller/Nuitka/pystray/pillow）

### 3.2 一键构建（推荐）
双击运行 scripts/build_all.bat，依次：
1. Cygwin gcc 编译 dist/fileguard.exe（自动找 gcc 并复制 cygwin1.dll）；
2. PyInstaller 打包 4 个 exe 到 dist/：core / guardian / lockscreen / admin；
3. 复制默认配置 dist/config/policy.json。

### 3.3 分开构建
- C 部分：scripts/build_cpp.bat（gcc -O2 -s -mwindows）
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

与打包版行为一致（守望副本以 python -m guard.watchdog 形式运行，随机 token 区分实例），适合改代码调试。

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
| system_restrictions | 系统功能限制列表（admin 勾选）：禁用任务管理器/注册表编辑器/命令提示符/运行/控制面板/移动存储自动运行，通过 HKCU 组策略实现，仅对当前账户生效，卸载时自动清理 |

## 六、测试

    python tests/smoke_test.py        # 逻辑冒烟（策略/密码/随机名）
    python tests/test_filelock.py     # 文件锁：删除/改名被拒、可写入、释放后可删
    python tests/integration_test.py  # 守望互拉：杀一个守望者，自动补位

已在 Windows + Python 3.12 验证通过：单元冒烟 4 项、文件锁 4 项、集成互拉 4 项全过；fileguard.exe（Cygwin gcc 14 编译）实测锁定行为正确。

## 七、已知限制
- 互守是笨方法：管理员同时结束所有进程 + 删除文件仍可解除（本就不是 rootkit）；
- 锁定界面基于 tkinter 全屏置顶，仅覆盖主显示器；鼠标被限制在主屏内，副屏不可操作；
- Ctrl+Alt+Del 无法被用户态程序屏蔽（系统安全注意序列）；
- 用量按开机在线时间累计，不区分实际敲键/空闲（简单可靠）；
- 系统休眠/睡眠造成的时长缺口不累计；
- 建议 NTFS + 给家长账户设密码。
