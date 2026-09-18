# tgshadow —— TimeGuard 磁盘影子（重启还原）驱动

> 类比"冰点还原 / Shadow Defender"：拦截受保护卷的**扇区级写入**，用写时复制（COW）
> 把改动重定向到外部影子存储；**重启默认丢弃 = 还原**；家长确认"保留"时才提交回真实卷。

## 当前进度：P1（骨架）

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 环境（WDK / 测试 VM / 测试签名 / 双机调试） | 🔄 进行中 |
| **P1** | **控制设备 + IOCTL + 卷过滤挂载（直通 + 计数，不改数据）** | **✅ 代码完成，待 VM 验证** |
| P2 | 拦截写 I/O，记录影子位图（仍不重定向） | ⏳ |
| P3 | COW 重定向：读改写全走影子 + 提交/丢弃 | ⏳ |
| P4 | 用户态服务：关机确认弹窗（保留需密码）+ 开机兜底 | ⏳ |
| P5 | 可靠性：崩溃一致性、休眠/快速启动、与 360/BitLocker 共存 | ⏳ |
| P6 | EV 签名 + 安装器集成 | ⏳ |

## 目录结构

    driver/
    ├── tgshadow/
    │   ├── tgshadow.h       用户态/内核共享接口（IOCTL、结构体）
    │   └── tgshadow.c       P1 驱动主体（WDM 卷过滤）
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

## 构建

    driver\build_driver.bat Debug      # 产物: driver\build\Debug\tgshadow.sys

## VM 内测试（依次执行）

1. **开启测试签名**（管理员 CMD，之后重启 VM）：
       bcdedit /set testsigning on
2. 把 `build\Debug\tgshadow.sys` 复制进 VM（例如 C:\tg\tgshadow.sys，连同 `sign_test.bat` 同目录结构）
3. **签名**（管理员）：`driver\sign_test.bat`
4. **加载**（管理员）：`driver\load_test.bat`
   - 成功标志：`sc query tgshadow` 显示 `RUNNING`
5. 查看内核日志：**DbgView**（管理员，勾选 Capture Kernel）或 WinDbg，过滤 `[TgShadow]`
6. **卸载**：`driver\unload_test.bat`

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

## ⚠️ 风险与铁律

1. **只在 VM 里开发调试**。扇区级驱动的 bug 会毁真实数据；物理机测试前必须先做 VM 快照。
2. **影子存储不能放在被保护的卷上**（P3 起强制校验），否则递归重定向 = 灾难。
3. **快速启动（Fast Startup）必须关闭**：否则"关机"其实是休眠，影子状态会被冻结，还原语义错乱。
4. 与 BitLocker / 杀软文件过滤驱动共存需要谨慎设计挂载顺序（P5 处理）。
