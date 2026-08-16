"""策略模型：加载 / 校验 / 查询（配额、禁止时段、家长密码）。"""
from datetime import datetime, time as dtime, timedelta

from share import paths, util

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
}

_last_good = None


def load() -> dict:
    """加载策略；配置损坏时退回上一次成功值。"""
    global _last_good
    data = util.read_json(paths.policy_path(), None)
    if not isinstance(data, dict):
        data = _last_good or {}
    cfg = dict(DEFAULTS)
    for k in DEFAULTS:
        if k in data and data[k] is not None:
            cfg[k] = data[k]
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
