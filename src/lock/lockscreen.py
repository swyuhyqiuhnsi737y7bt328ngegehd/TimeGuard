"""锁定屏幕进程 lockscreen.exe：常驻；出现 lock.flag 时全屏锁定并加固
（隐藏任务栏 / 鼠标限制在锁屏区域 / 持续置顶压制 / 屏蔽 Alt+Tab、Win 键、
Alt+F4、任务管理器等逃生键）；家长密码解锁（自动加时）。

解锁或退出时自动恢复任务栏并解除鼠标限制；未锁定时周期性自愈，
防止进程被强杀后桌面残留异常状态。
"""
import os
import time
import tkinter as tk

from core import policy
from lock import winlock
from share import logger, paths, util

_POLL_MS = 1000
_REHIDE_EVERY = 4   # 锁定时每 N 次轮询重新隐藏任务栏（防 explorer 重启后复现）
_HEAL_EVERY = 5     # 未锁定时每 N 次轮询自愈：恢复任务栏/解除鼠标限制


def _release_lock(state):
    """解除锁定与全部加固：恢复任务栏、解除鼠标限制、卸载键盘钩子、销毁锁窗。"""
    try:
        winlock.show_taskbar()
    except Exception:
        pass
    try:
        winlock.unclip()
    except Exception:
        pass
    try:
        winlock.stop_keyboard_block()
    except Exception:
        pass
    if state.get("frame") is not None:
        try:
            state["frame"].destroy()
        except Exception:
            pass
        state["frame"] = None


def _unlock(top, state, entry, hint):
    pwd = entry.get()
    cfg = policy.load()
    if policy.password_ok(cfg, pwd):
        try:
            req = {"ts": time.time(), "minutes": int(cfg.get("extra_minutes_per_unlock", 30))}
            util.write_json(os.path.join(paths.state_dir(), "extra_req.json"), req)
            logger.info("密码正确，申请加时")
        except Exception as e:
            logger.error(f"加时申请失败: {e}")
        enforcer_clear_lock()
        _release_lock(state)
    else:
        entry.delete(0, "end")
        hint.configure(text="密码错误，请重试")


def enforcer_clear_lock():
    """解锁：清除锁定标志（若仍超配额，主控几秒后会重新锁定）。"""
    from core import enforcer
    enforcer.clear_lock()


def _show_lock(root, flag, state):
    top = tk.Toplevel(root)
    top.attributes("-fullscreen", True)
    top.attributes("-topmost", True)
    top.configure(bg="#0e0e16")
    try:
        top.grab_set()
    except Exception:
        pass
    top.focus_force()
    top.protocol("WM_DELETE_WINDOW", lambda: None)  # 禁止 Alt+F4 / 关闭按钮
    info = tk.Label(top, text="", font=("Microsoft YaHei", 18), fg="#e8e8f0",
                    bg="#0e0e16", wraplength=900)
    info.pack(pady=(130, 8))
    count = tk.Label(top, text="", font=("Microsoft YaHei", 13), fg="#8a9bb0", bg="#0e0e16")
    count.pack(pady=6)
    hint = tk.Label(top, text="输入家长密码解锁（解锁将增加当日配额）", font=("Microsoft YaHei", 11),
                    fg="#6a7a90", bg="#0e0e16")
    hint.pack(pady=24)
    entry = tk.Entry(top, show="*", font=("Microsoft YaHei", 15), width=22, justify="center")
    entry.pack(pady=6)
    btn = tk.Button(top, text="解锁", font=("Microsoft YaHei", 12), bg="#2a4a6a", fg="white",
                    command=lambda: _unlock(top, state, entry, hint))
    btn.pack(pady=10)
    entry.bind("<Return>", lambda e: _unlock(top, state, entry, hint))
    entry.focus_set()
    state["frame"] = top
    state["entry"] = entry
    state["info"] = info
    state["count"] = count
    # 用量信息
    try:
        from datetime import datetime
        u = util.read_json(paths.usage_path(), {})
        used = float(u.get("used", 0))
        extra = float(u.get("extra", 0))
        quota = policy.quota_for(policy.load(), datetime.now())
        state["usage_text"] = f"已用 {int(used)} 分钟 / 配额 {int(quota)} 分钟（含加时 {int(extra)}）"
    except Exception:
        state["usage_text"] = ""
    # ---- 锁屏加固：隐藏任务栏 + 限制鼠标 + 屏蔽逃生键 + 置顶 ----
    try:
        state["hwnd"] = int(top.winfo_id())
        winlock.hide_taskbar()
        winlock.clip_to_window(state["hwnd"])
        if not winlock.start_keyboard_block():
            logger.warn("键盘钩子安装失败（逃生键屏蔽未生效）")
        winlock.force_topmost(state["hwnd"])
        logger.info("锁屏加固已启用（任务栏隐藏/鼠标限制/键位屏蔽/置顶）")
    except Exception as e:
        logger.error(f"锁屏加固失败: {e}")


def main():
    if not util.single_instance("lockscreen"):
        if util.launched_by_user():
            util.notify_ui("TimeGuard", "锁屏服务已在运行（无需重复启动）。")
        return
    if util.launched_by_user():
        # 用户直接双击启动：清除可能残留的退出标记
        try:
            if os.path.exists(paths.quit_flag_path()):
                os.remove(paths.quit_flag_path())
        except OSError:
            pass
    util.write_text(paths.service_pid_path("lockscreen"), str(os.getpid()))
    logger.info(f"lockscreen 启动 pid={os.getpid()}")
    try:
        root = tk.Tk()
    except Exception as e:
        logger.error(f"无法创建锁定界面（无桌面会话？）: {e}")
        return
    root.withdraw()
    state = {"frame": None, "entry": None, "info": None, "count": None,
             "usage_text": "", "hwnd": None}
    tick = [0]

    def poll():
        tick[0] += 1
        if util.quit_flag_active():
            root.destroy()
            return
        flag = util.read_json(paths.lock_flag_path(), None)
        if flag and time.time() < float(flag.get("until", 0)):
            if state["frame"] is None:
                _show_lock(root, flag, state)
            if state["frame"] is not None:
                left = int(float(flag.get("until", 0)) - time.time())
                h, m = divmod(left // 60, 60)
                reason = str(flag.get("reason", "已锁定"))
                try:
                    state["info"].configure(text=f"{reason}\n剩余 {h} 小时 {m} 分钟")
                    state["count"].configure(text=state["usage_text"])
                    # 持续压制：防其它窗口盖过锁屏、防鼠标被移出锁定区域
                    winlock.force_topmost(state["hwnd"])
                    winlock.clip_to_window(state["hwnd"])
                    if tick[0] % _REHIDE_EVERY == 0:
                        winlock.hide_taskbar()
                except Exception:
                    pass
        else:
            if state["frame"] is not None:
                _release_lock(state)
                logger.info("锁定解除，加固已释放")
            elif tick[0] % _HEAL_EVERY == 0:
                # 自愈：进程被强杀后重启 / 异常退出时，确保任务栏与鼠标不被残留状态影响
                try:
                    winlock.show_taskbar()
                    winlock.unclip()
                    winlock.stop_keyboard_block()
                except Exception:
                    pass
        root.after(_POLL_MS, poll)

    root.after(_POLL_MS, poll)
    try:
        root.mainloop()
    finally:
        _release_lock(state)
        logger.info("lockscreen 退出")


if __name__ == "__main__":
    main()
