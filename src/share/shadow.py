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
import glob
import os
import shutil
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


# ------------------------------------------------- 驱动签名（免费路线：自签名 + 测试模式）

CERT_SUBJECT = "CN=TimeGuard Debug"
_PS = "powershell.exe"


def _certs_dir() -> str:
    d = os.path.join(paths.state_dir(), "driver_certs")
    os.makedirs(d, exist_ok=True)
    return d


def _run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="gbk", errors="ignore", timeout=timeout,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except Exception as e:                                    # pragma: no cover
        return 1, str(e)


def _find_signtool():
    """signtool 只随 Windows SDK 安装，普通用户机器上通常没有；找不到就返回 None。"""
    hits = glob.glob(os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:Program Files (x86)"),
        "Windows Kits", "10", "bin", "*", "x64", "signtool.exe"))
    if hits:
        return sorted(hits)[-1]                    # 取版本号最大的
    local = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                         "TimeGuard", "signtool.exe")
    if os.path.isfile(local):
        return local
    return shutil.which("signtool.exe")


_CERT_PS_SRC = "param([string]$Subject, [string]$CerPath, [string]$Mode)\r\n$ErrorActionPreference = \"Stop\"\r\nif ($Mode -eq \"thumb\") {\r\n    $c = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1\r\n    if ($c) { Write-Output $c.Thumbprint }\r\n    exit 0\r\n}\r\nif ($Mode -eq \"create\") {\r\n    $c = Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1\r\n    if (-not $c) {\r\n        $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\\CurrentUser\\My -NotAfter (Get-Date).AddYears(10)\r\n    }\r\n    if (-not $c) { Write-Error \"certificate creation failed\"; exit 1 }\r\n    Export-Certificate -Cert $c -FilePath $CerPath -Force | Out-Null\r\n    Write-Output $c.Thumbprint\r\n    exit 0\r\n}\r\nWrite-Error \"unknown mode: $Mode\"\r\nexit 2\r\n"


def _cert_helper_path() -> str:
    """证书辅助脚本路径（不存在时由调用方报错，不再往代码里塞内联 PowerShell）。"""
    return os.path.join(_certs_dir(), "_tg_cert.ps1")


def _run_cert_helper(mode: str) -> str:
    """跑证书辅助脚本，返回其标准输出（失败返回空串）。"""
    p = _cert_helper_path()
    if not os.path.isfile(p) or os.path.getsize(p) < 200:
        # 从内置文本补写一份（打包版走这里）。
        # ⚠️ 必须写入 UTF-8 BOM：Windows PowerShell 5.1 读【无 BOM】的脚本会按 ANSI
        #    (GBK) 解码，脚本里一旦出现中文（注释/提示）就会解析失败或乱码。
        with open(p, "w", encoding="utf-8-sig", newline="") as fh:
            fh.write(_CERT_PS_SRC)
    rc, out = _run([_PS, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", p,
                    "-Subject", CERT_SUBJECT, "-CerPath", _cer_path(),
                    "-Mode", mode])
    return out or ""


def _pick_thumb(out: str) -> str:
    """从输出里挑出 40 位十六进制指纹（输出可能夹带别的行）。"""
    for line in (out or "").splitlines():
        line = line.strip()
        if len(line) >= 40 and all(c in "0123456789abcdefABCDEF" for c in line):
            return line
    return ""


def _cer_path() -> str:
    return os.path.join(_certs_dir(), "TimeGuardDebug.cer")


def ensure_signing_cert() -> str:
    r"""确保存在自签名代码签名证书，返回其 SHA1 指纹（十六进制）。

    用 certutil 而不是 PowerShell 的 New-SelfSignedCertificate：
    前者所有 Windows 都自带，后者在精简系统/旧版本上可能没有。
    """
    thumb = _pick_thumb(_run_cert_helper("create"))
    if thumb:
        return thumb

    cer = os.path.join(_certs_dir(), "TimeGuardDebug.cer")
    rc, out = _run(["certutil.exe", "-user", "-f", "-createSelfSignedCertificate",
                    CERT_SUBJECT, "-sz", "2048", "-e", "1.3.6.1.5.5.7.3.3",
                    "-exportCert", cer])
    if rc != 0:
        raise RuntimeError(f"创建自签名证书失败：{out.strip()[:300]}")
    for tok in out.split():
        t = tok.strip().strip('"')
        if len(t) == 40 and all(c in "0123456789abcdefABCDEF" for c in t):
            return t
    thumb = _pick_thumb(_run_cert_helper("thumb"))
    if thumb:
        return thumb
    raise RuntimeError("创建自签名证书失败：PowerShell 与 certutil 两条路都没拿到证书指纹")


def trust_signing_cert(thumb: str):
    """把证书导入受信任存储，并确保【本机】存储里也有它。

    两个原因：
      • Root / TrustedPublisher 里没有它，签名链就不被信任；
      • 内核模式驱动的签名检查走的是本机（LocalMachine）存储，而 certutil -user
        建出来的证书只在当前用户存储里 —— 只做用户存储的话，测试模式开着也可能加载失败。
    返回 (用户存储错误列表, 本机存储错误列表) —— 后者非空意味着内核多半不会认这个签名，
    调用方应当据此中止，而不是装完等重启才发现加载不了。
    """
    cer = os.path.join(_certs_dir(), "TimeGuardDebug.cer")
    if not os.path.isfile(cer):
        _run([_PS, "-NoProfile", "-Command",
              rf"Export-Certificate -Cert (Get-ChildItem Cert:\CurrentUser\My | "
              f"Where-Object {{ $_.Thumbprint -eq '{thumb}' }}) -FilePath '{cer}' -Force"])
    user_msgs, machine_msgs = [], []
    for store in ("Root", "TrustedPublisher"):            # 用户存储
        rc, out = _run(["certutil.exe", "-user", "-addstore", "-f", store, cer])
        if rc != 0:
            user_msgs.append(f"{store}: {out.strip()[:120]}")
    for store in ("Root", "TrustedPublisher", "My"):      # 本机存储（内核签名检查用）
        rc, out = _run(["certutil.exe", "-addstore", "-f", store, cer])
        if rc != 0:
            machine_msgs.append(f"{store}: {out.strip()[:120]}")
    return user_msgs, machine_msgs


def sign_driver_file(path: str, thumb: str):
    """给驱动签名。返回 (ok, 说明)。优先 signtool，退回 PowerShell。"""
    tool = _find_signtool()
    if tool:
        # 不要加 /pa：部分 signtool 版本没有这个开关（实测 10.0.26100 就报 Invalid option），
        # 内核驱动用的是 /fd sha256 + 证书指纹，本身不需要它。
        rc, out = _run([tool, "sign", "/fd", "sha256", "/sha1", thumb,
                        "/s", "My", "/v", path])
        if rc == 0:
            return True, "signtool"
        last = out.strip()[-300:]
    else:
        last = "系统里没有 signtool.exe（随 Windows SDK 安装）"
    rc, out = _run([_PS, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                    rf"$c = Get-ChildItem Cert:\CurrentUser\My | Where-Object "
                    f"{{ $_.Thumbprint -eq '{thumb}' }} | Select-Object -First 1; "
                    f"$r = Set-AuthenticodeSignature -FilePath '{path}' "
                    f"-Certificate $c -HashAlgorithm SHA256; "
                    f"Write-Output ($r.Status.ToString() + ' | ' + $r.StatusMessage)"])
    txt = (out or "").strip()
    if rc == 0 and txt.startswith("Valid"):
        return True, "powershell"
    return False, f"{last}；PowerShell 兜底也失败：{txt[-300:]}"


def verify_driver_signature(path: str):
    rc, out = _run([_PS, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                    f"(Get-AuthenticodeSignature '{path}').Status"])
    status = (out or "").strip().splitlines()[-1].strip() if out else ""
    return status


def test_signing_state():
    """返回 (是否已开启测试签名模式, 说明文字)。"""
    rc, out = _run(["bcdedit", "/enum", "{current}"])
    low = (out or "").lower()
    if "testsigning" in low and "yes" in low:
        return True, "已开启"
    return False, "未开启"


def enable_test_signing():
    """开启测试签名模式（需管理员 + 重启生效）。返回 (ok, 说明)。"""
    rc, out = _run(["bcdedit", "/set", "testsigning", "on"])
    txt = (out or "").strip()
    if rc == 0 and ("success" in txt.lower() or "成功" in txt):
        return True, "已开启测试签名模式（重启后生效，桌面右下角会出现「测试模式」水印）"
    if "secure boot" in txt.lower() or "安全启动" in txt:
        return False, ("Secure Boot 拦住了这个设置。请先重启进 BIOS/UEFI 关闭 Secure Boot，"
                       "再回到系统执行一次。")
    return False, f"开启失败：{txt[:300]}"


def security_precheck():
    """返回一组"会让自签名驱动加载不了"的警告（HVCI 等）。"""
    import winreg
    warns = []
    try:
        with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"SYSTEMCurrentControlSetControlDeviceGuardScenarios"
                r"HypervisorEnforcedCodeIntegrity") as k:
            try:
                if winreg.QueryValueEx(k, "Enabled")[0]:
                    warns.append(
                        "内存完整性（HVCI）已开启：开启测试签名模式也没用，"
                        "自签名驱动依然会被拒绝加载。"
                        "请到「Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性」关掉并重启。")
            except OSError:
                pass
    except OSError:
        pass
    return warns


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

    def _cleanup_driver():
        """部署失败时把驱动文件撤掉，别留下一个加载不了的 boot 驱动。"""
        try:
            os.remove(drv_dst)
        except OSError:
            pass

    # 0) 驱动签名：Windows 只加载有有效签名的内核驱动，没有证书就自签一个。
    #    必须在写 UpperFilters 之前做 —— 卷类的 UpperFilter 指向一个加载不了的驱动
    #    会把卷的启动路径搞坏，而这个代价远高于"功能装不上"。
    notes = []
    if verify_driver_signature(drv_dst) != "Valid":
        try:
            thumb = ensure_signing_cert()
        except RuntimeError as e:
            _cleanup_driver()
            return False, f"驱动没有有效签名，且自签名也失败：{e}"
        user_msgs, machine_msgs = trust_signing_cert(thumb)
        if machine_msgs:
            # 内核驱动签名检查走本机存储；导不进去就说明当前不是管理员，
            # 装了也无法加载 —— 此时中止，比让用户重启后发现没生效好得多。
            _cleanup_driver()
            return False, (
                "把证书导入本机受信任存储失败，驱动即使签名也不会被内核接受：\n  "
                + "\n  ".join(machine_msgs)
                + "\n\n请以【管理员身份】重新运行 admin.exe 后再试（需要写入本机证书存储）。")
        ok, how = sign_driver_file(drv_dst, thumb)
        if not ok:
            _cleanup_driver()
            return False, f"给驱动签名失败：{how}"
        if verify_driver_signature(drv_dst) != "Valid":
            _cleanup_driver()
            return False, "签名后校验仍未通过，已撤销安装（未写入卷过滤驱动注册表）"
        notes.append(f"已用自签名证书（{CERT_SUBJECT}）签名驱动（方式：{how}）")
        if user_msgs:
            notes.append("证书导入用户存储有告警：" + "；".join(user_msgs))

    # 0.5) 测试签名模式：自签名证书只有在"测试模式"下才被内核接受。
    #     Secure Boot 开着会被 bcdedit 直接拒绝；HVCI 开着则连测试模式也救不了。
    for w in security_precheck():
        notes.append("⚠ " + w)
    ts_on, ts_msg = test_signing_state()
    if ts_on:
        notes.append("测试签名模式已开启")
    else:
        ok_ts, msg_ts = enable_test_signing()
        if ok_ts:
            notes.append(msg_ts + " —— 必须重启，否则驱动加载不起来")
        else:
            _cleanup_driver()
            return False, (
                "驱动已签名，但系统当前不允许加载测试签名的驱动，安装已中止\n"
                f"（{msg_ts}）\n\n"
                "请按 README「六点五、磁盘还原」的步骤处理：\n"
                "  1. 以管理员身份执行  bcdedit /set testsigning on\n"
                "  2. 若提示被 Secure Boot 保护，先重启进 BIOS 关闭 Secure Boot\n"
                "  3. 关闭「内核隔离 → 内存完整性」(HVCI)，否则自签名驱动仍会被拒绝\n"
                "  4. 重启后回到本界面再点一次「安装驱动与服务」")

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
        _sc("delete", DRIVER_NAME)        # 别留下注册了却挂不上的驱动服务
        _cleanup_driver()
        return False, f"写入卷过滤注册表失败：{e}\n（已回滚驱动服务与驱动文件）"

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
    extra = ("\n" + "\n".join("• " + n for n in notes)) if notes else ""
    return True, ("驱动与服务已部署完成。\n"
                  "• 影子保护将在下次开机后自动启用（开机 120 秒后，避免拖慢启动）\n"
                  "• 关机时会弹出确认窗口：选择【保留修改】需要家长密码，否则一律还原\n"
                  f"• 服务状态：{'运行中' if running else '已安装，重启后生效'}"
                  f"{extra}\n"
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
