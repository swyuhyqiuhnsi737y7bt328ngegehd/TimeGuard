r"""孤儿化启动（util.spawn）测试：不依赖 cmd.exe，且不怕 cmd.exe 被封杀。

背景（实机 bug）：util.spawn 原先用 "cmd.exe /c start" 孵化进程，而 cmd.exe 属于
锁屏期间的封杀名单（restrictor.LOCKDOWN_TARGETS）—— 限制执行器每秒杀一次 cmd.exe。
于是【锁定期间任何 spawn 都会被连带杀掉】：托盘点"退出程序"拉不起密码确认窗口，
点了毫无反应，日志里也一行都不留。

修法：改用 CreateProcess 的 DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP 直接启动，
不经过 cmd.exe，同时仍保持"孤儿化"（不在调用方进程树里）。

运行: python tests/test_spawn.py
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from share import util  # noqa: E402

_TMP = tempfile.mkdtemp(prefix="tg_spawn_")


def test_spawn_starts_process():
    """spawn 必须真的把进程拉起来。"""
    marker = os.path.join(_TMP, "alive.txt")
    script = os.path.join(_TMP, "child.py")
    with open(script, "w", encoding="utf-8") as f:
        f.write("import sys, time\n"
                "open(sys.argv[1], 'w').write(str(len(sys.argv)))\n"
                "time.sleep(30)\n")
    p = util.spawn([sys.executable, script, marker])
    assert p is not None, "spawn 返回 None（启动失败）"
    for _ in range(40):
        if os.path.exists(marker):
            break
        time.sleep(0.25)
    assert os.path.exists(marker), "spawn 之后子进程没有跑起来"
    print("PASS: spawn 能把进程拉起来")
    try:
        p.terminate()
    except Exception:
        pass


def test_spawn_does_not_use_cmd_exe():
    """spawn 不能再经过 cmd.exe（cmd.exe 在锁屏封杀名单里）。"""
    import inspect
    import re
    src = inspect.getsource(util.spawn)
    # 只看"真的把 cmd.exe 当命令行参数用"的写法（文档里提到它是允许的）
    uses_cmd = re.search(r'["\']cmd\.exe["\']\s*,', src) is not None
    assert not uses_cmd, \
        "spawn 又用回 cmd.exe 当启动器了：锁屏期间限制执行器每秒杀 cmd.exe，拉不起进程"
    assert "DETACHED_PROCESS" in src, "spawn 应当使用 DETACHED_PROCESS 实现孤儿化"
    print("PASS: spawn 不依赖 cmd.exe（用 DETACHED_PROCESS）")


def test_child_survives_cmd_kill():
    """把 cmd.exe 全部杀掉（模拟锁定期间的限制执行器），spawn 仍须有效。"""
    subprocess.run(["taskkill", "/F", "/IM", "cmd.exe"], capture_output=True)
    marker = os.path.join(_TMP, "after_kill.txt")
    script = os.path.join(_TMP, "child2.py")
    with open(script, "w", encoding="utf-8") as f:
        f.write("import sys, time\n"
                "open(sys.argv[1], 'w').write('ok')\n"
                "time.sleep(30)\n")
    p = util.spawn([sys.executable, script, marker])
    assert p is not None, "cmd.exe 被杀之后 spawn 失败了（这正是实机 bug）"
    for _ in range(40):
        if os.path.exists(marker):
            break
        time.sleep(0.25)
    assert os.path.exists(marker), "cmd.exe 被杀之后子进程没跑起来"
    print("PASS: cmd.exe 被封杀时 spawn 依然有效（锁屏期间也能拉起确认窗口）")
    try:
        p.terminate()
    except Exception:
        pass


def main():
    for fn in (test_spawn_starts_process, test_spawn_does_not_use_cmd_exe,
               test_child_survives_cmd_kill):
        fn()
    print("\nspawn 测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())