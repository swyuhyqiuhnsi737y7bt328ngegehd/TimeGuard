"""系统功能限制测试（重定向到测试注册表路径，不影响真实策略）。
运行: python tests/test_policies.py
"""
import os
import sys
import winreg

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from share import policies  # noqa: E402


def main():
    real_root = policies._POLICY_ROOT
    policies._POLICY_ROOT = r"Software\TimeGuard\TestPolicies"
    try:
        # 1) 启用部分限制
        policies.apply_restrictions(["disable_taskmgr", "disable_autorun", "disable_cmd"])
        got = set(policies.get_restrictions())
        assert {"disable_taskmgr", "disable_autorun", "disable_cmd"} <= got, got
        assert "disable_run" not in got and "disable_regedit" not in got, got
        print("PASS: 启用限制写入生效")
        # 2) 重新 apply（覆盖未勾选项）
        policies.apply_restrictions(["disable_run"])
        got = set(policies.get_restrictions())
        assert got == {"disable_run"}, got
        print("PASS: 重新保存会关闭未勾选项")
        # 3) 全部清除
        policies.apply_restrictions([])
        got = policies.get_restrictions()
        assert got == [], got
        print("PASS: 清空限制生效")
        # 4) clear_all
        policies.apply_restrictions(["disable_regedit", "disable_control_panel"])
        policies.clear_all()
        got = policies.get_restrictions()
        assert got == [], got
        print("PASS: clear_all 生效")
        # 清理测试键
        for path in (policies._system_path(), policies._explorer_path()):
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
            except Exception:
                pass
        print("policies 测试全部通过")
        return 0
    finally:
        policies._POLICY_ROOT = real_root


if __name__ == "__main__":
    sys.exit(main())
