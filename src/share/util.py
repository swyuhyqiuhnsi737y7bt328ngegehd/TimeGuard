"""通用工具：哈希/随机名/JSON/进程枚举与存活检查/单实例/启动进程。"""
import ctypes
import hashlib
import json
import os
import random
import string
import subprocess
import sys
import time
from ctypes import wintypes

from . import logger

# ---------------- 基础工具 ----------------

def sha256_hex(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def random_name(n: int = 8) -> str:
    return "".join(random.choices(string.ascii_lowercase + string.digits, k=n))


def read_json(path, default=None):
    # utf-8-sig：兼容带 BOM 的 UTF-8 文件（记事本/PS 旧版写入可能带 BOM）
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            return json.load(f)
    except Exception:
        return default


def write_json(path, data):
    """原子写 JSON（临时文件 + 改名）。注意：被占用锁保护的文件不能用本函数（rename 需要 DELETE 权限）。"""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(str(text))


def read_text(path, default=""):
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            return f.read()
    except Exception:
        return default


# ---------------- Windows 进程 ----------------

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenProcess.restype = wintypes.HANDLE
_k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_k32.QueryFullProcessImageNameW.restype = wintypes.BOOL
_k32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                            wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]


def process_image(pid: int):
    """返回进程镜像完整路径（小写）；进程不存在或无权限返回 None。"""
    try:
        pid = int(pid)
    except Exception:
        return None
    if pid <= 0:
        return None
    h = _k32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if _k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            return buf.value.lower()
        return None
    finally:
        _k32.CloseHandle(h)


def process_alive(pid: int, expected_basename: str = None) -> bool:
    """进程是否存活。expected_basename 用于防 PID 复用（镜像文件名一致才算存活）。"""
    img = process_image(pid)
    if img is None:
        return False
    if expected_basename:
        return os.path.basename(img).lower() == str(expected_basename).lower()
    return True


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(wintypes.ULONG)),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


_k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
_k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
_k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]


def find_pid_by_name(basename: str) -> int:
    """按镜像文件名（如 core.exe）返回第一个匹配的 pid；找不到返回 0。"""
    TH32CS_SNAPPROCESS = 0x2
    target = str(basename).lower()
    snap = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == -1 or snap == (1 << 64) - 1:
        return 0
    try:
        pe = _PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
        if not _k32.Process32FirstW(snap, ctypes.byref(pe)):
            return 0
        while True:
            if pe.szExeFile.lower() == target:
                return int(pe.th32ProcessID)
            if not _k32.Process32NextW(snap, ctypes.byref(pe)):
                return 0
    finally:
        _k32.CloseHandle(snap)


def processes_under(root_dir: str):
    """返回可执行文件位于 root_dir 下的所有 (pid, exe_path)。

    用于卸载/清理：不依赖注册表，能找出目录下任意名字的进程（含随机名守望副本）。
    """
    root = os.path.normcase(os.path.abspath(root_dir))
    out = []
    TH32CS_SNAPPROCESS = 0x2
    snap = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == -1 or snap == (1 << 64) - 1:
        return out
    try:
        pe = _PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
        if not _k32.Process32FirstW(snap, ctypes.byref(pe)):
            return out
        while True:
            img = process_image(int(pe.th32ProcessID))
            if img and img.startswith(root):
                out.append((int(pe.th32ProcessID), img))
            if not _k32.Process32NextW(snap, ctypes.byref(pe)):
                break
    finally:
        _k32.CloseHandle(snap)
    return out


def kill_pids(pids):
    """按 PID 结束进程（含子进程树）。"""
    for pid in pids:
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, timeout=10)
        except Exception:
            pass


def spawn(cmd, env=None, cwd=None):
    """启动进程（隐藏控制台窗口）。失败返回 None。"""
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        return subprocess.Popen(cmd, creationflags=flags, env=env, cwd=cwd, close_fds=True)
    except Exception as e:
        logger.error(f"启动进程失败 {cmd}: {e}")
        return None


def spawn_service(name: str, extra_args=None):
    """启动一个服务进程（core/guardian/lockscreen/admin）。"""
    args = list(extra_args or [])
    if paths_is_frozen():
        exe = paths_service_exe(name)
        if not os.path.exists(exe):
            return None
        return spawn([exe] + args)
    return spawn([sys.executable, "-m", paths_service_module(name)] + args,
                 env={**os.environ, "PYTHONPATH": paths_src_dir()})


# 延迟导入 paths 避免循环依赖（util <- logger <- paths）
def paths_is_frozen():
    from . import paths
    return paths.is_frozen()


def paths_service_exe(name):
    from . import paths
    return paths.service_exe(name)


def paths_service_module(name):
    from . import paths
    return paths.service_module(name)


def paths_src_dir():
    from . import paths
    return paths.src_dir()


# ---------------- 退出标记（带时间戳，防残留导致下次启动即退出） ----------------

QUIT_FLAG_MAX_AGE = 300  # 5 分钟内写入的退出标记才有效


def write_quit_flag():
    """写入退出标记（带时间戳）。"""
    write_text(paths_quit_flag_path(), str(time.time()))


def quit_flag_active() -> bool:
    """退出标记是否存在且新鲜；残留的旧标记不生效（防止卸载后下次启动即退出）。"""
    try:
        v = float(read_text(paths_quit_flag_path(), "0"))
        return (time.time() - v) < QUIT_FLAG_MAX_AGE
    except Exception:
        return False


def paths_quit_flag_path():
    from . import paths
    return paths.quit_flag_path()


def notify_ui(title: str, msg: str):
    """弹出提示框（告知单实例冲突等用户可见事件）。"""
    try:
        import tkinter as tk
        from tkinter import messagebox
        r = tk.Tk()
        r.withdraw()
        messagebox.showinfo(title, msg, parent=r)
        r.destroy()
    except Exception:
        pass


def launched_by_user() -> bool:
    """父进程是否为 explorer.exe（即用户从桌面/资源管理器双击启动）。
    用于区分“用户手动运行”和“守护进程自动拉起”，避免竞态下乱弹提示框。"""
    TH32CS_SNAPPROCESS = 0x2
    try:
        snap = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if not snap or snap == -1 or snap == (1 << 64) - 1:
            return False
        try:
            mypid = os.getpid()
            pe = _PROCESSENTRY32W()
            pe.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
            ppid = 0
            if not _k32.Process32FirstW(snap, ctypes.byref(pe)):
                return False
            while True:
                if int(pe.th32ProcessID) == mypid:
                    ppid = int(pe.th32ParentProcessID)
                    break
                if not _k32.Process32NextW(snap, ctypes.byref(pe)):
                    return False
            if not ppid:
                return False
            if not _k32.Process32FirstW(snap, ctypes.byref(pe)):
                return False
            while True:
                if int(pe.th32ProcessID) == ppid:
                    return pe.szExeFile.lower() == "explorer.exe"
                if not _k32.Process32NextW(snap, ctypes.byref(pe)):
                    return False
        finally:
            _k32.CloseHandle(snap)
    except Exception:
        return False
    return False


# ---------------- 单实例 ----------------

_k32.CreateMutexW.restype = wintypes.HANDLE
_k32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
_k32.GetLastError.restype = wintypes.DWORD


def single_instance(name: str) -> bool:
    """同名互斥体：返回 False 表示已有实例在运行。"""
    h = _k32.CreateMutexW(None, True, r"Local\TimeGuard_" + name)
    if not h:
        return True  # 未知错误，放行
    if _k32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
        return False
    return True
