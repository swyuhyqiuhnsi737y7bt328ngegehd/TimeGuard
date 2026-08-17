"""配置完整性保护：防止直接编辑 policy.json 绕过家长设置（改密码/配额等）。

原理：
- 安装/首次保存时生成随机 MAC 密钥：state/config.key（注册表 HKCU/Software/TimeGuard/MacKey 有镜像）
- policy.json 增加 mac 字段 = HMAC-SHA256(密钥, 除 mac 外全部字段的规范 JSON)
- 任何校验失败的读取都被视为“配置被外部篡改”：
  core 拒绝采用被改的值，并从备份（state/policy.bak / 注册表 PolicyBackup）恢复家长设置

说明：同一用户权限下无法做到密码学级防护（孩子若同时改写密钥与备份仍可绕过），
本方案防的是“直接编辑配置文件”这类常见绕过，符合本项目“笨方法”定位。
"""
import hashlib
import hmac
import json
import os
import secrets

from . import logger, paths, util

MAC_KEY_NAME = "MacKey"
BAK_KEY_NAME = "PolicyBackup"
REG_BASE = r"Software\TimeGuard"

_last_bak = {"file": None, "reg": None}


def _reg_get(name):
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_BASE)
        v = winreg.QueryValueEx(k, name)[0]
        winreg.CloseKey(k)
        return v
    except Exception:
        return None


def _reg_set(name, value):
    try:
        import winreg
        k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, REG_BASE)
        winreg.SetValueEx(k, name, 0, winreg.REG_SZ, value)
        winreg.CloseKey(k)
    except Exception:
        pass


def key_path():
    return os.path.join(paths.state_dir(), "config.key")


def initialized() -> bool:
    """是否已进入 MAC 校验阶段（密钥存在即视为已初始化）。"""
    return os.path.exists(key_path()) or _reg_get(MAC_KEY_NAME) is not None


def get_key():
    try:
        t = util.read_text(key_path(), "").strip()
        if t:
            return t
    except Exception:
        pass
    return _reg_get(MAC_KEY_NAME)


def create_key():
    """生成随机密钥并写入文件 + 注册表镜像。"""
    k = secrets.token_hex(32)
    try:
        util.write_text(key_path(), k)
    except Exception as e:
        logger.warn(f"写入 config.key 失败: {e}")
    _reg_set(MAC_KEY_NAME, k)
    return k


def ensure_local():
    """注册表有密钥但本地文件缺失时补齐（升级/重建后的状态一致性）。"""
    k = get_key()
    if k and not os.path.exists(key_path()):
        try:
            util.write_text(key_path(), k)
        except Exception:
            pass
    return k


def canonical(cfg: dict) -> str:
    """除 mac 外全部字段的规范 JSON（键排序、紧凑、不转义非 ASCII）。"""
    payload = {k: v for k, v in cfg.items() if k != "mac"}
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sign(cfg: dict, key: str) -> dict:
    """返回带 mac 字段的配置副本（不修改原字典）。"""
    out = dict(cfg)
    out.pop("mac", None)
    out["mac"] = hmac.new(key.encode("utf-8"), canonical(out).encode("utf-8"),
                          hashlib.sha256).hexdigest()
    return out


def verify(cfg: dict, key: str) -> bool:
    mac = cfg.get("mac")
    if not mac or not key:
        return False
    expected = hmac.new(key.encode("utf-8"), canonical(cfg).encode("utf-8"),
                        hashlib.sha256).hexdigest()
    return hmac.compare_digest(mac, expected)


def backup_path():
    return os.path.join(paths.state_dir(), "policy.bak")


def save_backup(cfg: dict):
    """持久化备份（文件 + 注册表镜像）；内容未变化时跳过。
    注意：policy.bak 可能被占用锁保护（禁止改名），必须就地覆写，不能用 os.replace。
    """
    global _last_bak
    canon = canonical(cfg)
    try:
        if _last_bak["file"] != canon:
            with open(backup_path(), "w", encoding="utf-8") as f:
                json.dump(cfg, f, ensure_ascii=False)
            _last_bak["file"] = canon
    except Exception:
        pass
    try:
        if _last_bak["reg"] != canon:
            _reg_set(BAK_KEY_NAME, json.dumps(cfg, ensure_ascii=False))
            _last_bak["reg"] = canon
    except Exception:
        pass


def read_backup():
    try:
        d = util.read_json(backup_path(), None)
        if isinstance(d, dict):
            return d
    except Exception:
        pass
    try:
        t = _reg_get(BAK_KEY_NAME)
        if t:
            d = json.loads(t)
            if isinstance(d, dict):
                return d
    except Exception:
        pass
    return None


def remove_all():
    """卸载时清除密钥与备份（文件 + 注册表）。"""
    try:
        if os.path.exists(key_path()):
            os.remove(key_path())
    except Exception:
        pass
    try:
        if os.path.exists(backup_path()):
            os.remove(backup_path())
    except Exception:
        pass
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_BASE, 0, winreg.KEY_SET_VALUE)
        for name in (MAC_KEY_NAME, BAK_KEY_NAME):
            try:
                winreg.DeleteValue(k, name)
            except Exception:
                pass
        winreg.CloseKey(k)
    except Exception:
        pass
