r"""磁盘影子（重启还原）—— 主程序侧控制模块。

设计要点
--------
* 直接通过 DeviceIoControl 与内核驱动 tgshadow 通信，不依赖额外的命令行工具；
* 影子是**另一块卷上的预分配文件**（绝不能放在被保护的卷上，否则写影子会
  再次触发写时复制造成无限递归——驱动侧会自动识别并拒绝这种情况）；
* 开关、容量、影子路径持久化在 HKLM\SOFTWARE\TimeGuard\Shadow，
  驱动与服务（tgshadow_svc.exe）都读这里，所以主程序只要改配置即可；
* 关机确认弹窗（tgshadow_ask.exe）用 SHA-256 校验家长密码，哈希算法与
  share.util.sha256_hex 完全一致（UTF-8 字节 + 小写十六进制），
  因此主程序里的家长密码可以直接用于"保留本次修改"。

安全约定（踩过坑，务必保持）
--------------------------
* 还原路径下**绝不能在关机前停用保护**：一旦停用，文件系统的关机刷盘会直接
  落到真实卷，本该被还原的改动反而被持久化。停用只用于家长明确要求"现在就关掉"。
* 影子容量必须真实预分配；容量不足时宁可不启用，也不要半保护。
"""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
from ctypes import wintypes

from share import paths, util

# ----------------------------------------------------------------- 常量

DEVICE_PATH = r"\\\\.\\TgShadow"
CFG_KEY = r"SOFTWARE\TimeGuard\Shadow"

TGSHADOW_BLOCK_SIZE = 64 * 1024
FLAG_ATTACH_ONLY = 0x0001

_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_OPEN_EXISTING = 3
_INVALID_HANDLE = ctypes.c_void_p(-1).value


def _ctl_code(dev_type: int, func: int, method: int = 0, access: int = 0) -> int:
    return (dev_type << 16) | (access << 14) | (func << 2) | method


_DEV_TYPE = 0x8331
IOCTL_GET_VERSION = _ctl_code(_DEV_TYPE, 0x800)
IOCTL_GET_STATUS = _ctl_code(_DEV_TYPE, 0x801)
IOCTL_GET_VOLUMES = _ctl_code(_DEV_TYPE, 0x802)
IOCTL_ENABLE = _ctl_code(_DEV_TYPE, 0x803, 0, 2)
IOCTL_DISABLE = _ctl_code(_DEV_TYPE, 0x804, 0, 2)
IOCTL_COMMIT = _ctl_code(_DEV_TYPE, 0x805, 0, 2)
IOCTL_DISCARD = _ctl_code(_DEV_TYPE, 0x806, 0, 2)


class TGSHADOW_STATUS(ctypes.Structure):
    """必须与 driver/tgshadow/tgshadow.h 的 TGSHADOW_STATUS 逐字段一致。"""
    _fields_ = [
        ("VersionMajor", wintypes.DWORD),
        ("VersionMinor", wintypes.DWORD),
        ("Protected", wintypes.DWORD),
        ("FilterAttached", wintypes.DWORD),
        ("TargetVolumeNumber", wintypes.DWORD),
        ("Reserved0", wintypes.DWORD),
        ("ShadowBytesTotal", ctypes.c_ulonglong),
        ("ShadowBytesUsed", ctypes.c_ulonglong),
        ("ReadCount", ctypes.c_ulonglong),
        ("WriteCount", ctypes.c_ulonglong),
        ("RedirectedBlocks", ctypes.c_ulonglong),
        ("WritesPassive", ctypes.c_ulonglong),
        ("WritesHighIrql", ctypes.c_ulonglong),
        ("CowEntered", ctypes.c_ulonglong),
        ("CowNoBuffer", ctypes.c_ulonglong),
        ("CowWriteOk", ctypes.c_ulonglong),
        ("CowReadOk", ctypes.c_ulonglong),
        ("CowFailed", ctypes.c_ulonglong),
        ("CowLastStatus", wintypes.DWORD),
        ("CommittedBlocks", wintypes.DWORD),
        ("DiscardedBlocks", wintypes.DWORD),
        ("LastCommitFailed", wintypes.DWORD),
        ("PagingIo", wintypes.DWORD),
    ]


class TGSHADOW_ENABLE_INPUT(ctypes.Structure):
    _fields_ = [
        ("VolumeNumber", wintypes.DWORD),
        ("Flags", wintypes.DWORD),
        ("ShadowBytes", ctypes.c_ulonglong),
        ("VolumeBytes", ctypes.c_ulonglong),
        ("ShadowPath", wintypes.WCHAR * 260),
    ]


class TGSHADOW_VOLUME_INFO(ctypes.Structure):
    _fields_ = [
        ("VolumeNumber", wintypes.DWORD),
        ("IsTarget", wintypes.DWORD),
        ("TotalBytes", ctypes.c_ulonglong),
    ]


# ----------------------------------------------------------------- 底层调用

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                             ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                             ctypes.c_void_p]
_k32.DeviceIoControl.restype = wintypes.BOOL
_k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                 ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.GetLogicalDrives.restype = wintypes.DWORD
_k32.GetDriveTypeW.restype = wintypes.UINT
_k32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
_k32.GetDiskFreeSpaceExW.restype = wintypes.BOOL
_k32.GetDiskFreeSpaceExW.argtypes = [wintypes.LPCWSTR,
                                     ctypes.POINTER(ctypes.c_ulonglong),
                                     ctypes.POINTER(ctypes.c_ulonglong),
                                     ctypes.POINTER(ctypes.c_ulonglong)]
_k32.GetFileSizeEx.restype = wintypes.BOOL
_k32.GetFileSizeEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_ulonglong)]
_k32.SetFilePointerEx.restype = wintypes.BOOL
_k32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                  ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD]
_k32.SetEndOfFile.restype = wintypes.BOOL
_k32.SetEndOfFile.argtypes = [wintypes.HANDLE]


class ShadowError(RuntimeError):
    """驱动不可用或调用失败。"""

    def __init__(self, msg: str, code: int = 0):
        super().__init__(msg)
        self.code = code


def _open():
    h = _k32.CreateFileW(DEVICE_PATH, _GENERIC_READ | _GENERIC_WRITE, 3, None,
                         _OPEN_EXISTING, 0, None)
    if h == _INVALID_HANDLE or h is None:
        err = ctypes.get_last_error()
        raise ShadowError(f"无法打开 {DEVICE_PATH}（错误 {err}）：驱动未安装或未启动", err)
    return h


def _ioctl(handle, code: int, in_buf=None, out_size: int = 0):
    out = ctypes.create_string_buffer(out_size) if out_size else None
    ret = wintypes.DWORD(0)
    ok = _k32.DeviceIoControl(handle, code, in_buf, 
                              ctypes.sizeof(in_buf) if in_buf is not None else 0,
                              out, out_size, ctypes.byref(ret), None)
    if not ok:
        err = ctypes.get_last_error()
        raise ShadowError(f"DeviceIoControl(0x{code:08X}) 失败（错误 {err}）", err)
    return out


def available() -> bool:
    """驱动是否就绪（能打开控制设备）。"""
    try:
        h = _open()
    except ShadowError:
        return False
    _k32.CloseHandle(h)
    return True


def version() -> str:
    h = _open()
    try:
        buf = _ioctl(h, IOCTL_GET_VERSION, None, 64)
        return buf.raw.decode("utf-16-le", "ignore").split("\x00")[0]
    finally:
        _k32.CloseHandle(h)


def status() -> TGSHADOW_STATUS:
    h = _open()
    try:
        buf = _ioctl(h, IOCTL_GET_STATUS, None, ctypes.sizeof(TGSHADOW_STATUS))
        return TGSHADOW_STATUS.from_buffer_copy(buf)
    finally:
        _k32.CloseHandle(h)


def in_memory_bytes() -> int:
    """当前内核里影子占用的内存（用于判断影子是否合理地放在磁盘上）。"""
    try:
        import ctypes as _c
        class _PMC(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t),
                        ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
        return 0
    except Exception:
        return 0

def enable(volume_number: int = 0, shadow_path: str = "", shadow_mb: int = 0,
           volume_bytes: int = 0, attach_only: bool = False):
    """启用保护。

    volume_number=0 表示由驱动自动识别系统卷（推荐：用户态在 SYSTEM 会话里
    算不出 C: 的卷号）。shadow_path 为空则退化为内存影子（仅供验证，容量受
    物理内存限制，长时间使用会因容量耗尽而失去保护）。
    """
    h = _open()
    try:
        inp = TGSHADOW_ENABLE_INPUT()
        inp.VolumeNumber = volume_number
        inp.Flags = FLAG_ATTACH_ONLY if attach_only else 0
        inp.ShadowBytes = int(shadow_mb) * 1024 * 1024
        inp.VolumeBytes = int(volume_bytes)
        if shadow_path:
            nt = shadow_path if shadow_path.startswith("\\??\\") else "\\??\\" + shadow_path
            inp.ShadowPath = nt
        _ioctl(h, IOCTL_ENABLE, inp)
    finally:
        _k32.CloseHandle(h)


def disable():
    h = _open()
    try:
        _ioctl(h, IOCTL_DISABLE)
    finally:
        _k32.CloseHandle(h)


def commit() -> int:
    """把影子里的改动写回真实卷（家长确认"保留"）。返回已提交块数。"""
    h = _open()
    try:
        _ioctl(h, IOCTL_COMMIT)
    finally:
        _k32.CloseHandle(h)
    try:
        return int(status().CommittedBlocks)
    except Exception:
        return 0


def discard() -> int:
    """丢弃影子里的全部改动（还原）。返回丢弃块数。"""
    h = _open()
    try:
        _ioctl(h, IOCTL_DISCARD)
    finally:
        _k32.CloseHandle(h)
    try:
        return int(status().DiscardedBlocks)
    except Exception:
        return 0


def list_volumes():
    """返回驱动可见的卷号列表 [(volume_number, is_target), ...]。"""
    h = _open()
    try:
        size = ctypes.sizeof(TGSHADOW_VOLUME_INFO) * 32
        ret = wintypes.DWORD(0)
        out = ctypes.create_string_buffer(size)
        _k32.DeviceIoControl(h, IOCTL_GET_VOLUMES, None, 0, out, size,
                             ctypes.byref(ret), None)
        n = ret.value // ctypes.sizeof(TGSHADOW_VOLUME_INFO)
        items = []
        for i in range(n):
            info = TGSHADOW_VOLUME_INFO.from_buffer_copy(
                out.raw[i * ctypes.sizeof(TGSHADOW_VOLUME_INFO):
                        (i + 1) * ctypes.sizeof(TGSHADOW_VOLUME_INFO)])
            items.append((int(info.VolumeNumber), bool(info.IsTarget)))
        return items
    finally:
        _k32.CloseHandle(h)


# ----------------------------------------------------------------- 影子文件选址

def system_drive() -> str:
    return (os.environ.get("SystemDrive") or "C:").rstrip("\\").upper()


def writable_drives():
    """返回 [(盘符, 可用字节), ...]，已排除系统盘、光驱、不可写盘。"""
    out = []
    mask = _k32.GetLogicalDrives()
    for i in range(26):
        if not (mask >> i) & 1:
            continue
        drive = f"{chr(ord('A') + i)}:"
        if drive.upper() == system_drive():
            continue
        root = drive + "\\"
        # 1 = 无根目录, 2 = 可移动, 3 = 固定, 4 = 远程, 5 = 光驱, 6 = RAM
        if _k32.GetDriveTypeW(root) not in (2, 3, 6):
            continue
        free = ctypes.c_ulonglong(0)
        total = ctypes.c_ulonglong(0)
        if not _k32.GetDiskFreeSpaceExW(root, ctypes.byref(free),
                                        ctypes.byref(total), None):
            continue
        out.append((drive, int(free.value)))
    out.sort(key=lambda x: -x[1])
    return out


def pick_shadow_location(need_mb: int):
    """挑一个能放下影子的盘。返回 (影子文件路径, 实际可用 MB)；没有则 (None, 0)。"""
    need = int(need_mb) * 1024 * 1024
    for drive, free in writable_drives():
        if free <= need * 11 // 10:      # 留 10% 余量
            continue
        return (os.path.join(drive + "\\", "TimeGuardShadow.bin"), free // (1024 * 1024))
    return (None, 0)


def ensure_shadow_file(path: str, mb: int) -> int:
    """创建/扩充影子文件到指定大小，返回实际字节数。"""
    want = int(mb) * 1024 * 1024
    _k32.CreateFileW.restype = wintypes.HANDLE
    h = _k32.CreateFileW(path, _GENERIC_READ | _GENERIC_WRITE, 0, None, 4, 0, None)
    if h == _INVALID_HANDLE or h is None:
        raise ShadowError(f"无法创建影子文件 {path}（错误 {ctypes.get_last_error()}）")
    try:
        cur = ctypes.c_ulonglong(0)
        _k32.GetFileSizeEx(h, ctypes.byref(cur))
        if int(cur.value) >= want:
            return int(cur.value)
        pos = ctypes.c_longlong(want)
        if not _k32.SetFilePointerEx(h, pos, None, 0) or not _k32.SetEndOfFile(h):
            raise ShadowError(f"预分配影子文件失败（错误 {ctypes.get_last_error()}）")
        return want
    finally:
        _k32.CloseHandle(h)


# ----------------------------------------------------------------- 配置（注册表）

def _open_cfg(write: bool = False):
    import winreg
    return winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, CFG_KEY, 0,
                              winreg.KEY_WRITE if write else winreg.KEY_READ)


def cfg_get(name: str, default=None):
    import winreg
    try:
        with _open_cfg() as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return default


def cfg_set(name: str, value):
    import winreg
    with _open_cfg(True) as k:
        if isinstance(value, int):
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, value)
        else:
            winreg.SetValueEx(k, name, 0, winreg.REG_SZ, str(value))


def load_config() -> dict:
    return {
        "enabled": bool(cfg_get("Enabled", 0)),
        "shadow_path": cfg_get("ShadowPath", "") or "",
        "shadow_mb": int(cfg_get("ShadowMB", 0) or 0),
        "delay_sec": int(cfg_get("AutoEnableDelaySec", 120) or 120),
        "volume": int(cfg_get("VolumeNumber", 0) or 0),
        "password_hash": cfg_get("ParentPasswordHash", "") or "",
        "boot_attempts": int(cfg_get("BootAttempts", 0) or 0),
    }


def apply_config(enabled: bool, shadow_path: str = "", shadow_mb: int = 0,
                 delay_sec: int = 120, volume: int = 0):
    """把主程序的设置写进注册表（服务与驱动都读这里）。"""
    cfg_set("Enabled", 1 if enabled else 0)
    cfg_set("ShadowPath", shadow_path or "")
    cfg_set("ShadowMB", int(shadow_mb))
    cfg_set("AutoEnableDelaySec", int(delay_sec))
    cfg_set("VolumeNumber", int(volume))


def sync_parent_password(cfg: dict) -> bool:
    """把策略里的家长密码哈希同步给关机确认弹窗。

    share.util.sha256_hex 与 tgshadow_ask.exe 的哈希算法一致
    （UTF-8 字节 + 小写十六进制），所以直接复制即可。
    """
    h = (cfg or {}).get("parent_password_hash") or ""
    try:
        cfg_set("ParentPasswordHash", h)
        return True
    except OSError:
        return False


# ----------------------------------------------------------------- 驱动与服务

DRIVER_NAME = "tgshadow"
SERVICE_NAME = "TgShadowSvc"


def driver_files_present() -> bool:
    root = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                        "System32", "drivers")
    return os.path.isfile(os.path.join(root, "tgshadow.sys"))


def service_installed() -> bool:
    return _sc("query", SERVICE_NAME)[0] == 0


def service_running() -> bool:
    rc, out = _sc("query", SERVICE_NAME)
    return rc == 0 and "RUNNING" in out


def _sc(*args):
    try:
        p = subprocess.run(["sc.exe", *args], capture_output=True, text=True,
                           encoding="gbk", errors="ignore",
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except Exception as e:                                    # pragma: no cover
        return 1, str(e)


def service_start():
    return _sc("start", SERVICE_NAME)


def service_stop():
    return _sc("stop", SERVICE_NAME)


def service_ask_now():
    """让服务立刻走一遍"关机询问"流程（自定义控制码 128），用于测试。"""
    return _sc("control", SERVICE_NAME, "128")


VOLUME_CLASS_GUID = "{71a27cdd-812a-11d0-bec7-08002be2092f}"


def deploy(app_root_dir: str):
    r"""把驱动 + 服务部署到系统里（需要管理员权限）。返回 (ok, 说明文字)。

    做四件事：
      1) 把 tgshadow.sys 复制到 System32\drivers
      2) 注册内核驱动服务（type= kernel, start= boot —— 必须 boot 才能随卷一起加载）
      3) 把 tgshadow 追加到卷类 UpperFilters（这是挂上卷过滤栈的唯一正确方式）
      4) 部署并安装用户态服务 tgshadow_svc.exe（延迟启用 + 关机确认弹窗）
    """
    import shutil
    import winreg

    need = ["tgshadow.sys", "tgshadow_svc.exe", "tgshadow_ask.exe"]
    # 兼容两种布局：打包后放 <安装目录>\driver，开发树里在 driver\build\Debug
    cands = [os.path.join(app_root_dir, "driver"),
             os.path.join(app_root_dir, "driver", "build", "Debug"),
             app_root_dir]
    src = None
    for c in cands:
        if all(os.path.isfile(os.path.join(c, f)) for f in need):
            src = c
            break
    if src is None:
        have = {c: [f for f in need if os.path.isfile(os.path.join(c, f))]
                for c in cands}
        return False, ("安装文件缺失：" + "、".join(need) +
                       "\n查找过：" + "；".join(f"{k}({len(v)}/3)" for k, v in have.items()) +
                       "\n请先运行 driver\\build_driver.bat 与 build_svc.bat")

    sysroot = os.environ.get("SystemRoot", r"C:\Windows")
    drv_dir = os.path.join(sysroot, "System32", "drivers")
    drv_dst = os.path.join(drv_dir, "tgshadow.sys")
    try:
        shutil.copy2(os.path.join(src, "tgshadow.sys"), drv_dst)
    except OSError as e:
        return False, f"复制驱动失败：{e}\n（请以管理员身份运行本程序）"

    # 1) 驱动服务
    rc, out = _sc("create", DRIVER_NAME, "type=", "kernel", "start=", "boot",
                  "binPath=", r"System32\drivers\tgshadow.sys",
                  "DisplayName=", "TimeGuard Shadow Filter")
    if rc != 0:
        _sc("config", DRIVER_NAME, "type=", "kernel", "start=", "boot",
            "binPath=", r"System32\drivers\tgshadow.sys", "DisplayName=",
            "TimeGuard Shadow Filter")

    # 2) 卷类 UpperFilters（已存在则跳过；必须是 volsnap 之后追加，不能覆盖）
    try:
        key_path = ("SYSTEM\\CurrentControlSet\\Control\\Class\\"
                    + VOLUME_CLASS_GUID)
        with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, key_path, 0,
                                winreg.KEY_READ | winreg.KEY_WRITE) as k:
            try:
                cur, _t = winreg.QueryValueEx(k, "UpperFilters")
            except OSError:
                cur = ["volsnap"]
            cur = [str(x) for x in cur]
            if DRIVER_NAME not in cur:
                cur.append(DRIVER_NAME)
                winreg.SetValueEx(k, "UpperFilters", 0, winreg.REG_MULTI_SZ, cur)
    except OSError as e:
        return False, f"写入卷过滤注册表失败：{e}"

    # 3) 用户态服务
    for name in ("tgshadow_svc.exe", "tgshadow_ask.exe"):
        try:
            shutil.copy2(os.path.join(src, name), os.path.join(app_root_dir, name))
        except OSError:
            pass
    svc_exe = os.path.join(app_root_dir, "tgshadow_svc.exe")
    try:
        p = subprocess.run([svc_exe, "install"], capture_output=True, text=True,
                           encoding="gbk", errors="ignore",
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        svc_out = (p.stdout or "") + (p.stderr or "")
    except Exception as e:                                       # pragma: no cover
        svc_out = str(e)

    running = service_running()
    return True, ("驱动与服务已部署完成。\n"
                  "• 影子保护将在下次开机后自动启用（开机 120 秒后，避免拖慢启动）\n"
                  "• 关机时会弹出确认窗口：选择【保留修改】需要家长密码，否则一律还原\n"
                  f"• 服务状态：{'运行中' if running else '已安装，重启后生效'}\n"
                  f"• 服务安装输出：{(svc_out or '').strip()[:200]}")


def driver_status_text() -> str:
    """给界面用的一句话状态。"""
    if not available():
        return "驱动未就绪（未安装或未启动）"
    try:
        st = status()
    except ShadowError as e:
        return f"驱动异常：{e}"
    if st.Protected:
        used = st.ShadowBytesUsed / (1024 * 1024)
        return (f"保护中：受保护卷 {st.TargetVolumeNumber}，已重定向 {st.RedirectedBlocks} 块"
                f"（{used:.1f} MB），COW 失败 {st.CowFailed}")
    return f"已就绪但未启用保护（拦截 读{st.ReadCount}/写{st.WriteCount}）"
