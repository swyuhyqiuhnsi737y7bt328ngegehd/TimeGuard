"""计时与防改时间：每日用量累计、跨天重置、时间回拨检测。

另外负责"家长加时请求"（state/extra_req.json）的**签名写入与校验**：
这个文件是提权通道 —— core 会把里面的 minutes 直接加到当日额度上，
所以它必须带 HMAC，否则孩子一条 echo 就能给自己加十万分钟。

信任边界说明：密钥与 policy.json 的 mac 密钥是同一个（state/config.key + HKCU 镜像）。
同用户权限下这挡不住"连密钥一起改写"的攻击 —— 与 configmac 的定位一致，
防的是"直接编辑配置文件"这类普通绕过，不是密码学级防护。
"""
import os
import time
from datetime import datetime

from share import configmac, logger, paths, util

# 加时请求的有效期：写入后必须在这段时间内被 core 看到（core 每几秒一轮询）
EXTRA_REQ_MAX_AGE = 120

# 单次加时的上限。防的不是孩子（他要能签名就已经赢了），
# 而是防家长自己填错数量级、以及签名密钥意外泄漏后被无限放大。
MAX_EXTRA_MINUTES = 1440


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


# ------------------------------------------------------------ 家长加时请求

def extra_req_path() -> str:
    return os.path.join(paths.state_dir(), "extra_req.json")


def _mac_key(create: bool = False):
    """取 MAC 密钥；create=True 时不存在就创建（源码模式直接跑 admin 的场景）。"""
    key = configmac.get_key()
    if not key and create:
        key = configmac.create_key()
    return key


def validate_extra_request(req, key, now=None, max_minutes: int = MAX_EXTRA_MINUTES):
    """校验加时请求，返回有效分钟数（float）；不合法返回 None。

    纯函数（不碰文件系统/时间以外的外部状态），便于单测。
    """
    if not isinstance(req, dict) or not key:
        return None
    if not configmac.verify(req, key):
        return None                                   # 无签名或签名不对 = 伪造
    if now is None:
        now = time.time()
    try:
        ts = float(req.get("ts", 0))
        m = float(req.get("minutes", 0))
    except (TypeError, ValueError):
        return None
    if abs(now - ts) > EXTRA_REQ_MAX_AGE:
        return None                                   # 过期或时间戳造假
    if not (0 < m <= max_minutes):
        return None                                   # 负数/零/离谱的大数
    return m


def read_extra_request(max_minutes: int = MAX_EXTRA_MINUTES):
    """读取并校验加时请求，返回有效分钟数；无效返回 None。"""
    req = util.read_json(extra_req_path(), None)
    if req is None:
        return None
    m = validate_extra_request(req, _mac_key(create=False), max_minutes=max_minutes)
    if m is None and isinstance(req, dict):
        logger.warn("加时请求未通过校验（无签名/签名错误/过期/超限），已忽略")
    return m


def write_extra_request(minutes: float):
    """签名后写入加时请求（lockscreen 密码解锁、admin 点"加时"用）。"""
    key = _mac_key(create=True)
    if not key:
        raise RuntimeError("拿不到 MAC 密钥，无法签名加时请求")
    m = min(max(float(minutes), 1.0), float(MAX_EXTRA_MINUTES))
    req = {"ts": time.time(), "minutes": m}
    util.write_json(extra_req_path(), configmac.sign(req, key))
    return m
