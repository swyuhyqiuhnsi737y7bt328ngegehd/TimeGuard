"""系统功能限制（家长控制）：通过 HKCU 组策略实现，即改即生效，无需管理员权限。

注意：
- 仅对当前 Windows 账户生效；
- 卸载时必须调用 clear_all() 清理，避免残留限制；
- 限制命令提示符/注册表编辑器后，构建脚本与 regedit 将不可用（属预期效果）。
"""
import winreg

_POLICY_ROOT = r"Software\Microsoft\Windows\CurrentVersion\Policies"


def _system_path():
    return _POLICY_ROOT + r"\System"


def _explorer_path():
    return _POLICY_ROOT + r"\Explorer"


# 限制项 -> (位置: sys/exp, 值名, 启用值, 显示名)
RESTRICTIONS = {
    "disable_taskmgr": ("sys", "DisableTaskMgr", 1, "禁用任务管理器"),
    "disable_regedit": ("sys", "DisableRegistryTools", 1, "禁用注册表编辑器(regedit)"),
    "disable_cmd": ("sys", "DisableCMD", 1, "禁用命令提示符(cmd/bat)"),
    "disable_run": ("exp", "NoRun", 1, "禁用运行窗口(Win+R)"),
    "disable_control_panel": ("exp", "NoControlPanel", 1, "禁用控制面板"),
    "disable_autorun": ("exp", "NoDriveTypeAutorun", 0xFF, "禁用移动存储自动运行(U盘等)"),
}


def display_name(key):
    return RESTRICTIONS.get(key, (None, None, None, key))[3]


def _path_of(loc):
    return _system_path() if loc == "sys" else _explorer_path()


def _set_dword(path, name, value):
    try:
        k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, path)
        winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, value)
        winreg.CloseKey(k)
    except Exception:
        pass


def _del_value(path, name):
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE)
        try:
            winreg.DeleteValue(k, name)
        except FileNotFoundError:
            pass
        winreg.CloseKey(k)
    except Exception:
        pass


def _get_dword(path, name):
    try:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path)
        v = winreg.QueryValueEx(k, name)[0]
        winreg.CloseKey(k)
        return v
    except Exception:
        return 0


def apply_restrictions(enabled: list):
    """启用 enabled 中的限制项，并关闭其它限制项。传入 None/[] 表示全部解除。"""
    enabled = set(enabled or [])
    for key, (loc, name, val, _label) in RESTRICTIONS.items():
        if key in enabled:
            _set_dword(_path_of(loc), name, val)
        else:
            _del_value(_path_of(loc), name)


def get_restrictions():
    """读取当前生效的限制项 key 列表。"""
    out = []
    for key, (loc, name, _val, _label) in RESTRICTIONS.items():
        if _get_dword(_path_of(loc), name) != 0:
            out.append(key)
    return out


def clear_all():
    """卸载时清除全部系统限制。"""
    for key, (loc, name, _val, _label) in RESTRICTIONS.items():
        _del_value(_path_of(loc), name)
