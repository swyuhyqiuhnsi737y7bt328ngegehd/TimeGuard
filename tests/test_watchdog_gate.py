r"""守望者拉起闸门测试：同一目标的重复拉起必须被挡住。

背景（实机 bug）：core.exe / lockscreen.exe 各跑了 2 个实例 ——
多个主控同时累计用量、同时判定锁定，用量会翻倍。
根因：_spawn_gate()（4 秒闸门）原先只在 ensure_guardians 开头用了一次，
ensure_service（拉起 core/lockscreen）完全没有保护；3 个守望者每秒都判定一次，
而新进程要 1~3 秒才出现在进程列表里 —— 窗口期内重复拉起。
另外闸门必须【按目标分开】：全局共用一个会把 core 的补位也一起挡住。

运行: python tests/test_watchdog_gate.py
"""
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from guard import watchdog  # noqa: E402
from share import paths  # noqa: E402

_TMP = tempfile.mkdtemp(prefix="tg_gate_")
paths.state_dir = lambda: _TMP


def test_first_spawn_allowed():
    assert watchdog._spawn_gate("core") is True, "第一次拉起应放行"
    print("PASS: 第一次拉起放行")


def test_second_spawn_blocked():
    assert watchdog._spawn_gate("core") is False, "4 秒内重复拉起必须被挡住"
    print("PASS: 同一目标的重复拉起被挡住（这就是重复实例的根因）")


def test_targets_are_independent():
    """按目标分闸门：guardian 抢了不能把 core 的补位也挡掉。"""
    assert watchdog._spawn_gate("guardian") is True, "不同目标应各自放行"
    assert watchdog._spawn_gate("core") is False, "core 仍在自己的闸门窗口内"
    assert watchdog._spawn_gate("lockscreen") is True, "lockscreen 也应能立刻拉起"
    print("PASS: 不同目标各自独立（不会互相阻塞补位）")


def test_gate_expires():
    """窗口过后要能再次拉起（否则目标被杀后永远补不上）。"""
    ok = False
    for _ in range(int(watchdog.SPAWN_GATE_SECONDS) + 3):
        time.sleep(1)
        if watchdog._spawn_gate("expire_test"):
            ok = True
            break
    assert ok, "闸门窗口过后必须重新放行"
    print("PASS: 闸门窗口过后重新放行")


def main():
    try:
        for fn in (test_first_spawn_allowed, test_second_spawn_blocked,
                   test_targets_are_independent, test_gate_expires):
            fn()
    finally:
        shutil.rmtree(_TMP, ignore_errors=True)
    print("\n守望闸门测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())