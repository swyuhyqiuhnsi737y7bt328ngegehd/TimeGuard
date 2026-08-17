"""家长管理界面 admin.exe：策略编辑、立即锁定、加时解锁、卸载。

支持 --quit 模式：由托盘“退出程序”拉起，独立进程弹出家长密码确认框
（避免在托盘线程里创建 Tk 导致输入异常/狂点卡退）。
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

from core import policy
from share import logger, paths, util


def _save_policy(cfg):
    # 注意：policy.json 被 core/fileguard 占用（禁止删除），只能就地覆写，不能用 rename
    from share import configmac
    key = configmac.get_key() or configmac.create_key()
    signed = configmac.sign(cfg, key)  # 带完整性签名，防止被直接改文件绕过
    with open(paths.policy_path(), "w", encoding="utf-8") as f:
        json.dump(signed, f, ensure_ascii=False, indent=2)
    configmac.save_backup(signed)


def _prompt_password(root, title, prompt):
    """模态密码输入框：可聚焦、可输入、文字自动换行。返回输入内容或 None（取消）。"""
    dlg = tk.Toplevel(root)
    dlg.title(title)
    dlg.attributes("-topmost", True)
    dlg.configure(bg="#f5f6fa")
    try:
        root.update_idletasks()  # 确保主窗口尺寸已计算，避免对话框位置错乱
    except Exception:
        pass
    w, h = 380, 190
    x = root.winfo_rootx() + max(0, (root.winfo_width() - w) // 2)
    y = root.winfo_rooty() + max(0, (root.winfo_height() - h) // 2)
    dlg.geometry(f"{w}x{h}+{x}+{y}")
    dlg.resizable(False, False)
    dlg.transient(root)
    result = {"val": None}
    tk.Label(dlg, text=prompt, font=("Microsoft YaHei", 10), bg="#f5f6fa", fg="#333",
             wraplength=340, justify="left").pack(padx=18, pady=(16, 8))
    entry = tk.Entry(dlg, show="*", font=("Microsoft YaHei", 12), width=24)
    entry.pack(padx=18, pady=6)

    def ok():
        result["val"] = entry.get()
        dlg.destroy()

    def cc():
        dlg.destroy()

    btns = tk.Frame(dlg, bg="#f5f6fa")
    btns.pack(pady=8)
    tk.Button(btns, text="确定", width=8, command=ok).pack(side="left", padx=8)
    tk.Button(btns, text="取消", width=8, command=cc).pack(side="left", padx=8)
    dlg.bind("<Return>", lambda e: ok())
    dlg.bind("<Escape>", lambda e: cc())
    dlg.protocol("WM_DELETE_WINDOW", cc)
    try:
        dlg.grab_set()
    except Exception:
        pass
    entry.focus_set()
    dlg.wait_window()
    return result["val"]


def _ask_password(root, cfg) -> bool:
    if not cfg.get("parent_password_hash"):
        return True
    pwd = _prompt_password(root, "家长验证", "请输入家长密码：")
    if pwd is None:
        return False
    if policy.password_ok(cfg, pwd):
        return True
    messagebox.showerror("TimeGuard", "密码错误", parent=root)
    return False


def _set_password(root, cfg):
    p1 = _prompt_password(root, "设置家长密码", "请输入新密码（至少 4 位）：")
    if p1 is None:
        return False
    if len(p1) < 4:
        messagebox.showerror("TimeGuard", "密码至少 4 位", parent=root)
        return False
    p2 = _prompt_password(root, "设置家长密码", "请再次输入新密码：")
    if p1 != p2:
        messagebox.showerror("TimeGuard", "两次输入不一致", parent=root)
        return False
    cfg["parent_password_hash"] = util.sha256_hex(p1)
    return True


def _ring_running() -> bool:
    """保护环（core）是否在运行。"""
    try:
        if util.find_pid_by_name("core.exe"):
            return True
        pid = int(util.read_text(paths.service_pid_path("core"), "0") or "0")
        return bool(pid) and util.process_alive(pid, "python.exe")
    except Exception:
        return False


def _start_ring():
    """启动 fileguard + core（守望者会自动补全）。

    显式启动 = 撤销之前的退出意图：清除残留的 quit.flag，
    否则刚退出过（标记仍新鲜）时 core 一启动就会立即退出。
    """
    try:
        if os.path.exists(paths.quit_flag_path()):
            os.remove(paths.quit_flag_path())
    except OSError:
        pass
    try:
        fg = os.path.join(paths.app_root(), "fileguard.exe")
        if os.path.exists(fg):
            util.spawn([fg])
        util.spawn_service("core")
    except Exception as e:
        logger.error(f"启动保护环失败: {e}")


def _cleanup_quit_flag():
    """退出流程收尾：等所有进程退出后清除退出标记，避免下次启动被旧标记挡住。"""
    time.sleep(10)
    try:
        os.remove(paths.quit_flag_path())
    except OSError:
        pass


def _quit_mode():
    """--quit 模式：独立进程的家长密码确认框（由托盘菜单拉起）。"""
    if not util.single_instance("admin_quit"):
        return  # 已有一个确认框在显示，忽略重复点击
    try:
        top = tk.Tk()
        top.title("退出 TimeGuard")
        top.attributes("-topmost", True)
        top.configure(bg="#f5f6fa")
        w, h = 400, 220
        sw, sh = top.winfo_screenwidth(), top.winfo_screenheight()
        top.geometry(f"{w}x{h}+{max(0, (sw - w) // 2)}+{max(0, (sh - h) // 2)}")
        top.resizable(False, False)
        tk.Label(top, text="输入家长密码确认退出\n（退出后保护停止，需重新启动才能恢复）",
                 font=("Microsoft YaHei", 10), bg="#f5f6fa", fg="#333",
                 justify="left").pack(padx=20, pady=(18, 8))
        entry = tk.Entry(top, show="*", font=("Microsoft YaHei", 12), width=24)
        entry.pack(padx=20, pady=6)
        err = tk.Label(top, text="", font=("Microsoft YaHei", 9), fg="#c33", bg="#f5f6fa")
        err.pack()
        btns = tk.Frame(top, bg="#f5f6fa")
        btns.pack(pady=10)

        def submit():
            if policy.password_ok(policy.load(), entry.get()):
                util.write_quit_flag()
                threading.Thread(target=_cleanup_quit_flag, daemon=True).start()
                top.destroy()
            else:
                entry.delete(0, "end")
                err.configure(text="密码错误")

        def cancel():
            top.destroy()

        tk.Button(btns, text="确定", width=8, command=submit).pack(side="left", padx=8)
        tk.Button(btns, text="取消", width=8, command=cancel).pack(side="left", padx=8)
        top.bind("<Return>", lambda e: submit())
        top.bind("<Escape>", lambda e: cancel())
        top.protocol("WM_DELETE_WINDOW", cancel)
        try:
            top.grab_set()
        except Exception:
            pass
        top.focus_force()
        entry.focus_set()
        top.mainloop()
    except Exception as e:
        logger.error(f"退出确认窗口异常: {e}")


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
    # 清除配置保护密钥与备份（文件 + 注册表）
    try:
        from share import configmac
        configmac.remove_all()
    except Exception:
        pass
    # 清除系统功能限制（不残留限制策略）
    try:
        from share import policies as _pol
        _pol.clear_all()
    except Exception:
        pass
    # 确认进程全部停止后，才清理退出标记（防止残留进程因标记消失而复活互相拉起）
    try:
        os.remove(paths.quit_flag_path())
    except OSError:
        pass
    # 确保配置（含家长密码）被清除：即使删除失败，也先就地清空密码哈希
    pol = os.path.join(paths.app_root(), "config", "policy.json")
    try:
        if os.path.exists(pol):
            with open(pol, "w", encoding="utf-8") as f:
                f.write(json.dumps({"parent_password_hash": "", "version": 1}))
    except Exception:
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
    if "--quit" in sys.argv:
        _quit_mode()
        return
    if not util.single_instance("admin"):
        util.notify_ui("TimeGuard", "管理界面已在运行（请查看已打开的窗口）。")
        return
    # 打开前先检查配置是否被外部篡改（policy.load 会自动恢复）
    from share import configmac
    raw = util.read_json(paths.policy_path(), None)
    if configmac.initialized() and (not isinstance(raw, dict)
                                    or not configmac.verify(raw, configmac.get_key())):
        messagebox.showwarning("TimeGuard", "检测到配置曾被外部修改，已自动恢复家长设置。",
                               parent=None)
    cfg = policy.load()
    root = tk.Tk()
    root.title("TimeGuard 家长控制")
    root.geometry("620x880")
    root.configure(bg="#f5f6fa")
    if not _ask_password(root, cfg):
        root.destroy()
        return

    # 首次使用：提示设置密码，但不强制（取消也能继续浏览设置）
    if not cfg.get("parent_password_hash"):
        messagebox.showinfo("TimeGuard",
                            "尚未设置家长密码，时间限制不会生效。\n"
                            "点击下方“修改密码”按钮即可设置。", parent=root)

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

    # 系统功能限制
    r += 1
    lf = ttk.LabelFrame(frm, text="系统功能限制（仅当前 Windows 账户，勾选后保存立即生效）")
    lf.grid(row=r, column=0, columnspan=5, sticky="we", padx=12, pady=6)
    from share import policies as _pol
    restr_keys = list(_pol.RESTRICTIONS.keys())
    restr_vars = {}
    for i, key in enumerate(restr_keys):
        var = tk.BooleanVar(value=key in (cfg.get("system_restrictions", []) or []))
        ttk.Checkbutton(lf, text=_pol.display_name(key), variable=var).grid(
            row=i // 3, column=i % 3, sticky="w", padx=10, pady=4)
        restr_vars[key] = var
    ttk.Label(lf, text="提示：禁用命令提示符/注册表编辑器后，构建脚本(bat)与 regedit 也会被禁，需取消勾选后恢复。",
              font=("Microsoft YaHei", 8), foreground="#888").grid(
        row=(len(restr_keys) + 2) // 3, column=0, columnspan=3, sticky="w",
        padx=10, pady=(2, 6))

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
        ncfg["system_restrictions"] = [k for k, v in restr_vars.items() if v.get()]
        try:
            _save_policy(ncfg)
        except Exception as e:
            messagebox.showerror("TimeGuard", f"保存失败：{e}", parent=root)
            return
        # 立即应用系统限制（不等控制器轮询）
        try:
            _pol.apply_restrictions(ncfg.get("system_restrictions", []))
        except Exception:
            pass
        if not _ring_running():
            if messagebox.askyesno("TimeGuard",
                                   "主控程序（core）未在运行，时间限制不会生效。\n是否立即启动？",
                                   parent=root):
                _start_ring()
                messagebox.showinfo("TimeGuard", "已启动。请稍候在任务栏托盘（^ 溢出区）查看图标。",
                                    parent=root)
        messagebox.showinfo("TimeGuard", "设置已保存，立即生效。", parent=root)

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
