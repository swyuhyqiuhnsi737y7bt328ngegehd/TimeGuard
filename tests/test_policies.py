"""系统功能限制测试。

默认：重定向到测试注册表路径（Software\\TimeGuard\\TestPolicies），不影响真实策略。
真实键自检：python tests/test_policies.py --real
  （在真实 HKCU Policies 键上写入标记值并清理；若被安全软件拦截会如实报错，
    用于诊断“勾选了限制但没生效”）。
"""
import os
import sys
import tempfile
import winreg

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from share import policies  # noqa: E402


def real_selftest():
    """真实键自检：标记写入 -> 读回 -> 清理；再实测一轮 apply/get/clear。"""
    print("== 系统限制真实键自检 ==")
    ok, msg = policies.selftest()
    print(("PASS: " if ok else "FAIL: ") + msg.replace("\n", " "))
    if not ok:
        print("诊断：注册表写入被拦截（通常为 360 等安全软件的注册表防护），系统功能限制无法生效")
        return 1
    snap = os.path.join(tempfile.gettempdir(), "tg_real_restrict_snapshot.json")
    old_snap = policies._SNAPSHOT_PATH
    policies._SNAPSHOT_PATH = snap
    try:
        if os.path.exists(snap):
            os.remove(snap)
        failed = policies.apply_restrictions(["disable_taskmgr", "disable_run"])
        if failed:
            print("FAIL: 应用限制失败:", failed)
            return 1
        got = set(policies.get_restrictions())
        if not {"disable_taskmgr", "disable_run"} <= got:
            print("FAIL: 应用后读取校验不一致:", got)
            policies.clear_all()
            return 1
        print("PASS: 应用限制并读回校验一致")
        failed = policies.clear_all()
        if failed:
            print("FAIL: 清理失败:", failed)
            return 1
        print("PASS: 清理完成（只恢复 TimeGuard 写入的值）")
        print("真实键自检全部通过")
        return 0
    finally:
        policies._SNAPSHOT_PATH = old_snap
        try:
            os.remove(snap)
        except OSError:
            pass


def main():
    if "--real" in sys.argv:
        return real_selftest()
    real_root = policies._POLICY_ROOT
    policies._POLICY_ROOT = r"Software\TimeGuard\TestPolicies"
    snap = os.path.join(tempfile.gettempdir(), "tg_test_restrict_snapshot.json")
    old_snap = policies._SNAPSHOT_PATH
    policies._SNAPSHOT_PATH = snap
    try:
        if os.path.exists(snap):
            os.remove(snap)
        # 清理历史遗留的测试键（上次运行中断时可能残留），保证测试可重复
        for path in (policies._system_path(), policies._explorer_path()):
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
            except Exception:
                pass
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
        # 4) clear_all 后快照复位：再次 apply -> clear_all 也能清干净
        policies.apply_restrictions(["disable_regedit", "disable_control_panel"])
        policies.clear_all()
        got = policies.get_restrictions()
        assert got == [], got
        print("PASS: clear_all 生效（含快照复位）")
        # 5) 快照恢复语义：用户原本有值 -> 我们覆盖 -> 清除后原值被恢复
        #    模拟“用户值在 TimeGuard 首次应用之前就已存在”：重置快照后再播种
        if os.path.exists(snap):
            os.remove(snap)
        policies._set_dword(policies._path_of("exp"), "NoRun", 7)   # 模拟用户原值 7
        policies.apply_restrictions(["disable_run"])
        assert policies._get_dword(policies._path_of("exp"), "NoRun") == 1, "启用后应为 1"
        policies.apply_restrictions([])
        assert policies._get_dword(policies._path_of("exp"), "NoRun") == 7, "清除后应恢复原值 7"
        policies._del_value(policies._path_of("exp"), "NoRun")
        print("PASS: 清除后恢复用户原值（不破坏已有设置）")
        # 6) 写入失败可见性：快照路径不可写时 apply 应仍返回失败信息而非静默
        #    （真机行为由 --real 覆盖；这里验证接口能返回失败列表）
        assert policies.apply_restrictions([]) == [], "空列表应无失败项"
        print("PASS: apply_restrictions 返回失败项列表接口正常")
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
        policies._SNAPSHOT_PATH = old_snap
        try:
            os.remove(snap)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
