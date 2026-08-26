"""系统功能限制执行器测试（进程拦截 + Win+R 钩子）。

不依赖注册表；结束进程的测试只针对本测试自己拉起的子进程，
不会影响机器上其它同名进程。
运行: python tests/test_restrictor.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from share import restrictor, util  # noqa: E402


def main():
    # 1) 映射完整性
    assert len(restrictor.RESTRICTIONS) >= 4, restrictor.RESTRICTIONS
    for key, (kind, targets, label) in restrictor.RESTRICTIONS.items():
        assert kind in ("proc", "hotkey"), key
        assert label, key
        assert targets, key
        assert restrictor.display_name(key) == label, key
    assert restrictor.display_name("not_a_real_key") == "not_a_real_key"
    print("PASS: 限制项映射完整（含 display_name）")

    # 2) 目标解析
    procs = restrictor.process_targets(["disable_cmd", "disable_taskmgr"])
    assert "cmd.exe" in procs and "powershell.exe" in procs and "taskmgr.exe" in procs
    assert "regedit.exe" not in procs
    assert restrictor.process_targets([]) == set()
    hot = restrictor.hotkey_targets(["disable_run"])
    assert hot == {"win+r"}
    assert restrictor.hotkey_targets([]) == set()
    print("PASS: 进程/热键目标解析正确")

    # 3) 结束被禁进程链路（只杀本测试拉起的子进程）
    victim = subprocess.Popen(["cmd.exe", "/c", "ping -n 30 127.0.0.1 >nul"],
                              creationflags=subprocess.CREATE_NO_WINDOW)
    pid = victim.pid
    try:
        found = util.find_pids_by_name("cmd.exe")
        assert pid in found, f"未找到测试子进程 {pid}，实际 {found}"
        n = util.kill_pids([pid])
        assert n >= 1, "kill_pids 未报告成功"
        time.sleep(1.2)
        assert not util.process_alive(pid), "被禁进程未被结束"
        print("PASS: 被禁进程可被结束（枚举 + taskkill 链路正常）")
    finally:
        try:
            victim.kill()
        except Exception:
            pass

    # 4) Win+R 钩子安装/卸载
    from lock import winlock
    assert winlock.block_run_hotkey(True), "Win+R 钩子安装失败"
    assert winlock.run_hook_active()
    assert winlock.block_run_hotkey(False)
    time.sleep(0.3)
    assert not winlock.run_hook_active(), "Win+R 钩子未卸载"
    print("PASS: Win+R 拦截钩子安装/卸载正常")

    print("restrictor 测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
