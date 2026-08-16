"""极简日志：写 state/logs/app.log，源码模式同时打印到控制台。"""
import os
import threading
import time

from . import paths

_LOCK = threading.Lock()


def log(level: str, msg: str):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} [{level}] {msg}"
    try:
        with _LOCK:
            p = os.path.join(paths.logs_dir(), "app.log")
            if os.path.exists(p) and os.path.getsize(p) > 2 * 1024 * 1024:
                try:
                    os.replace(p, p + ".1")
                except OSError:
                    pass
            with open(p, "a", encoding="utf-8") as f:
                f.write(line + "\n")
    except Exception:
        pass
    if not paths.is_frozen():
        try:
            print(line)
        except Exception:
            pass


def info(msg: str):
    log("INFO", msg)


def warn(msg: str):
    log("WARN", msg)


def error(msg: str):
    log("ERROR", msg)
