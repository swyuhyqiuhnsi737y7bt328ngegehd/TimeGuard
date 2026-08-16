"""锁定屏幕进程 lockscreen.exe：常驻；出现 lock.flag 时全屏锁定；家长密码解锁（自动加时）。"""
import os
import time
import tkinter as tk

from core import policy
from share import logger, paths, util

_POLL_MS = 1000


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
        top.destroy()
        state["frame"] = None
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
    state["frame"] = top
    state["entry"] = entry
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
    entry.focus_set()
    state["info"] = info
    state["count"] = count


def main():
    if not util.single_instance("lockscreen"):
        return
    util.write_text(paths.service_pid_path("lockscreen"), str(os.getpid()))
    logger.info(f"lockscreen 启动 pid={os.getpid()}")
    try:
        root = tk.Tk()
    except Exception as e:
        logger.error(f"无法创建锁定界面（无桌面会话？）: {e}")
        return
    root.withdraw()
    state = {"frame": None, "entry": None, "usage_text": ""}

    def poll():
        if os.path.exists(paths.quit_flag_path()):
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
                    state["frame"].lift()
                    state["info"].configure(text=f"{reason}\n剩余 {h} 小时 {m} 分钟")
                    state["count"].configure(text=state["usage_text"])
                except Exception:
                    pass
        else:
            if state["frame"] is not None:
                try:
                    state["frame"].destroy()
                except Exception:
                    pass
                state["frame"] = None
        root.after(_POLL_MS, poll)

    root.after(_POLL_MS, poll)
    root.mainloop()
    logger.info("lockscreen 退出")


if __name__ == "__main__":
    main()
