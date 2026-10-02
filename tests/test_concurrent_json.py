r"""并发写 JSON 测试：多个进程同时写同一个文件不能互相撞坏临时文件。

背景（实机日志刷屏）：3 个守望副本每秒都在写 state/guardians.json，
而 util.write_json 原先用固定的 "<path>.tmp"，A 刚创建、B 就去 rename ——
Windows 上表现为持续报错：
    [WinError 32] 另一个程序正在使用此文件，进程无法访问
    [WinError 5]  拒绝访问
    [Errno 13]    Permission denied
修复：临时文件名按 pid 唯一化。

运行: python tests/test_concurrent_json.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

_TMP = tempfile.mkdtemp(prefix="tg_cjson_")
TARGET = os.path.join(_TMP, "shared.json")
ROUNDS = 60
WORKERS = 4

CHILD = """
import json, os, sys, time
sys.path.insert(0, sys.argv[1])
from share import util
target = sys.argv[2]
for i in range(int(sys.argv[3])):
    try:
        util.write_json(target, {"pid": os.getpid(), "i": i})
    except Exception as e:
        print("ERR:", type(e).__name__, e)
        sys.exit(1)
print("OK")
"""


def main():
    try:
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
        procs = [subprocess.Popen(
            [sys.executable, "-c", CHILD, src, TARGET, str(ROUNDS)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for _ in range(WORKERS)]
        bad = []
        for p in procs:
            out, _ = p.communicate(timeout=180)
            if p.returncode != 0 or "ERR:" in (out or ""):
                bad.append((p.returncode, (out or "").strip()[:200]))
        assert not bad, f"{WORKERS} 个进程并发写有失败：{bad}"
        print(f"PASS: {WORKERS} 个进程各写 {ROUNDS} 次，全部成功（无 WinError 32/5/13）")

        with open(TARGET, encoding="utf-8") as f:
            json.load(f)          # 目标文件必须仍是合法 JSON（原子替换生效）
        print("PASS: 目标文件仍是完整合法的 JSON（原子替换生效）")

        leftovers = [n for n in os.listdir(_TMP) if ".tmp" in n]
        assert not leftovers, f"留下了临时文件：{leftovers}"
        print("PASS: 没有残留 .tmp 文件")
    finally:
        shutil.rmtree(_TMP, ignore_errors=True)
    print("\n并发写 JSON 测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())