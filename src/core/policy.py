"""策略模型：加载 / 校验 / 查询（配额、禁止时段、家长密码）。

加载时做完整性校验（见 share/configmac.py）：密钥存在时要求 mac 签名有效，
签名无效 = 配置被外部修改（如孩子直接编辑 policy.json）-> 拒绝采用并从备份恢复。
"""
import json
from datetime import datetime, time as dtime, timedelta

from share import configmac, logger, paths, util

DEFAULTS = {
    "version": 1,
    "parent_password_hash": "",            # 为空 = 未设置密码，限制不生效
    "daily_quota": {"weekday": 120, "weekend": 240},   # 分钟
    "forbidden_windows": [{"start": "22:30", "end": "07:30"}],
    "enforce_action": "lock",              # lock | kill | logoff | shutdown
    "kill_processes": [],                  # action=kill 时结束的进程名列表
    "remind_minutes": 5,                   # 剩余这么多分钟时提醒
    "extra_minutes_per_unlock": 30,        # 家长密码解锁一次增加的分钟数
    "tamper_penalty_minutes": 60,          # 检测到改时间回拨的惩罚（加到已用时长）
    "check_interval_seconds": 5,
    "restriction_poll_seconds": 1,          # 系统限制执行器轮询周期（秒，进程级拦截/Win+R 钩子）
    "system_restrictions": [],               # 系统功能限制项（见 share/restrictor.py）
}

_last_good = None


def _merge(data: dict) -> dict:
    """默认值打底 + 叠加文件内容。

    ⚠️ 必须保留 DEFAULTS 之外的键，不能只挑 DEFAULTS 里的键复制：
    admin 会把 shadow_enabled / shadow_mb（磁盘还原设置）写进 policy.json，
    老实现读的时候把这些键丢掉，于是界面永远显示默认值 ——
    "改了保存没反应"，而且看不出是哪儿丢的。
    """
    cfg = dict(data) if isinstance(data, dict) else {}
    for k, v in DEFAULTS.items():
        if cfg.get(k) is None:
            cfg[k] = v
    return cfg


def _restore_in_place(cfg: dict, key: str):
    """把恢复出的配置【重新签名后】写回 policy.json。

    就地覆写（policy.json 可能被 fileguard 占用锁禁止改名，不能用 rename）。

    ⚠️ 必须重新签名：写回一份无签名的配置，下次加载又会验签失败 ——
    于是打包版每次开机都报一次"配置被外部修改"，磁盘上的配置永远处于被篡改状态，
    家长还会看到莫名其妙的告警（这正是加这个参数的修复原因）。
    """
    try:
        signed = configmac.sign(cfg, key) if key else cfg
        with open(paths.policy_path(), "w", encoding="utf-8") as f:
            json.dump(signed, f, ensure_ascii=False, indent=2)
        if key:
            configmac.save_backup(signed)
    except Exception as e:
        logger.warn(f"恢复配置写回失败: {e}")


def load() -> dict:
    """加载策略（含完整性校验）。

    - 未初始化（无 config.key）：按原样加载（兼容旧版配置）
    - 已初始化：要求 mac 签名有效；无效 = 被外部篡改
      -> 从备份(state/policy.bak / 注册表)恢复家长设置并拒绝被改的值
    """
    global _last_good
    data = util.read_json(paths.policy_path(), None)
    if not isinstance(data, dict):
        data = _last_good or {}
    if configmac.initialized():
        configmac.ensure_local()  # 补齐密钥文件（注册表有、文件缺失时）
        key = configmac.get_key()
        if key and configmac.verify(data, key):
            cfg = _merge(data)
            _last_good = cfg
            configmac.save_backup(data)  # 持久化备份（供篡改后恢复）
            return cfg
        # 签名无效：配置被外部修改
        logger.warn("检测到策略配置被外部修改（mac 校验失败），正在从备份恢复家长设置")
        restored = configmac.read_backup()
        if isinstance(restored, dict) and key and configmac.verify(restored, key):
            if paths.is_frozen():
                # 仅打包版就地写回修复；源码模式不碰项目模板文件
                _restore_in_place(restored, key)
            cfg = _merge(restored)
            _last_good = cfg
            return cfg
        if _last_good is not None:
            return _last_good  # 用内存中的上一次有效配置
        logger.warn("配置被篡改且无有效备份，使用默认配置（请用 admin 重新设置）")
        return dict(DEFAULTS)
    cfg = _merge(data)
    _last_good = cfg
    return cfg


def quota_for(cfg, now: datetime) -> int:
    key = "weekend" if now.weekday() >= 5 else "weekday"
    try:
        return max(0, int(cfg.get("daily_quota", {}).get(key, 120)))
    except Exception:
        return 120


def _hhmm(s):
    h, m = str(s).strip().split(":")
    return int(h), int(m)


def forbidden_window_info(cfg, now: datetime):
    """返回 (是否在禁止时段, 结束时间或 None)。支持跨午夜的时段。"""
    t = now.time()
    best = None
    for w in cfg.get("forbidden_windows", []) or []:
        try:
            sh, sm = _hhmm(w["start"])
            eh, em = _hhmm(w["end"])
        except Exception:
            continue
        start, end = dtime(sh, sm), dtime(eh, em)
        if start <= end:
            if start <= t < end:
                until = datetime.combine(now.date(), end)
                if best is None or until < best:
                    best = until
        else:  # 跨午夜，如 22:30 - 07:30
            if t >= start or t < end:
                if t >= start:
                    until = datetime.combine(now.date() + timedelta(days=1), end)
                else:
                    until = datetime.combine(now.date(), end)
                if best is None or until < best:
                    best = until
    return (best is not None, best)


def password_ok(cfg, pwd: str) -> bool:
    h = str(cfg.get("parent_password_hash") or "")
    if not h:
        return True
    return util.sha256_hex(pwd) == h
