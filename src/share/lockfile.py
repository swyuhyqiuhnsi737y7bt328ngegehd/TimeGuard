"""文件自我保护（Python 版）：占用文件句柄，禁止删除/改名，允许读写。

原理：以共享模式 FILE_SHARE_READ | FILE_SHARE_WRITE（不含 FILE_SHARE_DELETE）
打开文件并持有句柄 —— 其它进程仍可打开读写、程序仍可运行，
但任何删除/改名操作都会因共享冲突而失败。句柄存活期间保护一直有效。

C 版 fileguard.exe（src/protect/fileguard.c）是更强的一层：锁定整个目录。
"""
import ctypes
import glob
import os
import sys
from ctypes import wintypes

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x1
FILE_SHARE_WRITE = 0x2
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                             wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                             wintypes.HANDLE]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]


def open_deny_delete(path: str):
    """打开文件但共享模式不含 DELETE：本进程存活期间该文件无法被删除/改名。
    返回句柄；失败返回 None。"""
    h = _k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
    if not h or h == -1 or h == (1 << 64) - 1:
        return None
    return h


class FileLocker:
    """持有多个文件句柄，阻止外部删除/改名。"""

    def __init__(self):
        self._handles = []

    def lock(self, path: str) -> bool:
        if not os.path.isfile(path):
            return False
        h = open_deny_delete(path)
        if h:
            self._handles.append(h)
            return True
        return False

    def lock_self(self):
        """锁住自己正在运行的可执行文件（打包后为 exe，源码模式锁本模块文件示意）。"""
        if getattr(sys, "frozen", False):
            self.lock(sys.executable)
        else:
            self.lock(os.path.abspath(__file__))

    def lock_dir(self, d: str, patterns=("*.exe",)):
        for pat in patterns:
            for p in glob.glob(os.path.join(d, pat)):
                self.lock(p)

    def release(self):
        for h in self._handles:
            try:
                _k32.CloseHandle(h)
            except Exception:
                pass
        self._handles.clear()
