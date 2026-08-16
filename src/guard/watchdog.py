"""守望进程（guardian.exe）：多个副本互相监视。

设计（用户要求的“笨方法”）：
- 安装时把 guardian.exe 复制成 N 份随机名副本（如 x7k2p9q1.exe），名字随机；
- 每个副本每 3 秒检查其它副本、core、lockscreen、fileguard 是否存活；
- 谁被结束，就由存活的同伴重新拉起；副本文件被删，就现场再造一份随机名新副本；
- 另有 fileguard（C 程序）作为最后防线：检测到 core 死亡时直接拉起它。
"""
import glob
import os
import re
import shutil
import sys
import time

from share import logger, paths, util
from share.lockfile import FileLocker

GUARDIAN_COUNT = 3        # 守望进程副本数（含自己）
WATCH_INTERVAL = 3        # 秒
SPAWN_GATE_SECONDS = 4    # 同一时刻只允许一个守望者发起拉起，避免重复拉起


def _registry_path():
    return os.path.join(paths.state_dir(), "guardians.json")


def _load_registry():
    return util.read_json(_registry_path(), {"copies": [], "seed": "guardian.exe"})


def _save_registry(r):
    util.write_json(_registry_path(), r)


def _spawn_gate() -> bool:
    """拉新进程前先抢“闸门”：4 秒内只放行一个。"""
    p = os.path.join(paths.state_dir(), "spawn_lock")
    try:
        if os.path.exists(p) and time.time() - os.path.getmtime(p) < SPAWN_GATE_SECONDS:
            return False
        util.write_text(p, str(time.time()))
        return True
    except Exception:
        return True


def _py_env():
    return {**os.environ, "PYTHONPATH": paths.src_dir()}


def ensure_fileguard():
    """确保 C 程序 fileguard（文件自锁 + 最后防线）在运行。"""
    exe = os.path.join(paths.app_root(), "fileguard.exe")
    if not paths.is_frozen():
        exe = os.path.join(paths.app_root(), "dist", "fileguard.exe")
    if not os.path.exists(exe):
        return
    if util.find_pid_by_name("fileguard.exe"):
        return
    util.spawn([exe])


def ensure_service(name: str, exe_name: str, module: str):
    """确保一个常驻服务进程（core/lockscreen）在运行，死了就拉起。"""
    if paths.is_frozen():
        exe = os.path.join(paths.app_root(), exe_name)
        if not os.path.exists(exe):
            return
        if util.find_pid_by_name(exe_name):
            return
        util.spawn([exe])
    else:
        try:
            pid = int(util.read_text(paths.service_pid_path(name), "0") or "0")
        except Exception:
            pid = 0
        if pid and util.process_alive(pid, "python.exe"):
            return
        util.spawn([sys.executable, "-m", module], env=_py_env())


def _guardian_cmd(token: str):
    if paths.is_frozen():
        return [sys.executable, "--token", token]
    return [sys.executable, "-m", "guard.watchdog", "--token", token]


def _token_of(exe_path: str) -> str:
    stem = os.path.splitext(os.path.basename(exe_path))[0]
    return stem if len(stem) >= 4 else util.random_name()


_RANDOM_COPY_RE = re.compile(r"^[a-z0-9]{8}\.exe$")  # 随机副本命名模式
_KNOWN_EXES = ("core.exe", "guardian.exe", "lockscreen.exe", "admin.exe", "fileguard.exe")


def _orphan_copies():
    """安装目录里遗留的旧随机副本（8 位随机名 exe，排除固定清单）。

    注册表丢失时优先回收复用它们，而不是新建，避免副本文件越积越多。
    """
    out = []
    try:
        for name in os.listdir(paths.app_root()):
            # guardian.exe 恰好也是 8 个字母，必须显式排除固定清单
            if name not in _KNOWN_EXES and _RANDOM_COPY_RE.match(name):
                p = os.path.join(paths.app_root(), name)
                if os.path.isfile(p):
                    out.append(p)
    except Exception:
        pass
    return out


def ensure_guardians():
    """保证 GUARDIAN_COUNT 个守望副本在运行；缺哪个拉起哪个，缺副本文件就回收/新造随机名副本。"""
    if not _spawn_gate():
        return
    if paths.is_frozen():
        reg = _load_registry()
        copies = [p for p in reg.get("copies", []) if os.path.exists(p)]
        if not copies:
            # 注册表丢失（重建/清理过 state 目录）：回收目录里的旧副本，而不是新建
            copies = _orphan_copies()
            reg["copies"] = copies
        live = 0
        for p in list(copies):
            if not os.path.exists(p):
                continue
            if util.find_pid_by_name(os.path.basename(p)):
                live += 1
                continue
            util.spawn([p])  # 拉起被结束的同伴（乐观计数，下轮校验）
            live += 1
        copies = [p for p in copies if os.path.exists(p)]
        if live < GUARDIAN_COUNT:
            # 用目录里尚未登记的旧副本补位（数量超出 3 的历史遗留）
            spare = [p for p in _orphan_copies() if p not in copies]
            for p in spare:
                if live >= GUARDIAN_COUNT:
                    break
                copies.append(p)
                util.spawn([p])
                live += 1
        if live < GUARDIAN_COUNT:
            seed = os.path.join(paths.app_root(), "guardian.exe")
            if not os.path.exists(seed):
                logger.error("缺少 guardian.exe，无法创建守望副本")
                _save_registry(reg)
                return
            for _ in range(GUARDIAN_COUNT - live):
                newp = os.path.join(paths.app_root(), util.random_name() + ".exe")
                try:
                    shutil.copy2(seed, newp)
                except Exception as e:
                    logger.error(f"创建守望副本失败: {e}")
                    break
                copies.append(newp)
                util.spawn([newp])
        reg["copies"] = copies
        _save_registry(reg)
    else:
        # 源码模式：按注册条目计数
        live = 0
        for f in glob.glob(os.path.join(paths.state_dir(), "guard_*.json")):
            e = util.read_json(f, None)
            if e and util.process_alive(int(e.get("pid", 0)), "python.exe")                     and time.time() - float(e.get("ts", 0)) < 120:
                live += 1
        if live >= GUARDIAN_COUNT:
            return
        tok = util.random_name()
        util.spawn(_guardian_cmd(tok), env=_py_env())


def _cleanup_entries(my_token: str):
    """清理孤儿注册条目，防止 state 目录无限增长。"""
    valid = None
    if paths.is_frozen():
        valid = {_token_of(p) for p in _load_registry().get("copies", [])}
    for f in glob.glob(os.path.join(paths.state_dir(), "guard_*.json")):
        token = os.path.basename(f)[len("guard_"):-len(".json")]
        if token == my_token:
            continue
        if valid is not None and token not in valid:
            try:
                os.remove(f)
            except OSError:
                pass
            continue
        e = util.read_json(f, None)
        if not e:
            try:
                os.remove(f)
            except OSError:
                pass
            continue
        if not util.process_alive(int(e.get("pid", 0)), "python.exe" if not paths.is_frozen() else None)                 and time.time() - float(e.get("ts", 0)) > 60:
            try:
                os.remove(f)
            except OSError:
                pass


def main():
    args = sys.argv[1:]
    token = None
    if "--token" in args:
        idx = args.index("--token")
        if idx + 1 < len(args):
            token = args[idx + 1]
    if not token:
        token = _token_of(sys.executable) if paths.is_frozen() else util.random_name()
    # 去重：同名 token 已有存活实例则退出（防止两个守望者同时拉起同一副本）
    entry = util.read_json(paths.guardian_entry_path(token), None)
    if entry:
        base = os.path.basename(sys.executable) if paths.is_frozen() else "python.exe"
        if int(entry.get("pid", 0)) != os.getpid() and                 util.process_alive(int(entry.get("pid", 0)), base):
            return
    util.write_json(paths.guardian_entry_path(token),
                    {"pid": os.getpid(), "token": token, "ts": time.time(),
                     "cmd": _guardian_cmd(token)})
    locker = FileLocker()
    locker.lock_self()  # 占用自己的 exe 文件，阻止被删除/改名
    logger.info(f"guardian[{token}] 启动 pid={os.getpid()}")
    while True:
        if util.quit_flag_active():
            logger.info(f"guardian[{token}] 收到退出指令")
            break
        try:
            ensure_guardians()
            ensure_fileguard()
            ensure_service("core", "core.exe", "core.controller")
            ensure_service("lockscreen", "lockscreen.exe", "lock.lockscreen")
            _cleanup_entries(token)
            # 心跳：刷新自己的注册条目
            util.write_json(paths.guardian_entry_path(token),
                            {"pid": os.getpid(), "token": token, "ts": time.time(),
                             "cmd": _guardian_cmd(token)})
        except Exception as e:
            logger.error(f"guardian[{token}] 循环异常: {e}")
        time.sleep(WATCH_INTERVAL)
    locker.release()
    logger.info(f"guardian[{token}] 退出")


if __name__ == "__main__":
    main()
