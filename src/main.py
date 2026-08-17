"""TimeGuard 命令行入口。

用法：
  python src/main.py install     打包完成后安装到 dist 并启动（写开机自启动）
  python src/main.py uninstall   停止所有进程、移除自启动、清理文件
  python src/main.py dev         源码模式运行（开发调试）
  python src/main.py status      查看策略/用量/进程状态
  python src/main.py hashpwd <密码>   打印密码哈希（可手工写入 policy.json）
  python src/main.py resetpw     清空家长密码（忘记密码时用，需在项目目录运行）
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DIST = os.path.join(ROOT, "dist")
sys.path.insert(0, HERE)

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def _py_env():
    env = dict(os.environ)
    env["PYTHONPATH"] = HERE
    return env


def _spawn(cmd, env=None):
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        return subprocess.Popen(cmd, creationflags=flags, env=env, close_fds=True)
    except Exception as e:
        print("[错误] 启动失败:", cmd, e)
        return None


def cmd_install(args):
    core = os.path.join(DIST, "core.exe")
    if not os.path.exists(core):
        print(f"[错误] 未找到 {core}\n请先运行 scripts\\build_all.bat")
        return 1
    os.makedirs(os.path.join(DIST, "config"), exist_ok=True)
    dst = os.path.join(DIST, "config", "policy.json")
    if not os.path.exists(dst):
        src = os.path.join(ROOT, "config", "policy.json")
        if os.path.exists(src):
            shutil.copy2(src, dst)
            print("[配置] 已复制默认策略到", dst)
    os.makedirs(os.path.join(DIST, "state"), exist_ok=True)
    with open(os.path.join(DIST, "state", "installed.flag"), "w") as f:
        f.write("1")
    try:
        import winreg
        k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY)
        winreg.SetValueEx(k, "TimeGuard", 0, winreg.REG_SZ, core)
        winreg.CloseKey(k)
        print("[自启动] 已写入注册表 HKCU\\...\\Run\\TimeGuard =", core)
        print("[自启动] 位置：注册表（非计划任务/启动文件夹），登录 Windows 时自动运行")
    except Exception as e:
        print("[警告] 写入自启动失败:", e)
    fg = os.path.join(DIST, "fileguard.exe")
    if os.path.exists(fg):
        _spawn([fg])
        print("[启动] fileguard.exe（文件自锁 + 最后防线）")
    _spawn([core])
    print("[启动] core.exe（主控制，会自动拉起 3 个随机名守望副本）")
    print("完成。查看状态: python src\\main.py status")
    return 0


def cmd_uninstall(args):
    target = os.path.abspath(args.dir or DIST)
    state = os.path.join(target, "state")
    os.makedirs(state, exist_ok=True)
    qf = os.path.join(state, "quit.flag")
    try:
        with open(qf, "w") as f:
            f.write(str(time.time()))
        print("[退出] 已写入退出标记")
    except OSError as e:
        print("[警告] 写入退出标记失败:", e)
    from share import util as _util
    time.sleep(3)  # 给守望者一点时间自行退出（看到退出标记后不再互相拉起）
    # 按“目标目录下所有进程”反复清理，直到全部死亡（不依赖注册表，覆盖随机名副本）
    for _ in range(8):
        procs = _util.processes_under(target)
        if not procs:
            break
        _util.kill_pids([p[0] for p in procs])
        print(f"[停止] {len(procs)} 个进程")
        time.sleep(2)
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE)
        try:
            winreg.DeleteValue(k, "TimeGuard")
            print("[自启动] 已移除")
        except FileNotFoundError:
            print("[自启动] 无此项目")
        winreg.CloseKey(k)
    except Exception as e:
        print("[警告] 移除自启动失败:", e)
    # 确认进程全部停止后，才清理退出标记（防止残留进程因标记消失而复活）
    try:
        os.remove(qf)
    except OSError:
        pass
    # 确保配置（含家长密码）被清除：即使删除失败，也先就地清空密码哈希
    pol = os.path.join(target, "config", "policy.json")
    try:
        if os.path.exists(pol):
            with open(pol, "w", encoding="utf-8") as f:
                f.write(json.dumps({"parent_password_hash": "", "version": 1}))
    except Exception:
        pass
    removed = 0
    for f in (glob.glob(os.path.join(target, "*.exe")) +
              glob.glob(os.path.join(target, "*.dll")) +
              [os.path.join(target, "config"), os.path.join(target, "state")]):
        try:
            if os.path.isdir(f):
                shutil.rmtree(f, ignore_errors=True)
            else:
                os.remove(f)
            removed += 1
        except OSError:
            pass
    print(f"[清理] 完成（{removed} 项；正在运行的 exe 无法删除，请手动处理）")
    return 0


def cmd_dev(args):
    from share import paths
    fg = os.path.join(DIST, "fileguard.exe")
    if os.path.exists(fg):
        _spawn([fg])
        print("[启动] fileguard.exe")
    _spawn([sys.executable, "-m", "core.controller"], _py_env())
    print("[启动] core.controller（源码模式）")
    print("提示：状态查看 python src\\main.py status；退出可在 state 目录放 quit.flag")
    return 0


def cmd_status(args):
    from core import policy
    from share import paths, util
    print("== 策略 ==")
    cfg = policy.load()
    print("  每日配额:", cfg.get("daily_quota"),
          " 动作:", cfg.get("enforce_action"),
          " 禁止时段:", cfg.get("forbidden_windows"))
    print("  家长密码:", "已设置" if cfg.get("parent_password_hash") else "未设置（限制未启用）")
    print("  解锁加时:", cfg.get("extra_minutes_per_unlock"),
          " 提前提醒:", cfg.get("remind_minutes"),
          " 改时间惩罚:", cfg.get("tamper_penalty_minutes"))
    print("== 今日用量 ==", util.read_json(paths.usage_path(), {}))
    print("== 开机自启动（注册表）==")
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY)
        val = winreg.QueryValueEx(k, "TimeGuard")[0]
        print("  TimeGuard =", val)
        winreg.CloseKey(k)
    except Exception:
        print("  （未设置）")
    print("== 常驻进程 ==")
    for name in ["fileguard", "core", "lockscreen", "admin"]:
        pid = util.find_pid_by_name(name + ".exe")
        print(f"  {name}.exe:", f"运行中 pid={pid}" if pid else "未运行")
    for f in glob.glob(os.path.join(paths.state_dir(), "guard_*.json")):
        e = util.read_json(f, None)
        if e:
            base = "python.exe" if not paths.is_frozen() else None
            alive = util.process_alive(int(e.get("pid", 0)), base)
            print(f"  guardian[{e.get('token')}] pid={e.get('pid')} {'存活' if alive else '已死'}")
    print("== 状态目录 ==", paths.state_dir())
    return 0


def cmd_hashpwd(args):
    from share import util
    print(util.sha256_hex(args.password))
    return 0


def cmd_resetpw(args):
    target = os.path.join(DIST if args.target == "dist" else ROOT, "config", "policy.json")
    try:
        with open(target, encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:
        cfg = {}
    cfg["parent_password_hash"] = ""
    with open(target, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    print(f"已清空密码（{target}）。请用 admin.exe 重新设置。")
    return 0


def main():
    ap = argparse.ArgumentParser(description="TimeGuard 工具")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("install")
    p.set_defaults(fn=cmd_install)
    p = sub.add_parser("uninstall")
    p.add_argument("--dir", default=None, help="安装目录（默认 dist）")
    p.set_defaults(fn=cmd_uninstall)
    p = sub.add_parser("dev")
    p.set_defaults(fn=cmd_dev)
    p = sub.add_parser("status")
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("hashpwd")
    p.add_argument("password")
    p.set_defaults(fn=cmd_hashpwd)
    p = sub.add_parser("resetpw")
    p.add_argument("--target", choices=["dist", "src"], default="dist")
    p.set_defaults(fn=cmd_resetpw)
    args = ap.parse_args()
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()
