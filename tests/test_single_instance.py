r"""单实例互斥体测试：同名互斥体必须拦住第二个实例。

背景（实机 bug）：single_instance 原先用 `_k32.GetLastError()` 判断，
而 _k32 是 use_last_error=True 打开的 DLL —— ctypes 把错误码存在私有副本里，
必须用 ctypes.get_last_error() 才读得到。结果判定不可靠，
实机上 core.exe 与 lockscreen.exe 各跑了 2 个实例（多个主控同时累计用量、
同时判定锁定，用量会翻倍）。

运行: python tests/test_single_instance.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from share import util  # noqa: E402

NAME = "pytest_single_instance"


def test_second_call_rejected():
    assert util.single_instance(NAME) is True, "首个实例应被放行"
    assert util.single_instance(NAME) is False, "第二个实例必须被拒绝"
    print("PASS: 同进程内第二个实例被拒绝")


def test_other_name_unaffected():
    assert util.single_instance(NAME + "_other") is True, "不同名字不应互相干扰"
    print("PASS: 不同名字的互斥体互不干扰")


def test_cross_process_rejected():
    """跨进程也要拦得住（这才是真实场景：守望者重复拉起）。"""
    code = (
        "import sys; sys.path.insert(0, r'%s'); "
        "from share import util; "
        "print(util.single_instance(r'%s'))"
    ) % (os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"), NAME)
    out = subprocess.run([sys.executable, "-c", code], capture_output=True,
                         text=True, timeout=60)
    got = (out.stdout or "").strip().splitlines()[-1] if out.stdout else ""
    assert got == "False", f"子进程应被拒绝，实际输出 {got!r} / {out.stderr[-200:]}"
    print("PASS: 另一个进程的实例被拒绝")


def main():
    for fn in (test_second_call_rejected, test_other_name_unaffected,
               test_cross_process_rejected):
        fn()
    print("\n单实例测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())