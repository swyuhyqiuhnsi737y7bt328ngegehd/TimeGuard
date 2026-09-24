r"""策略合并测试：policy.json 里 DEFAULTS 之外的键必须原样保留。

背景（修复的真实 bug）：老 _merge 只复制 DEFAULTS 里有的键，于是 admin 写进
policy.json 的 shadow_enabled / shadow_mb（磁盘还原设置）每次读都被丢掉 ——
界面永远显示默认值，家长"改了保存没反应"且看不出原因。

运行: python tests/test_policy_merge.py
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from core import policy  # noqa: E402
from share import configmac  # noqa: E402

_TMP = tempfile.mkdtemp(prefix="tg_polmerge_")

# 隔离 mac 密钥：必须把密钥也换掉。
# 用仓库真实的 state/config.key 时，测试写出的无签名 policy.json 会被判成外部篡改，
# policy.load 转而从备份恢复或回落默认值 —— 断言就测不到 _merge 本身了。
# （顺带说明：源码模式下改 config/policy.json 不生效，就是这个机制造成的。）
_KEY = os.path.join(_TMP, "config.key")
with open(_KEY, "w", encoding="utf-8") as _f:
    _f.write("0" * 64)
configmac.key_path = lambda: _KEY


def _sign(data):
    return configmac.sign(data, "0" * 64)


def _write(data):
    p = os.path.join(_TMP, "policy.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(_sign(data), f, ensure_ascii=False)
    return p


def test_unknown_keys_preserved():
    """磁盘还原设置（非 DEFAULTS 键）必须能读回来。"""
    p = _write({"shadow_enabled": True, "shadow_mb": 4096})
    policy.paths.policy_path = lambda: p
    cfg = policy.load()
    assert cfg.get("shadow_enabled") is True, f"shadow_enabled 被丢掉了：{cfg}"
    assert cfg.get("shadow_mb") == 4096, f"shadow_mb 被丢掉了：{cfg}"
    print("PASS: DEFAULTS 之外的键（shadow_enabled/shadow_mb）能读回来")


def test_defaults_still_applied():
    """缺字段时默认值仍然生效（不能因为保留未知键就把默认值弄丢）。"""
    p = _write({"shadow_enabled": True})
    policy.paths.policy_path = lambda: p
    cfg = policy.load()
    for k, v in policy.DEFAULTS.items():
        assert k in cfg, f"默认键 {k} 缺失"
        if k not in ("shadow_enabled", "shadow_mb"):
            assert cfg[k] == v or cfg[k] is not None, f"{k} 默认值异常：{cfg[k]!r}"
    assert cfg["shadow_enabled"] is True
    assert cfg["check_interval_seconds"] == policy.DEFAULTS["check_interval_seconds"]
    print("PASS: 缺字段时默认值仍然补上")


def test_explicit_none_replaced_by_default():
    """显式 null 视为"没设置"，用默认值填（老行为要保持）。"""
    p = _write({"remind_minutes": None})
    policy.paths.policy_path = lambda: p
    cfg = policy.load()
    assert cfg["remind_minutes"] == policy.DEFAULTS["remind_minutes"], cfg["remind_minutes"]
    print("PASS: 显式 null 仍然回落到默认值")


def test_empty_file_still_defaults():
    """空文件/坏文件不能把程序搞崩，仍要给出可用默认值。"""
    p = _write({})
    policy.paths.policy_path = lambda: p
    cfg = policy.load()
    assert cfg["daily_quota"] == policy.DEFAULTS["daily_quota"]
    print("PASS: 空配置仍返回完整默认值")


def test_roundtrip_through_save():
    """模拟 admin 保存 -> core 读取的完整来回。"""
    p = os.path.join(_TMP, "policy2.json")
    policy.paths.policy_path = lambda: p
    _write({})                      # 先落一份签名过的初始配置，避免触发篡改告警
    data = policy.load()
    data["shadow_enabled"] = True
    data["shadow_mb"] = 2048
    data["daily_quota"] = {"weekday": 90, "weekend": 180}
    with open(p, "w", encoding="utf-8") as f:
        json.dump(_sign(data), f, ensure_ascii=False)
    back = policy.load()
    assert back["shadow_enabled"] is True and back["shadow_mb"] == 2048, back
    assert back["daily_quota"] == {"weekday": 90, "weekend": 180}, back["daily_quota"]
    print("PASS: admin 保存 -> core 读取 的来回不丢字段")


def main():
    try:
        for fn in (test_unknown_keys_preserved, test_defaults_still_applied,
                   test_explicit_none_replaced_by_default, test_empty_file_still_defaults,
                   test_roundtrip_through_save):
            fn()
    finally:
        shutil.rmtree(_TMP, ignore_errors=True)
    print("\npolicy 合并测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())