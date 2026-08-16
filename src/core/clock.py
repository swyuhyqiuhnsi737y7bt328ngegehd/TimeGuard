"""计时与防改时间：每日用量累计、跨天重置、时间回拨检测。"""
import time
from datetime import datetime

from share import logger, paths, util


def load_usage() -> dict:
    return util.read_json(paths.usage_path(), {})


def _save(u):
    util.write_json(paths.usage_path(), u)


def tick(policy: dict, now: datetime):
    """每次主循环调用。返回 (used_minutes, extra_minutes, tamper_count)。"""
    u = load_usage()
    today = now.strftime("%Y-%m-%d")
    if u.get("date") != today:
        u = {"date": today, "used": 0.0, "extra": 0.0, "tamper": 0, "last_ts": time.time()}
    now_ts = time.time()
    last = float(u.get("last_ts", now_ts))
    tamper = int(u.get("tamper", 0))
    if now_ts < last - 120:  # 时间被回拨（容差 2 分钟，避免 NTP 微调误报）
        pen = float(policy.get("tamper_penalty_minutes", 60))
        u["used"] = float(u.get("used", 0)) + pen
        u["tamper"] = tamper + 1
        logger.warn(f"检测到系统时间回拨，今日额度扣减 {int(pen)} 分钟")
    u["last_ts"] = now_ts
    _save(u)
    return float(u.get("used", 0)), float(u.get("extra", 0)), int(u.get("tamper", 0))


def accumulate(minutes: float):
    """累计已用分钟数（仅主控进程调用）。"""
    u = load_usage()
    u["used"] = float(u.get("used", 0)) + max(0.0, minutes)
    u["last_ts"] = time.time()
    _save(u)


def add_extra(minutes: float):
    """家长加时：增加今日可用额度。"""
    u = load_usage()
    u["extra"] = float(u.get("extra", 0)) + max(0.0, minutes)
    _save(u)
