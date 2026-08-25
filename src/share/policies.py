"""系统功能限制（家长控制）：通过 HKCU 组策略实现，即改即生效，无需管理员权限。

注意：
- 仅对当前 Windows 账户生效；
- 卸载时必须调用 clear_all() 清理，避免残留限制；
- 限制命令提示符/注册表编辑器后，构建脚本与 regedit 将不可用（属预期效果）；
- 写操作可能被安全软件（如 360 的“注册表防护”）拦截：apply_restrictions /
  clear_all 会返回失败项列表，调用方（admin/controller）必须提示家长并记录日志；
- 首次写入前快照用户原有的同名策略值，clear_all 只恢复/清理本程序改动过的值，
  不会破坏安装 TimeGuard 之前用户已有的设置；
- DisableCMD 使用值 2（cmd 与 .bat/.cmd 脚本一起禁）；PowerShell / Windows
  Terminal 不受该策略限制（已知局限，见 README）。
"""
import json
import os
import winreg

from share import logger, paths

_POLICY_ROOT = r"Software\Microsoft\Windows\CurrentVersion\Policies"

# 限制项 -> (位置: sys/exp, 值名, 启用值, 显示名)
RESTRICTIONS = {
    "disable_taskmgr": ("sys", "DisableTaskMgr", 1, "禁用任务管理器"),
    "disable_regedit": ("sys", "DisableRegistryTools", 1, "禁用注册表编辑器(regedit)"),
    # DisableCMD: 1=仅禁 cmd.exe；2=连 .bat/.cmd 脚本一起禁（与界面宣称一致）
    "disable_cmd": ("sys", "DisableCMD", 2, "禁用命令提示符(cmd/bat)"),
    "disable_run": ("exp", "NoRun", 1, "禁用运行窗口(Win+R)"),
    "disable_control_panel": ("exp", "NoControlPanel", 1, "禁用控制面板"),
    "disable_autorun": ("exp", "NoDriveTypeAutorun", 0xFF, "禁用移动存储自动运行(U盘等)"),
}

# 快照文件：记录“本程序写入前该值原本是什么”，保证 clear_all 能精确恢复。
# 测试可覆盖该路径（避免污染真实 state）。
_SNAPSHOT_PATH = None


def _snapshot_path():
    global _SNAPSHOT_PATH
    if _SNAPSHOT_PATH is None:
        _SNAPSHOT_PATH = os.path.join(paths.state_dir(), "restrict_snapshot.json")
    return _SNAPSHOT_PATH


def _load_snapshot():
    try:
        with open(_snapshot_path(), "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _save_snapshot(snap):
    try:
        os.makedirs(os.path.dirname(_snapshot_path()), exist_ok=True)
        with open(_snapshot_path(), "w", encoding="utf-8") as f:
            json.dump(snap, f, ensure_ascii=False, indent=1)
    except Exception as e:
        logger.warn(f"限制快照写入失败: {e}")


def _system_path():
    return _POLICY_ROOT + r"\System"


def _explorer_path():
    return _POLICY_ROOT + r"\Explorer"


def _path_of(loc):
    return _system_path() if loc == "sys" else _explorer_path()


def display_name(key):
    return RESTRICTIONS.get(key, (None, None, None, key))[3]


def _set_dword(path, name, value):
    """写入策略值；成功返回 True，失败返回 False（错误会记日志，不静默）。"""
    try:
        k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, path)
        winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, value)
        winreg.CloseKey(k)
        return True
    except Exception as e:
        logger.error(f"写入策略值失败 {path}\\{name}: {e}")
        return False


def _del_value(path, name):
    """删除策略值；成功（或本就不存在）返回 True，失败返回 False。"""
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE)
        try:
            winreg.DeleteValue(k, name)
        except FileNotFoundError:
            pass
        winreg.CloseKey(k)
        return True
    except Exception as e:
        logger.error(f"删除策略值失败 {path}\\{name}: {e}")
        return False


def _get_dword(path, name):
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path)
        v = winreg.QueryValueEx(k, name)[0]
        winreg.CloseKey(k)
        return v
    except Exception:
        return 0


def _encode_value(v, t):
    if t == winreg.REG_BINARY and isinstance(v, bytes):
        return [int(b) for b in v]
    return v


def _decode_value(v, t):
    if t == winreg.REG_BINARY and isinstance(v, list):
        return bytes(v)
    return v


def _read_slot(loc, name):
    """读取当前值 -> [type, value] 或 None（不存在）。"""
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, _path_of(loc))
    except FileNotFoundError:
        return None
    try:
        v, t = winreg.QueryValueEx(k, name)
        return [t, _encode_value(v, t)]
    except FileNotFoundError:
        return None
    finally:
        winreg.CloseKey(k)


def _restore_slot(loc, name, orig):
    """把某个值恢复到 TimeGuard 写入前的状态（orig 为 None = 原本不存在）。"""
    if orig is None:
        return _del_value(_path_of(loc), name)
    try:
        t, v = orig
        v = _decode_value(v, t)
        k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, _path_of(loc))
        winreg.SetValueEx(k, name, 0, t, v)
        winreg.CloseKey(k)
        return True
    except Exception as e:
        logger.error(f"恢复策略值失败 {_path_of(loc)}\\{name}: {e}")
        return False


def apply_restrictions(enabled: list):
    """启用 enabled 中的限制项，并关闭其它限制项。传入 None/[] 表示全部解除。

    返回失败项 key 列表（写注册表被安全软件拦截等）；空列表 = 全部成功。
    首次写入前快照用户原值；只改本程序负责的 6 个值，不影响其它策略。
    """
    enabled = set(enabled or [])
    snap = _load_snapshot()
    failed = []
    for key, (loc, name, val, _label) in RESTRICTIONS.items():
        rec = snap.get(key)
        if key in enabled:
            if rec is None:
                rec = {"orig": _read_slot(loc, name), "ours": False}
            if not rec.get("ours"):
                # 首次写入：记录原值后落盘（防止中途崩溃丢失快照）
                snap[key] = rec
                _save_snapshot(snap)
                if _set_dword(_path_of(loc), name, val):
                    rec["ours"] = True
                    snap[key] = rec
                    _save_snapshot(snap)
                else:
                    failed.append(key)
            else:
                # 之前已写入过（中间可能被孩子/其它程序改回）：直接重施加
                if not _set_dword(_path_of(loc), name, val):
                    failed.append(key)
        else:
            if rec and rec.get("ours"):
                if _restore_slot(loc, name, rec.get("orig")):
                    rec["ours"] = False
                    rec["orig"] = None
                    snap[key] = rec
                    _save_snapshot(snap)
                else:
                    failed.append(key)
    return failed


def get_restrictions():
    """读取当前生效的限制项 key 列表。"""
    out = []
    for key, (loc, name, _val, _label) in RESTRICTIONS.items():
        if _get_dword(_path_of(loc), name) != 0:
            out.append(key)
    return out


def clear_all():
    """卸载时清除全部系统限制：只恢复/清理 TimeGuard 自己写入的值，
    用户安装前已有的同名策略值会被原样恢复。返回失败项列表。"""
    snap = _load_snapshot()
    failed = []
    for key, (loc, name, _val, _label) in RESTRICTIONS.items():
        rec = snap.get(key)
        if rec and rec.get("ours"):
            if _restore_slot(loc, name, rec.get("orig")):
                rec["ours"] = False
                rec["orig"] = None
                snap[key] = rec
                _save_snapshot(snap)
            else:
                failed.append(key)
    return failed


def selftest():
    """真实键自检：向 Policies\Explorer 写入一个标记值、读回、删除。

    返回 (ok, 说明)。用于诊断“限制不生效”是不是注册表写入被安全软件拦截。
    """
    probe_name = "TimeGuardSelfTest"
    path = _explorer_path()
    try:
        if not _set_dword(path, probe_name, 1):
            return (False,
                    "注册表写入被拒绝！\n\n安全软件（如 360 的注册表防护/主动防御）拦截了对\n"
                    "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer 的写入，\n"
                    "系统功能限制将全部不生效。\n\n"
                    "请在 360 中关闭“注册表防护”或把本程序加入白名单后重试。")
        got = _get_dword(path, probe_name)
        if got != 1:
            _del_value(path, probe_name)
            return (False, f"写入成功但读回异常（value={got}），请检查安全软件。")
        if not _del_value(path, probe_name):
            return (False, "写入正常，但清理标记值失败（请手动删除 "
                           "HKCU\\...\\Policies\\Explorer\\TimeGuardSelfTest）。")
        return (True, "自检通过：注册表可正常写入/读取/清理，系统功能限制可以生效。")
    except Exception as e:
        logger.error(f"限制自检异常: {e}")
        return (False, f"自检异常：{e}")
