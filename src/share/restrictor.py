"""系统功能限制（进程级拦截 + Win+R 热键拦截），不依赖注册表组策略。

为什么弃用 HKCU 组策略注册表方案：
- 360 等安全软件拦截对 Policies 键的写入，导致限制静默失效；
- DisableCMD=1 不禁 bat、完全挡不住 PowerShell / Windows Terminal；
- NoRun/NoControlPanel 需重启资源管理器（或重新登录）才生效；
- 卸载清理会误删用户原有的同名策略值。

本方案：
- 进程类限制：由 core 周期（默认 1 秒）枚举进程，结束被禁程序的全部实例
  （任务管理器 / regedit / cmd / PowerShell / Windows Terminal / 控制面板 / 设置）；
- Win+R：低级键盘钩子吞掉 Win+R（复用 lock/winlock 的钩子模式），core 按需开关；
- 限制仅在主控进程运行期间生效：无注册表残留、无安全软件冲突、卸载即完全消失。

已知边界（用户态通用局限，防君子不防小人）：
- 孩子把被禁程序改名复制一份（如 cmd.exe -> x.exe）后名称匹配失效；
- 管理员权限的同类进程无法被普通权限结束（孩子没有管理员权限，影响有限）；
- 禁用控制面板仅拦截 control.exe 与设置应用（SystemSettings.exe），
  不保证拦截所有 ms-settings 变体入口。
"""
import threading

from share import logger, util


# 限制项 -> (类型, 目标, 显示名)
#   proc:   目标为可执行文件名列表（大小写不敏感），全部实例都会被结束
#   hotkey: 目标为热键名（当前仅 "win+r"）
RESTRICTIONS = {
    "disable_taskmgr": ("proc", ("taskmgr.exe",), "禁用任务管理器"),
    "disable_regedit": ("proc", ("regedit.exe",), "禁用注册表编辑器(regedit)"),
    "disable_cmd": ("proc", (
        "cmd.exe", "powershell.exe", "pwsh.exe", "powershell_ise.exe",
        "wt.exe", "WindowsTerminal.exe",
    ), "禁用命令提示符/PowerShell/终端(cmd/ps)"),
    "disable_run": ("hotkey", ("win+r",), "禁用运行窗口(Win+R)"),
    "disable_control_panel": ("proc", ("control.exe", "SystemSettings.exe"),
                              "禁用控制面板/设置(Win+I)"),
}


def display_name(key):
    """限制项显示名；未知 key 原样返回（兼容旧配置中的已废弃项）。"""
    info = RESTRICTIONS.get(key)
    return info[2] if info else key


def process_targets(enabled) -> set:
    """enabled 限制项 -> 需要结束的可执行文件名集合（小写）。"""
    out = set()
    for key in (enabled or []):
        kind, targets, _ = RESTRICTIONS.get(key, (None, (), key))
        if kind == "proc":
            out.update(t.lower() for t in targets)
    return out


def hotkey_targets(enabled) -> set:
    """enabled 限制项 -> 需要拦截的热键名集合（小写）。"""
    out = set()
    for key in (enabled or []):
        kind, targets, _ = RESTRICTIONS.get(key, (None, (), key))
        if kind == "hotkey":
            out.update(t.lower() for t in targets)
    return out


def kill_forbidden_once(enabled) -> int:
    """结束所有被禁程序的当前实例；返回尝试结束的进程数。"""
    names = process_targets(enabled)
    if not names:
        return 0
    killed = 0
    for name in names:
        try:
            pids = util.find_pids_by_name(name)
            if pids:
                killed += util.kill_pids(pids)
        except Exception as e:
            logger.error(f"结束被禁进程失败 {name}: {e}")
    return killed


class Restrictor(threading.Thread):
    """系统功能限制执行器：周期结束被禁进程；Win+R 拦截按需开/关。

    get_restrictions 为可调用对象，返回当前启用的限制项列表
    （由主循环在策略重载后更新，避免线程内重复读盘/验签）。
    随 core 退出而终止（daemon 线程），退出时自动卸载热键钩子。
    """

    def __init__(self, get_restrictions, poll_seconds=1.0):
        super().__init__(daemon=True, name="restrictor")
        self._get = get_restrictions
        self._poll = max(0.5, float(poll_seconds or 1.0))
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        last_hotkeys = None
        while not self._stop.wait(self._poll):
            try:
                enabled = self._get() or []
                try:
                    kill_forbidden_once(enabled)
                except Exception as e:
                    logger.error(f"结束被禁进程异常: {e}")
                hot = hotkey_targets(enabled)
                if hot != last_hotkeys:
                    last_hotkeys = hot
                    try:
                        from lock import winlock
                        if "win+r" in hot:
                            winlock.block_run_hotkey(True)
                        else:
                            winlock.block_run_hotkey(False)
                    except Exception as e:
                        logger.error(f"Win+R 拦截开关失败: {e}")
            except Exception as e:
                logger.error(f"限制执行异常: {e}")
        # 退出时卸载热键钩子（不留钩子残留）
        try:
            from lock import winlock
            winlock.block_run_hotkey(False)
        except Exception:
            pass
