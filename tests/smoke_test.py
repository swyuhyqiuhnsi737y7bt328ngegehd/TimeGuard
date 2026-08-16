"""逻辑冒烟测试（不启动任何进程、不写状态文件）。运行: python tests/smoke_test.py"""
import os
import sys
from datetime import datetime

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from core import policy as P  # noqa: E402
from share import util  # noqa: E402


def test_quota():
    cfg = {"daily_quota": {"weekday": 100, "weekend": 300}}
    assert P.quota_for(cfg, datetime(2025, 1, 6)) == 100  # 周一
    assert P.quota_for(cfg, datetime(2025, 1, 5)) == 300  # 周日


def test_windows():
    cfg = {"forbidden_windows": [{"start": "22:30", "end": "07:30"}]}
    in1, u1 = P.forbidden_window_info(cfg, datetime(2025, 1, 6, 23, 0))
    assert in1 and u1.day == 7 and u1.hour == 7, (in1, u1)  # 跨午夜：次日 07:30
    in2, _ = P.forbidden_window_info(cfg, datetime(2025, 1, 6, 8, 0))
    assert not in2
    in3, u3 = P.forbidden_window_info(cfg, datetime(2025, 1, 6, 2, 0))
    assert in3 and u3.day == 6 and u3.hour == 7  # 当天 07:30
    cfg2 = {"forbidden_windows": [{"start": "12:00", "end": "13:00"}]}
    in4, u4 = P.forbidden_window_info(cfg2, datetime(2025, 1, 6, 12, 30))
    assert in4 and u4.hour == 13
    in5, _ = P.forbidden_window_info(cfg2, datetime(2025, 1, 6, 13, 30))
    assert not in5


def test_password():
    h = util.sha256_hex("abc")
    assert P.password_ok({"parent_password_hash": h}, "abc")
    assert not P.password_ok({"parent_password_hash": h}, "abd")
    assert P.password_ok({"parent_password_hash": ""}, "x")


def test_names():
    assert len(util.random_name(8)) == 8
    assert util.random_name() != util.random_name()


if __name__ == "__main__":
    for fn in (test_quota, test_windows, test_password, test_names):
        fn()
        print("ok:", fn.__name__)
    print("全部通过")
