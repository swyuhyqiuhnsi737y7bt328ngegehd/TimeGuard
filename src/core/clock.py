"""计时与防改时间：每日用量累计、跨天重置、时间篡改检测。

另外负责"家长加时请求"（state/extra_req.json）的**签名写入与校验**：
这个文件是提权通道 —— core 会把里面的 minutes 直接加到当日额度上，
所以它必须带 HMAC，否则孩子一条 echo 就能给自己加十万分钟。

信任边界说明：密钥与 policy.json 的 mac 密钥是同一个（state/config.key + HKCU 镜像）。
同用户权限下这挡不住"连密钥一起改写"的攻击 —— 与 configmac 的定位一致，
防的是"直接编辑配置文件"这类普通绕过，不是密码学级防护。

计时凭证（state/usage.json）的保护策略见 USAGE_REG_VALUE：文件 + 注册表镜像，
并且**任何日期跳变都不再把用量清零**（见 tick 的注释，这是修掉的一个提权漏洞）。
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

# 时间连续性判定的容差：让 NTP 校时/夏令时这类正常微调不误报
TOLERANCE_SECONDS = 120
# 合法的"跨天"至少得有这么长的真实（单调时钟）流逝 —— 崩溃重启丢掉几分钟是正常的
MIN_DAY_ROLLOVER_SECONDS = 300

# usage.json 的注册表镜像（与 configmac 用注册表做备份镜像同一思路）。
# 为什么不只靠文件：usage.json 是唯一的计费凭证，删掉/写坏就会"静默归零"，
# 等于额度满血复活。镜像让"删文件"不再是无成本操作。
USAGE_REG_BASE = r"Software\TimeGuard"
USAGE_REG_VALUE = "UsageMirror"


def _reg_mirror_read() -> str:
    """读注册表里的用量镜像（裸 JSON 字符串），读不到返回空串。"""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, USAGE_REG_BASE) as k:
            return str(winreg.QueryValueEx(k, USAGE_REG_VALUE)[0] or "")
    except Exception:
        return ""


def _reg_mirror_write(raw: str):
    """写用量镜像（顺带做一次防重入：内容没变就不写）。"""
    if raw == _reg_mirror_read():
        return
    try:
        import winreg
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, USAGE_REG_BASE, 0,
                                winreg.KEY_SET_VALUE) as k:
            winreg.SetValueEx(k, USAGE_REG_VALUE, 0, winreg.REG_SZ, raw)
    except Exception as e:
        logger.warn(f"写用量镜像失败（不影响计时）：{e}")


def _mirror_backup(local: dict) -> dict:
    """决定是否要用注册表镜像兜底，以及兜底后重新落盘的副本。

    只在两种情况下兜底，避免把旧数据当成新数据覆盖掉：
      • 本地文件整个没了 / 结构坏了（used 缺失）；
      • 本地那份的日期比镜像旧（文件被还原成旧副本）。
    同一天时取两者较大的 used —— 删文件回退到旧镜像不该变成"减负"。
    """
    raw = _reg_mirror_read()
    if not raw:
        return None
    try:
        import json
        mirror = json.loads(raw)
    except Exception:
        return None
    if not isinstance(mirror, dict) or "used" not in mirror:
        return None
    if "used" not in local:
        logger.warn("计时凭证 usage.json 缺失或损坏，已用注册表镜像恢复（删除计时文件不会重置额度）")
        return mirror
    try:
        if str(mirror.get("date", "")) > str(local.get("date", "")):
            logger.warn("计时凭证 usage.json 的日期比镜像旧，已用镜像恢复（疑似被还原成旧副本）")
            return mirror
        if str(mirror.get("date", "")) == str(local.get("date", "")) \
                and float(mirror.get("used", 0)) > float(local.get("used", 0)):
            logger.warn("计时凭证 usage.json 的用量低于镜像，已取较大值（删除/回退文件不会重置额度）")
            return mirror
    except (TypeError, ValueError):
        return mirror
    return None


def load_usage() -> dict:
    """读当日用量；文件缺失/损坏时用注册表镜像兜底，而不是静默归零。"""
    u = util.read_json(paths.usage_path(), {})
    if not isinstance(u, dict):
        u = {}
    backup = _mirror_backup(u)
    if backup is None:
        return u
    try:
        return u if float(backup.get("used", 0)) <= float(u.get("used", 0)) else backup
    except (TypeError, ValueError):
        return backup


def _save(u):
    util.write_json(paths.usage_path(), u)
    try:
        import json
        _reg_mirror_write(json.dumps(u, ensure_ascii=False))
    except Exception:
        pass


def _elapsed_seconds(wall_jump: float, mono_jump: float) -> float:
    """这一轮主循环真正过了多久（秒）。

    优先用单调时钟：它不受改系统时间影响，是"时间真的流逝了多久"的唯一可信来源。
    单调时钟不可用时退回墙钟增量（此时无法识别篡改，只能保持旧的扣减行为）。
    """
    if mono_jump is None:
        return wall_jump
    return min(mono_jump, wall_jump) if wall_jump >= 0 else mono_jump


def tick(policy: dict, now: datetime, mono=None):
    """每次主循环调用。返回 (used_minutes, extra_minutes, tamper_count)。

    ⚠️ 铁律：**任何日期跳变都不允许把用量清零**。
    老实现是"日期 != 今天 → 整体重建 used/extra/tamper"，而回拨检测写在它后面，
    于是"把系统时间改成明天"就能让当日额度清零、连篡改计数一起抹掉 ——
    改一次时间 = 额度满血 + 无惩罚，防改时间功能形同虚设。
    现在改成：只有"单调时钟确实也走了至少 MIN_DAY_ROLLOVER_SECONDS、且与墙钟
    增量一致"才认定为合法跨天；其余日期变化一律按篡改处理：扣减惩罚、保留用量。
    """
    u = load_usage()
    today = now.strftime("%Y-%m-%d")
    wall = now.timestamp()
    mono_now = time.monotonic() if mono is None else float(mono)
    pre_ts = float(u.get("last_ts", wall))
    pre_mono = float(u.get("last_mono", 0.0))
    wall_jump = wall - pre_ts
    mono_jump = (mono_now - pre_mono) if pre_mono > 0 else None
    same_day = u.get("date") == today

    if not same_day:
        rollover_ok = (pre_mono > 0
                       and mono_jump is not None
                       and mono_jump >= MIN_DAY_ROLLOVER_SECONDS
                       and abs(wall_jump - mono_jump) <= TOLERANCE_SECONDS)
        if rollover_ok:
            logger.info(f"跨天：用量从 {u.get('date')} 重置为 {today}（时间连续性校验通过）")
            u = {"date": today, "used": 0.0, "extra": 0.0, "tamper": 0}
            u["last_ts"] = wall
            u["last_mono"] = mono_now
            _save(u)
            return 0.0, 0.0, 0
        # 日期跳变一律按篡改处理。注意这里【只能扣一次】：日期跳变本身就意味着
        # 墙钟与单调时钟不一致，再走下面那条检查会把同一次篡改罚两遍。
        _add_tamper(u, policy, "系统日期被改动")
    elif mono_jump is not None and abs(wall_jump - mono_jump) > TOLERANCE_SECONDS:
        # 同一天内把时钟往前/往后拨（没跨日期边界）
        _add_tamper(u, policy, "系统时间被大幅调整")

    u["date"] = today
    u["last_ts"] = wall
    u["last_mono"] = mono_now
    _save(u)
    return float(u.get("used", 0)), float(u.get("extra", 0)), int(u.get("tamper", 0))


def _add_tamper(u: dict, policy: dict, why: str):
    """记一次时间篡改：扣减额度假定 + 累加篡改计数（计数不再被跨天清零）。"""
    pen = float((policy or {}).get("tamper_penalty_minutes", 60))
    u["used"] = float(u.get("used", 0)) + pen
    u["tamper"] = int(u.get("tamper", 0)) + 1
    u["date"] = time.strftime("%Y-%m-%d")
    u["last_ts"] = time.time()
    u["last_mono"] = time.monotonic()
    logger.warn(f"检测到{why}，今日额度扣减 {int(pen)} 分钟（第 {u['tamper']} 次）")


def accumulate(minutes: float):
    """累计已用分钟数（仅主控进程调用）。"""
    u = load_usage()
    u["used"] = float(u.get("used", 0)) + max(0.0, minutes)
    u["last_ts"] = time.time()
    u["last_mono"] = time.monotonic()
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
