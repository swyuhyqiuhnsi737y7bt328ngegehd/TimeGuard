"""家长管理界面 admin.exe：策略编辑、立即锁定、加时解锁、卸载。"""
import glob
import json
import os
import shutil
import subprocess
import time
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

from core import policy
from share import logger, paths, util


def _save_policy(cfg):
    # 注意：policy.json 被 core/fileguard 占用（禁止删除），只能就地覆写，不能用 rename
    with open(paths.policy_path(), "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)


def _ask_password(root, cfg) -> bool:
    if not cfg.get("parent_password_hash"):
        return True
    pwd = simpledialog.askstring("家长验证", "请输入家长密码：", show="*", parent=root)
    if pwd is None:
        return False
    if policy.password_ok(cfg, pwd):
        return True
    messagebox.showerror("TimeGuard", "密码错误", parent=root)
    return False


def _set_password(root, cfg):
    p1 = simpledialog.askstring("设置家长密码", "请输入新密码（至少 4 位）：", show="*", parent=root)
    if p1 is None:
        return False
    if len(p1) < 4:
        messagebox.showerror("TimeGuard", "密码至少 4 位", parent=root)
        return False
    p2 = simpledialog.askstring("设置家长密码", "请再次输入新密码：", show="*", parent=root)
    if p1 != p2:
        messagebox.showerror("TimeGuard", "两次输入不一致", parent=root)
        return False
    cfg["parent_password_hash"] = util.sha256_hex(p1)
    return True


def _remove_run_key():
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                           r"Software\Microsoft\Windows\CurrentVersion\Run", 0,
                           winreg.KEY_SET_VALUE)
        try:
            winreg.DeleteValue(k, "TimeGuard")
        except FileNotFoundError:
            pass
        k.Close()
    except Exception:
        pass


def _uninstall(root):
    if not messagebox.askyesno("卸载 TimeGuard", "将停止所有 TimeGuard 进程并移除开机自启动，确定？",
                               parent=root):
        return
    try:
        util.write_quit_flag()
    except OSError:
        pass
    # 先等守望者自行退出（它们看到退出标记后 3 秒内退出，不会互相拉起）
    time.sleep(3)
    # 按“安装目录下所有进程”反复清理，直到全部死亡（不依赖注册表，覆盖随机名副本）
    for _ in range(8):
        procs = [p for p in util.processes_under(paths.app_root()) if p[0] != os.getpid()]
        if not procs:
            break
        util.kill_pids([p[0] for p in procs])
        time.sleep(2)
    procs = [p for p in util.processes_under(paths.app_root()) if p[0] != os.getpid()]
    if procs:
        logger.warn(f"卸载时仍有 {len(procs)} 个进程未停止: {procs}")
    _remove_run_key()
    # 确认进程全部停止后，才清理退出标记（防止残留进程因标记消失而复活互相拉起）
    try:
        os.remove(paths.quit_flag_path())
    except OSError:
        pass
    removed = 0
    root_dir = paths.app_root()
    for f in (glob.glob(os.path.join(root_dir, "*.exe")) +
              glob.glob(os.path.join(root_dir, "*.dll")) +
              [os.path.join(root_dir, "config"), os.path.join(root_dir, "state")]):
        try:
            if os.path.isdir(f):
                shutil.rmtree(f, ignore_errors=True)
            else:
                os.remove(f)
            removed += 1
        except OSError:
            pass
    messagebox.showinfo("卸载完成",
                        f"已停止所有进程并移除自启动项（清理 {removed} 项）。\n"
                        "本窗口(admin.exe)正在运行无法自删，关闭本窗口后手动删除安装目录即可。",
                        parent=root)


def main():
    if not util.single_instance("admin"):
        util.notify_ui("TimeGuard", "管理界面已在运行（请查看已打开的窗口）。")
        return
    cfg = policy.load()
    root = tk.Tk()
    root.title("TimeGuard 家长控制")
    root.geometry("620x760")
    root.configure(bg="#f5f6fa")
    if not _ask_password(root, cfg):
        root.destroy()
        return

    # 首次使用：强制设置密码
    if not cfg.get("parent_password_hash"):
        messagebox.showinfo("TimeGuard", "首次使用，请先设置家长密码（密码为空时限制不生效）。", parent=root)
        if not _set_password(root, cfg):
            root.destroy()
            return
        _save_policy(cfg)
        cfg = policy.load()

    pad = {"padx": 12, "pady": 6}
    frm = ttk.Frame(root)
    frm.pack(fill="both", expand=True, padx=16, pady=10)

    # 配额
    r = 0
    ttk.Label(frm, text="每日配额（分钟）").grid(row=r, column=0, sticky="w", **pad)
    q_wd = tk.IntVar(value=int(cfg["daily_quota"].get("weekday", 120)))
    q_we = tk.IntVar(value=int(cfg["daily_quota"].get("weekend", 240)))
    ttk.Label(frm, text="工作日:").grid(row=r, column=1, sticky="e")
    ttk.Spinbox(frm, from_=0, to=1440, textvariable=q_wd, width=6).grid(row=r, column=2, **pad)
    ttk.Label(frm, text="周末:").grid(row=r, column=3, sticky="e")
    ttk.Spinbox(frm, from_=0, to=1440, textvariable=q_we, width=6).grid(row=r, column=4, **pad)

    # 禁止时段（最多 3 组）
    r += 1
    ttk.Label(frm, text="禁止使用时段（HH:MM，可跨午夜，如 22:30-07:30）").grid(
        row=r, column=0, columnspan=5, sticky="w", **pad)
    wins = cfg.get("forbidden_windows", []) or []
    win_vars = []
    for i in range(3):
        r += 1
        on = tk.BooleanVar(value=i < len(wins))
        st = tk.StringVar(value=wins[i]["start"] if i < len(wins) else "22:30")
        en = tk.StringVar(value=wins[i]["end"] if i < len(wins) else "07:30")
        ttk.Checkbutton(frm, variable=on, text=f"时段{i + 1}").grid(row=r, column=0, sticky="w", **pad)
        ttk.Entry(frm, textvariable=st, width=8).grid(row=r, column=2, **pad)
        ttk.Label(frm, text="至").grid(row=r, column=3)
        ttk.Entry(frm, textvariable=en, width=8).grid(row=r, column=4, **pad)
        win_vars.append((on, st, en))

    # 执行动作
    r += 1
    ttk.Label(frm, text="超时后执行动作").grid(row=r, column=0, sticky="w", **pad)
    act = tk.StringVar(value=cfg.get("enforce_action", "lock"))
    acts = [("lock", "锁定屏幕"), ("kill", "结束指定进程"), ("logoff", "注销当前用户"), ("shutdown", "定时关机")]
    for i, (val, lab) in enumerate(acts):
        ttk.Radiobutton(frm, text=lab, value=val, variable=act).grid(row=r, column=i + 1, sticky="w", **pad)

    # 结束进程列表
    r += 1
    ttk.Label(frm, text="action=kill 时结束的进程名（一行一个，如 game.exe）").grid(
        row=r, column=0, columnspan=5, sticky="w", **pad)
    r += 1
    kill_txt = tk.Text(frm, width=56, height=4)
    kill_txt.insert("1.0", "\n".join(str(x) for x in cfg.get("kill_processes", []) or []))
    kill_txt.grid(row=r, column=0, columnspan=5, sticky="we", padx=12)

    # 其它参数
    r += 1
    remind = tk.IntVar(value=int(cfg.get("remind_minutes", 5)))
    extra = tk.IntVar(value=int(cfg.get("extra_minutes_per_unlock", 30)))
    pen = tk.IntVar(value=int(cfg.get("tamper_penalty_minutes", 60)))
    ttk.Label(frm, text="提前提醒（分钟）:").grid(row=r, column=0, sticky="w", **pad)
    ttk.Spinbox(frm, from_=0, to=120, textvariable=remind, width=6).grid(row=r, column=1, sticky="w", **pad)
    ttk.Label(frm, text="解锁加时（分钟）:").grid(row=r, column=2, sticky="e")
    ttk.Spinbox(frm, from_=5, to=240, textvariable=extra, width=6).grid(row=r, column=3, sticky="w", **pad)
    r += 1
    ttk.Label(frm, text="改时间惩罚（分钟）:").grid(row=r, column=0, sticky="w", **pad)
    ttk.Spinbox(frm, from_=0, to=600, textvariable=pen, width=6).grid(row=r, column=1, sticky="w", **pad)

    # 按钮
    r += 1
    btns = ttk.Frame(frm)
    btns.grid(row=r, column=0, columnspan=5, pady=16)

    def save():
        ncfg = policy.load()
        ncfg["daily_quota"] = {"weekday": max(0, q_wd.get()), "weekend": max(0, q_we.get())}
        ws = []
        for on, st, en in win_vars:
            if on.get():
                ws.append({"start": st.get().strip(), "end": en.get().strip()})
        ncfg["forbidden_windows"] = ws
        ncfg["enforce_action"] = act.get()
        ncfg["kill_processes"] = [x.strip() for x in kill_txt.get("1.0", "end").splitlines() if x.strip()]
        ncfg["remind_minutes"] = max(0, remind.get())
        ncfg["extra_minutes_per_unlock"] = max(5, extra.get())
        ncfg["tamper_penalty_minutes"] = max(0, pen.get())
        try:
            _save_policy(ncfg)
            messagebox.showinfo("TimeGuard", "设置已保存，立即生效。", parent=root)
        except Exception as e:
            messagebox.showerror("TimeGuard", f"保存失败：{e}", parent=root)

    def lock_now():
        from core import enforcer
        enforcer.set_lock("家长手动锁定", time.time() + 8 * 3600)
        enforcer.ensure_lockscreen()
        messagebox.showinfo("TimeGuard", "已锁定。", parent=root)

    def extra_time():
        util.write_json(os.path.join(paths.state_dir(), "extra_req.json"),
                        {"ts": time.time(), "minutes": max(5, extra.get())})
        from core import enforcer
        enforcer.clear_lock()
        messagebox.showinfo("TimeGuard", f"已加时 {max(5, extra.get())} 分钟并解除锁定。", parent=root)

    def cancel_shutdown():
        try:
            subprocess.run(["shutdown", "/a"], capture_output=True, timeout=10)
            messagebox.showinfo("TimeGuard", "已取消定时关机。", parent=root)
        except Exception as e:
            messagebox.showerror("TimeGuard", f"取消失败：{e}", parent=root)

    def change_pwd():
        if _set_password(root, cfg):
            _save_policy(cfg)
            messagebox.showinfo("TimeGuard", "密码已更新。", parent=root)

    ttk.Button(btns, text="保存设置", command=save).pack(side="left", padx=6)
    ttk.Button(btns, text="立即锁定", command=lock_now).pack(side="left", padx=6)
    ttk.Button(btns, text="加时并解锁", command=extra_time).pack(side="left", padx=6)
    ttk.Button(btns, text="取消定时关机", command=cancel_shutdown).pack(side="left", padx=6)
    ttk.Button(btns, text="修改密码", command=change_pwd).pack(side="left", padx=6)
    ttk.Button(btns, text="卸载并退出", command=lambda: _uninstall(root)).pack(side="left", padx=6)

    root.mainloop()
    logger.info("admin 退出")


if __name__ == "__main__":
    main()
