"""锁屏加固（Windows 原生 API，ctypes 实现，无第三方依赖）：

1. hide_taskbar / show_taskbar  —— 隐藏/恢复任务栏（主任务栏 + 多显示器副任务栏）
2. clip_to_window / unclip      —— 把鼠标限制在锁屏窗口区域内（防点到锁窗外）
3. force_topmost                —— 持续置顶压制，防止被其它程序窗口盖住
4. start_keyboard_block / stop  —— 低级键盘钩子，屏蔽 Alt+Tab / Win 键 / Alt+F4 /
                                   Ctrl+Esc / Ctrl+Shift+Esc(任务管理器) / 菜单键等逃生键

注意：Ctrl+Alt+Del 是内核保留的"安全注意序列"，用户态无法屏蔽（系统设计如此）。
"""
import ctypes
import threading
import time
from ctypes import wintypes

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)

SW_HIDE = 0
SW_SHOW = 5
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010

WH_KEYBOARD_LL = 13
HC_ACTION = 0
WM_KEYDOWN = 0x0100
WM_SYSKEYDOWN = 0x0104

VK_TAB = 0x09
VK_ESCAPE = 0x1B
VK_CONTROL = 0x11
VK_MENU = 0x12  # Alt
VK_LWIN = 0x5B
VK_RWIN = 0x5C
VK_APPS = 0x5D  # 右键菜单键
VK_F4 = 0x73
VK_LSHIFT = 0xA0
VK_RSHIFT = 0xA1


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wintypes.DWORD), ("scanCode", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG))]


WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)

_user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
_user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.IsWindowVisible.restype = wintypes.BOOL
_user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
_user32.ClipCursor.argtypes = [ctypes.POINTER(RECT)]
_user32.ClipCursor.restype = wintypes.BOOL
_user32.GetClipCursor.argtypes = [ctypes.POINTER(RECT)]
_user32.GetClipCursor.restype = wintypes.BOOL
_user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, wintypes.UINT]
_user32.SetForegroundWindow.argtypes = [wintypes.HWND]
_user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
_user32.GetAsyncKeyState.restype = ctypes.c_short
_user32.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
_user32.SetWindowsHookExW.restype = wintypes.HANDLE
_user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD]
_user32.UnhookWindowsHookEx.argtypes = [wintypes.HANDLE]
_user32.CallNextHookEx.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
_user32.CallNextHookEx.restype = ctypes.c_long
_user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                                wintypes.UINT, wintypes.UINT]
_user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
_user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
_user32.PostThreadMessageW.argtypes = [wintypes.DWORD, wintypes.UINT, wintypes.WPARAM,
                                       wintypes.LPARAM]
_k32.GetCurrentThreadId.restype = wintypes.DWORD


# ---------------- 任务栏 ----------------

def _taskbar_hwnds():
    hwnds = []

    @WNDENUMPROC
    def _cb(h, _):
        buf = ctypes.create_unicode_buffer(128)
        _user32.GetClassNameW(h, buf, 128)
        if buf.value in ("Shell_TrayWnd", "Shell_SecondaryTrayWnd"):
            hwnds.append(h)
        return True

    _user32.EnumWindows(_cb, 0)
    return hwnds


def hide_taskbar():
    """隐藏任务栏（含多显示器副任务栏）。"""
    for h in _taskbar_hwnds():
        _user32.ShowWindow(h, SW_HIDE)


def show_taskbar():
    """恢复任务栏显示。"""
    for h in _taskbar_hwnds():
        _user32.ShowWindow(h, SW_SHOW)


def taskbar_visible() -> bool:
    """任务栏当前是否可见（测试用）。"""
    hs = _taskbar_hwnds()
    return bool(hs) and any(_user32.IsWindowVisible(h) for h in hs)


# ---------------- 鼠标区域限制 ----------------

def clip_to_window(hwnd) -> bool:
    """把鼠标限制在指定窗口的屏幕矩形内（全屏锁窗 = 主显示器）。"""
    r = RECT()
    if not _user32.GetWindowRect(hwnd, ctypes.byref(r)):
        return False
    _user32.ClipCursor(ctypes.byref(r))
    return True


def clip_to_rect(left, top, right, bottom):
    r = RECT(left, top, right, bottom)
    _user32.ClipCursor(ctypes.byref(r))


def unclip():
    """解除鼠标限制。"""
    _user32.ClipCursor(None)


def get_clip_rect():
    """查询当前鼠标裁剪矩形（未裁剪时返回屏幕大小）。"""
    r = RECT()
    if _user32.GetClipCursor(ctypes.byref(r)):
        return (r.left, r.top, r.right, r.bottom)
    return None


# ---------------- 置顶压制 ----------------

def force_topmost(hwnd):
    """把锁窗压到所有窗口之上（配合轮询调用，防被游戏/全屏程序盖住）。"""
    _user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
    _user32.SetForegroundWindow(hwnd)


# ---------------- 系统快捷键屏蔽 ----------------

_hook = None
_pump_thread = None
_pump_tid = 0
_cb_ref = None


def _key_blocked(vk: int) -> bool:
    alt = _user32.GetAsyncKeyState(VK_MENU) & 0x8000
    ctrl = _user32.GetAsyncKeyState(VK_CONTROL) & 0x8000
    shift = (_user32.GetAsyncKeyState(VK_LSHIFT) | _user32.GetAsyncKeyState(VK_RSHIFT)) & 0x8000
    if vk in (VK_LWIN, VK_RWIN, VK_APPS):
        return True                      # Win 键 / 菜单键
    if vk == VK_TAB and alt:
        return True                      # Alt+Tab / Alt+Shift+Tab
    if vk == VK_ESCAPE and (alt or ctrl or (ctrl and shift)):
        return True                      # Alt+Esc / Ctrl+Esc / Ctrl+Shift+Esc(任务管理器)
    if vk == VK_F4 and alt:
        return True                      # Alt+F4
    return False


def _hook_proc(nCode, wParam, lParam):
    if nCode == HC_ACTION and wParam in (WM_KEYDOWN, WM_SYSKEYDOWN):
        ks = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
        if _key_blocked(int(ks.vkCode)):
            return 1  # 吞掉该按键
    return _user32.CallNextHookEx(_hook, nCode, wParam, lParam)


def start_keyboard_block() -> bool:
    """安装全局低级键盘钩子（钩子与消息泵同在泵线程，回调在其中执行）。"""
    global _hook, _pump_thread, _pump_tid, _cb_ref
    if _hook:
        return True
    _cb_ref = HOOKPROC(_hook_proc)
    ready = threading.Event()
    result = {}

    def worker():
        global _hook, _pump_tid
        _pump_tid = int(_k32.GetCurrentThreadId())
        h = _user32.SetWindowsHookExW(WH_KEYBOARD_LL, _cb_ref, None, 0)
        result["hook"] = h
        ready.set()
        if not h:
            return
        msg = wintypes.MSG()
        while _user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            _user32.TranslateMessage(ctypes.byref(msg))
            _user32.DispatchMessageW(ctypes.byref(msg))

    _pump_thread = threading.Thread(target=worker, daemon=True, name="keyblock-pump")
    _pump_thread.start()
    ready.wait(3)
    _hook = result.get("hook")
    return bool(_hook)


def stop_keyboard_block():
    """卸载键盘钩子并结束消息泵线程。"""
    global _hook, _pump_tid
    if _hook:
        _user32.UnhookWindowsHookEx(_hook)
        _hook = None
    if _pump_tid:
        _user32.PostThreadMessageW(_pump_tid, 0x0012, 0, 0)  # WM_QUIT
        _pump_tid = 0


def hook_active() -> bool:
    return bool(_hook)
