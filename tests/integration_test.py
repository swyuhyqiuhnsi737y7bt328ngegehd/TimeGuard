"""守望进程集成测试（源码模式，安全：默认策略无密码 = 不执行任何限制）。

流程：起 3 个守望者 -> 杀其中一个 -> 验证被同伴拉起 -> 验证 core 被拉起 ->
      写 quit.flag -> 验证全部退出 -> 清理 state 目录。
运行: python tests/integration_test.py
"""
import glob
import os
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
STATE = os.path.join(ROOT, "state")
os.makedirs(STATE, exist_ok=True)


def env_with_path():
    env = dict(os.environ)
    env["PYTHONPATH"] = SRC
    return env


def read_entry(token):
    p = os.path.join(STATE, f"guard_{token}.json")
    try:
        with open(p, encoding="utf-8") as f:
            import json
            return json.load(f)
    except Exception:
        return None


def alive(pid, basename=None):
    try:
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32")
        h = k32.OpenProcess(0x1000, False, int(pid))
        if not h:
            return False
        k32.CloseHandle(h)
        return True
    except Exception:
        return False


def stop_all():
    """写退出标记，等所有进程自行退出；再强清 fileguard 与可能漏网的源码 core。"""
    try:
        with open(os.path.join(STATE, "quit.flag"), "w") as f:
            f.write(str(time.time()))
    except OSError:
        pass
    time.sleep(7)
    try:
        subprocess.run(["taskkill", "/F", "/IM", "fileguard.exe"],
                       capture_output=True, timeout=10)
    except Exception:
        pass
    try:
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                        "$p = Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
                        "Where-Object { $_.CommandLine -like '*core.controller*' -or "
                        "$_.CommandLine -like '*guard.watchdog*' -or "
                        "$_.CommandLine -like '*lock.lockscreen*' }; "
                        "foreach ($x in $p) { Stop-Process -Id $x.ProcessId -Force -ErrorAction SilentlyContinue }"],
                       capture_output=True, timeout=60)
    except Exception:
        pass
    time.sleep(1)


def cleanup():
    stop_all()
    for f in glob.glob(os.path.join(STATE, "*")):
        try:
            if os.path.isdir(f):
                import shutil
                shutil.rmtree(f, ignore_errors=True)
            else:
                os.remove(f)
        except OSError:
            pass


def wait_for(pred, timeout, desc):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(1)
    print(f"FAIL: 等待超时 - {desc}")
    return False


def main():
    print("== 守望进程集成测试（源码模式）==")
    cleanup()
    toks = ["t1aaa", "t2bbb", "t3ccc"]
    for t in toks:
        subprocess.Popen([sys.executable, "-m", "guard.watchdog", "--token", t],
                         env=env_with_path(), creationflags=0x08000000)
    # 1) 3 个守望者都注册
    def all_registered():
        return all(read_entry(t) and alive(read_entry(t)["pid"], "python.exe") for t in toks)
    if not wait_for(all_registered, 40, "3 个守望者注册"):
        cleanup(); return 1
    print("PASS: 3 个守望者已注册并存活")
    # 2) core 应被守望者拉起（源码模式：core.pid 文件 + 存活）
    def core_up():
        try:
            pid = int(open(os.path.join(STATE, "core.pid")).read())
        except Exception:
            return False
        return alive(pid, "python.exe")
    if not wait_for(core_up, 40, "core 被拉起"):
        cleanup(); return 1
    print("PASS: core 被守望者拉起（pid 文件存活）")
    # 3) 杀掉一个守望者，应被同伴拉起（源码模式 = 新随机 token 的替代者；
    #    冷冻模式 = 同名随机 exe 副本复活）。验证存活守望者总数恢复为 3。
    e1 = read_entry(toks[0])
    old_pid = int(e1["pid"])
    os.kill(old_pid, signal.SIGTERM)

    def live_guardian_count():
        n = 0
        for f in glob.glob(os.path.join(STATE, "guard_*.json")):
            tok = os.path.basename(f)[len("guard_"):-len(".json")]
            e = read_entry(tok)
            if e and alive(e["pid"], "python.exe"):
                n += 1
        return n

    def new_token_appeared():
        for f in glob.glob(os.path.join(STATE, "guard_*.json")):
            tok = os.path.basename(f)[len("guard_"):-len(".json")]
            if tok not in toks:
                e = read_entry(tok)
                if e and alive(e["pid"], "python.exe"):
                    return True
        return False

    if not wait_for(lambda: live_guardian_count() >= 3, 45, "守望者总数恢复到 3"):
        cleanup(); return 1
    ok_new = wait_for(new_token_appeared, 30, "出现新 token 的替代守望者")
    print(f"PASS: 守望者 {toks[0]} (pid {old_pid}) 被杀后存活数恢复为 3"
          f"{'，且出现新替代者' if ok_new else ''}")
    # 4) 写退出标记，全部应退出
    # 注意：本机进程创建频繁、PID 可能被快速复用，pid 判断不可靠，
    # 因此用“命令行扫描”做权威校验（本测试环境 python 高度活跃）。
    with open(os.path.join(STATE, "quit.flag"), "w") as f:
        f.write(str(time.time()))

    def guard_proc_count():
        try:
            out = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command",
                 "(Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
                 "Where-Object { $_.CommandLine -like '*guard.watchdog*' -or "
                 "$_.CommandLine -like '*core.controller*' -or "
                 "$_.CommandLine -like '*lock.lockscreen*' }).Count"],
                capture_output=True, text=True, timeout=60)
            return int(out.stdout.strip() or "0")
        except Exception:
            return -1

    if not wait_for(lambda: guard_proc_count() == 0, 30, "所有进程退出"):
        cleanup(); return 1
    print("PASS: 写入 quit.flag 后所有守望者/core/lockscreen 退出")
    stop_all()
    for f in glob.glob(os.path.join(STATE, "*")):
        try:
            if os.path.isdir(f):
                import shutil
                shutil.rmtree(f, ignore_errors=True)
            else:
                os.remove(f)
        except OSError:
            pass
    print("集成测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
