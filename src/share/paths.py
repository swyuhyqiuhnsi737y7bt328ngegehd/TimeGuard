"""路径与运行模式（源码运行 / PyInstaller 打包后）。"""
import os
import sys


def is_frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def app_root() -> str:
    """安装目录：打包后为 exe 所在目录；源码运行时为项目根目录。"""
    if is_frozen():
        return os.path.dirname(os.path.abspath(sys.executable))
    # __file__ = <根>/src/share/paths.py，向上 3 层到项目根
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def src_dir() -> str:
    return os.path.join(app_root(), "src")


def config_dir() -> str:
    d = os.path.join(app_root(), "config")
    os.makedirs(d, exist_ok=True)
    return d


def state_dir() -> str:
    d = os.path.join(app_root(), "state")
    os.makedirs(d, exist_ok=True)
    return d


def logs_dir() -> str:
    d = os.path.join(state_dir(), "logs")
    os.makedirs(d, exist_ok=True)
    return d


def policy_path() -> str:
    return os.path.join(config_dir(), "policy.json")


def usage_path() -> str:
    return os.path.join(state_dir(), "usage.json")


def lock_flag_path() -> str:
    return os.path.join(state_dir(), "lock.flag")


def quit_flag_path() -> str:
    return os.path.join(state_dir(), "quit.flag")


def service_pid_path(name: str) -> str:
    return os.path.join(state_dir(), name + ".pid")


def guardian_entry_path(token: str) -> str:
    return os.path.join(state_dir(), f"guard_{token}.json")


def service_module(name: str) -> str:
    return {"core": "core.controller", "lockscreen": "lock.lockscreen",
            "guardian": "guard.watchdog", "admin": "gui.admin"}[name]


def service_exe(name: str) -> str:
    return os.path.join(app_root(), name + ".exe")
