r"""计时与防改时间测试（clock）：覆盖"改系统时间/删计时文件就能重置额度"这条提权路径。

背景（修复的两个真实漏洞）：
  1. 老实现是"日期 != 今天 -> 整体重建 used/extra/tamper"，而回拨检测写在它后面，
     所以把系统时间改成明天就能让当日额度清零、连篡改计数一起抹掉 ——
     防改时间功能形同虚设（README 承诺的 tamper_penalty 从不生效）。
  2. usage.json 是唯一计费凭证却没有任何兜底，删掉/写坏就静默归零。

运行: python tests/test_clock.py
"""
import json
import os
import shutil
import sys
import tempfile
from datetime import datetime, timedelta

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from core import clock  # noqa: E402
from share import paths  # noqa: E402

# 隔离：用量文件与注册表镜像都指向临时位置，绝不碰真实状态
_TMP = tempfile.mkdtemp(prefix="tg_clock_")
paths.usage_path = lambda: os.path.join(_TMP, "usage.json")
clock.USAGE_REG_BASE = r"Software\TimeGuardTest\Clock"

POLICY = {"tamper_penalty_minutes": 60}
DAY = datetime(2026, 3, 10, 12, 0, 0)      # 一个不会撞上"今天"的日期
DAY_TS = DAY.timestamp()


def _reset():
    p = paths.usage_path()
    if os.path.exists(p):
        os.remove(p)
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, clock.USAGE_REG_BASE, 0,
                           winreg.KEY_SET_VALUE)
        try:
            winreg.DeleteValue(k, clock.USAGE_REG_VALUE)
        except FileNotFoundError:
            pass
        k.Close()
    except FileNotFoundError:
        pass


def _seed(used, extra=0.0, tamper=0, date=DAY, mono=1000.0):
    p = paths.usage_path()
    with open(p, "w", encoding="utf-8") as f:
        json.dump({"date": date.strftime("%Y-%m-%d"), "used": used, "extra": extra,
                   "tamper": tamper, "last_ts": date.timestamp(), "last_mono": mono}, f)


def _read():
    with open(paths.usage_path(), encoding="utf-8") as f:
        return json.load(f)


def test_no_reset_on_forward_date_change():
    """把时间往前调一天，不许清零额度（老实现就在这里把 used 清成 0）。"""
    _reset()
    _seed(used=100.0, mono=1000.0)
    used, extra, tamper = clock.tick(POLICY, DAY + timedelta(days=1), mono=1000.5)
    assert used >= 100.0, f"日期跳变不应清零用量，实际 {used}"
    assert tamper >= 1, "日期跳变应记为篡改"
    assert used == 160.0, f"应施加 60 分钟惩罚，实际 {used}"
    print("PASS: 向前改日期不清零额度，且施加篡改惩罚")


def test_no_reset_on_backward_date_change():
    """把时间往回调，同样不许清零。"""
    _reset()
    _seed(used=50.0, mono=1000.0)
    used, _, tamper = clock.tick(POLICY, DAY - timedelta(days=1), mono=1000.5)
    assert used >= 50.0, f"回拨不应清零用量，实际 {used}"
    assert tamper >= 1, "回拨应记为篡改"
    print("PASS: 回拨系统时间不清零额度")


def test_legit_midnight_rollover_resets():
    """真正跨天（单调时钟也走了 6 小时、且与墙钟一致）才允许重置。"""
    _reset()
    _seed(used=200.0, mono=1000.0, date=datetime(2026, 3, 10, 23, 0, 0))
    used, extra, tamper = clock.tick(POLICY, datetime(2026, 3, 11, 5, 0, 0), mono=1000.0 + 21600)
    assert used == 0.0 and extra == 0.0 and tamper == 0, (used, extra, tamper)
    print("PASS: 合法跨天正常重置")


def test_fake_midnight_not_reset():
    """墙钟跨天但单调时钟没动 = 改时间，绝不重置。"""
    _reset()
    _seed(used=90.0, mono=1000.0, date=datetime(2026, 3, 10, 23, 0, 0))
    used, _, tamper = clock.tick(POLICY, datetime(2026, 3, 11, 0, 30, 0), mono=1000.2)
    assert used >= 90.0, f"假跨天不应清零，实际 {used}"
    assert tamper >= 1, "假跨天应记为篡改"
    print("PASS: 假跨天（只改墙钟）被识别为篡改，不清零")


def test_small_clock_jump_not_reset():
    """同一天内往前调几小时：也不许把用量变小。"""
    _reset()
    _seed(used=77.0, mono=1000.0)
    used, _, tamper = clock.tick(POLICY, DAY + timedelta(hours=5), mono=1000.1)
    assert used >= 77.0, f"小幅改时间不应减少用量，实际 {used}"
    print("PASS: 同日内改时间不减少用量")


def test_normal_tick_accumulates():
    """正常推进：单调与墙钟一致 -> 不加惩罚、用量原样返回。"""
    _reset()
    _seed(used=30.0, mono=1000.0)
    used, _, tamper = clock.tick(POLICY, DAY + timedelta(seconds=5), mono=1005.0)
    assert used == 30.0 and tamper == 0, (used, tamper)
    print("PASS: 正常 tick 不误报篡改")


def test_usage_file_deleted_restores_from_mirror():
    """删掉 usage.json 不能把额度刷回 0（注册表镜像兜底）。"""
    _reset()
    _seed(used=120.0, mono=1000.0)
    clock.tick(POLICY, DAY + timedelta(seconds=5), mono=1005.0)   # 建立镜像
    os.remove(paths.usage_path())                                # 模拟孩子删文件
    u = clock.load_usage()
    assert float(u.get("used", 0)) >= 120.0, f"删文件后用量被重置了：{u}"
    print("PASS: 删除 usage.json 后从镜像恢复用量（不再静默归零）")


def test_usage_file_tampered_to_zero_restores_from_mirror():
    """把 usage.json 改成 used=0 也没用，取两者较大值。"""
    _reset()
    _seed(used=150.0, mono=1000.0)
    clock.tick(POLICY, DAY + timedelta(seconds=5), mono=1005.0)
    _seed(used=0.0, mono=1000.0)                                 # 手改成 0
    u = clock.load_usage()
    assert float(u.get("used", 0)) >= 150.0, f"改成 0 应被镜像顶回去：{u}"
    print("PASS: 把 usage.json 改成 0 会被镜像顶回较大值")


def test_tamper_count_survives_rollover():
    """篡改计数不能被跨天清零（老实现会连 tamper 一起抹掉）。"""
    _reset()
    _seed(used=10.0, mono=1000.0)
    clock.tick(POLICY, DAY + timedelta(days=1), mono=1000.5)      # 篡改一次 -> tamper=1
    assert _read()["tamper"] >= 1
    clock.tick(POLICY, DAY + timedelta(days=1, seconds=5), mono=1005.5)
    assert int(_read()["tamper"]) >= 1, "篡改计数不该凭空消失"
    print("PASS: 篡改计数可持续累计")


def main():
    try:
        for fn in (test_no_reset_on_forward_date_change,
                   test_no_reset_on_backward_date_change,
                   test_legit_midnight_rollover_resets,
                   test_fake_midnight_not_reset,
                   test_small_clock_jump_not_reset,
                   test_normal_tick_accumulates,
                   test_usage_file_deleted_restores_from_mirror,
                   test_usage_file_tampered_to_zero_restores_from_mirror,
                   test_tamper_count_survives_rollover):
            fn()
    finally:
        shutil.rmtree(_TMP, ignore_errors=True)
    print("\nclock 测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())