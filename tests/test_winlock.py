"""锁屏加固模块测试（非入侵式，任务栏只会短暂隐藏/恢复约 2 秒）。

运行: python tests/test_winlock.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from lock import winlock  # noqa: E402


def main():
    print("[1] 任务栏隐藏/恢复...")
    winlock.hide_taskbar()
    time.sleep(0.6)
    hidden = not winlock.taskbar_visible()
    print(("PASS: 任务栏已隐藏" if hidden else "FAIL: 任务栏仍可见"))
    winlock.show_taskbar()
    time.sleep(0.6)
    shown = winlock.taskbar_visible()
    print(("PASS: 任务栏已恢复" if shown else "FAIL: 任务栏未恢复"))
    if not (hidden and shown):
        return 1

    print("[2] 鼠标区域限制/解除...")
    winlock.clip_to_rect(100, 100, 300, 300)
    time.sleep(0.3)
    r = winlock.get_clip_rect()
    clipped = r is not None and (r[0], r[1]) == (100, 100) and (r[2], r[3]) == (300, 300)
    print(("PASS: 鼠标被限制在 100,100-300,300" if clipped else f"FAIL: 限制矩形={r}"))
    winlock.unclip()
    time.sleep(0.3)
    r2 = winlock.get_clip_rect()
    # 未裁剪时 GetClipCursor 返回整个屏幕的矩形
    full = r2 is not None and (r2[2] - r2[0] > 500) and (r2[3] - r2[1] > 500)
    print(("PASS: 鼠标限制已解除（屏幕范围）" if full else f"FAIL: 仍受限 {r2}"))
    if not (clipped and full):
        return 1

    print("[3] 键盘钩子安装/卸载...")
    ok = winlock.start_keyboard_block()
    print(("PASS: 钩子已安装" if ok else "FAIL: 钩子安装失败"))
    active = winlock.hook_active()
    winlock.stop_keyboard_block()
    time.sleep(0.3)
    stopped = not winlock.hook_active()
    print(("PASS: 钩子已卸载" if stopped else "FAIL: 钩子仍生效"))
    if not (ok and active and stopped):
        return 1

    print("winlock 测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
