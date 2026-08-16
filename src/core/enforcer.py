"""执行限制动作：锁定屏幕 / 结束进程 / 注销 / 定时关机。"""
import os
import subprocess
import time

from guard import watchdog
from share import logger, paths, util


def set_lock(reason: str, until_ts: float):
    util.write_json(paths.lock_flag_path(), {"reason": reason, "until": until_ts,
                                             "ts": time.time()})


def clear_lock():
    try:
        os.remove(paths.lock_flag_path())
    except FileNotFoundError:
        pass
    except OSError:
        pass


def ensure_lockscreen():
    watchdog.ensure_service("lockscreen", "lockscreen.exe", "lock.lockscreen")


def _kill_processes(cfg):
    for name in cfg.get("kill_processes", []) or []:
        name = str(name).strip()
        if not name:
            continue
        try:
            subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True, timeout=15)
        except Exception:
            pass


def enforce(cfg, reason: str, until_ts: float):
    """按策略动作执行限制。until_ts 仅 lock 动作使用。"""
    action = str(cfg.get("enforce_action", "lock"))
    logger.info(f"执行限制动作 [{action}]：{reason}")
    if action == "kill":
        _kill_processes(cfg)
    elif action == "logoff":
        try:
            subprocess.run(["shutdown", "/l"], capture_output=True, timeout=15)
        except Exception:
            pass
    elif action == "shutdown":
        try:
            subprocess.run(["shutdown", "/s", "/t", "60", "/c", "TimeGuard: 时间到，即将关机"],
                           capture_output=True, timeout=15)
        except Exception:
            pass
    else:  # lock（默认）
        set_lock(reason, until_ts)
        ensure_lockscreen()
