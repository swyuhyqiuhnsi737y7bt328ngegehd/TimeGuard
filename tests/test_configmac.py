"""配置完整性测试：签名/校验/篡改检测/自动恢复。运行: python tests/test_configmac.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from core import policy as P  # noqa: E402
from share import configmac, paths, util  # noqa: E402


def main():
    tmp = tempfile.mkdtemp()
    orig_policy = paths.policy_path
    orig_state = paths.state_dir
    paths.policy_path = lambda: os.path.join(tmp, "policy.json")
    paths.state_dir = lambda: tmp
    # 隔离注册表：测试不读写真实 HKCU\Software\TimeGuard
    reg = {}
    orig_reg_get = configmac._reg_get
    orig_reg_set = configmac._reg_set
    configmac._reg_get = lambda n: reg.get(n)
    configmac._reg_set = lambda n, v: reg.update({n: v})
    try:
        base_cfg = {"parent_password_hash": "abc123hash",
                    "daily_quota": {"weekday": 120, "weekend": 240}}
        # 1) 未初始化：按原样加载（兼容旧版）
        util.write_json(paths.policy_path(), dict(base_cfg))
        P._last_good = None
        c = P.load()
        assert c["parent_password_hash"] == "abc123hash", c
        print("PASS: 未初始化时按原样加载")
        # 2) 初始化：密钥 + 签名
        key = configmac.create_key()
        signed = configmac.sign(base_cfg, key)
        util.write_json(paths.policy_path(), signed)
        P._last_good = None
        c = P.load()
        assert c["parent_password_hash"] == "abc123hash", c
        print("PASS: 签名配置正常加载（并已生成备份）")
        # 3) 篡改密码 -> 自动恢复
        tampered = dict(signed)
        tampered["parent_password_hash"] = ""  # 孩子清空密码
        util.write_json(paths.policy_path(), tampered)
        P._last_good = None
        c = P.load()
        assert c["parent_password_hash"] == "abc123hash", c
        ondisk = util.read_json(paths.policy_path(), {})
        assert ondisk.get("parent_password_hash") == "abc123hash", ondisk
        print("PASS: 篡改密码被检测并自动恢复（磁盘也恢复）")
        # 4) 篡改配额 -> 自动恢复
        tampered2 = dict(signed)
        tampered2["daily_quota"] = {"weekday": 0, "weekend": 0}
        util.write_json(paths.policy_path(), tampered2)
        P._last_good = None
        c = P.load()
        assert c["daily_quota"]["weekday"] == 120, c["daily_quota"]
        print("PASS: 篡改配额被检测并恢复")
        # 5) 删除 mac 字段 -> 仍被拦截
        no_mac = {k: v for k, v in signed.items() if k != "mac"}
        util.write_json(paths.policy_path(), no_mac)
        P._last_good = None
        c = P.load()
        assert c["parent_password_hash"] == "abc123hash", c
        print("PASS: 删除 mac 字段同样被拦截恢复")
        # 6) 备份文件也被删 -> 注册表备份兜底
        try:
            os.remove(configmac.backup_path())
        except OSError:
            pass
        tampered3 = dict(signed)
        tampered3["parent_password_hash"] = "hacked"
        util.write_json(paths.policy_path(), tampered3)
        P._last_good = None
        c = P.load()
        assert c["parent_password_hash"] == "abc123hash", c
        print("PASS: 备份文件被删后从注册表备份恢复")
        print("configmac 测试全部通过")
        return 0
    finally:
        paths.policy_path = orig_policy
        paths.state_dir = orig_state
        configmac._reg_get = orig_reg_get
        configmac._reg_set = orig_reg_set


if __name__ == "__main__":
    sys.exit(main())
