"""主控制进程 core.exe：时间策略执行、用量累计、托盘、拉起保护组件。"""
import os
import sys
import threading
import time
from datetime import datetime, timedelta

from core import clock, enforcer, policy
from share import logger, paths, util
from share.lockfile import FileLocker

_tray_icon = None


def _notify(title, msg):
    global _tray_icon
    if _tray_icon is not None:
        try:
            _tray_icon.notify(msg, title)
            return
        except Exception:
            pass
    logger.info(f"提醒[{title}]: {msg}")


def _make_icon_image():
    try:
        from PIL import Image, ImageDraw
        img = Image.new("RGBA", (64, 64), (24, 26, 44, 255))
        d = ImageDraw.Draw(img)
        d.ellipse([6, 6, 58, 58], outline=(140, 230, 150, 255), width=5)
        d.line([32, 32, 32, 15], fill=(140, 230, 150, 255), width=5)
        d.line([32, 32, 45, 39], fill=(140, 230, 150, 255), width=5)
        return img
    except Exception:
        return None


def _open_admin():
    util.spawn_service("admin")


def _lock_now():
    enforcer.set_lock("家长手动锁定", time.time() + 8 * 3600)
    enforcer.ensure_lockscreen()


def _quit_app():
    """家长确认退出：由独立进程(admin.exe --quit)弹模态密码框。

    不在托盘线程里创建 Tk（狂点输入框曾导致整进程卡退）；
    多开时第二个实例会因独立互斥体自动退出，不会叠加弹窗。
    """
    try:
        util.spawn_service("admin", ["--quit"])
    except Exception as e:
        logger.error(f"启动退出确认窗口失败: {e}")


def start_tray():
    global _tray_icon
    img = _make_icon_image()
    if img is None:
        logger.warn("托盘图标创建失败（缺少 Pillow 库？），仅日志运行")
        return
    try:
        import pystray
        _tray_icon = pystray.Icon("TimeGuard", img, "TimeGuard 时间控制",
                                  pystray.Menu(
                                      pystray.MenuItem("打开家长设置", _open_admin, default=True),
                                      pystray.MenuItem("立即锁定", _lock_now),
                                      pystray.MenuItem("退出程序（需家长密码）", _quit_app)))
        threading.Thread(target=_tray_icon.run, daemon=True).start()
        # 启动提示：Win11 托盘图标默认在溢出区(^)里，弹通知让用户知道程序已运行
        threading.Timer(3.0, _notify_startup).start()
    except Exception as e:
        logger.error(f"托盘启动失败: {e}")


def _notify_startup():
    global _tray_icon
    try:
        if _tray_icon is not None:
            _tray_icon.notify("TimeGuard 正在运行（托盘图标若在 ^ 溢出区，可拖出固定）",
                              "TimeGuard 时间控制")
    except Exception:
        pass


def _apply_extra_requests():
    """处理家长加时请求（lockscreen/admin 写入 state/extra_req.json）。"""
    p = os.path.join(paths.state_dir(), "extra_req.json")
    req = util.read_json(p, None)
    if not req:
        return
    try:
        if time.time() - float(req.get("ts", 0)) <= 120:
            m = float(req.get("minutes", 30))
            clock.add_extra(m)
            logger.info(f"家长加时 {int(m)} 分钟")
    except Exception:
        pass
    finally:
        try:
            os.remove(p)
        except OSError:
            pass


def _ensure_protection():
    """确保文件自锁与守望进程在运行（核心自己也参与保护环）。"""
    from guard import watchdog
    watchdog.ensure_fileguard()
    watchdog.ensure_guardians()


def _ensure_autostart():
    """自愈开机自启动：注册表 HKCU Run 指向自己（core.exe）。

    启动项只在注册表（非计划任务/启动文件夹）；每次启动都校正，
    避免卸载测试/路径变动后重启不再自动运行。
    """
    if not paths.is_frozen():
        return
    try:
        import winreg
        key = winreg.CreateKey(winreg.HKEY_CURRENT_USER,
                               r"Software\Microsoft\Windows\CurrentVersion\Run")
        try:
            cur = winreg.QueryValueEx(key, "TimeGuard")[0]
        except FileNotFoundError:
            cur = None
        if cur != sys.executable:
            winreg.SetValueEx(key, "TimeGuard", 0, winreg.REG_SZ, sys.executable)
            logger.info(f"已校正开机自启动: {sys.executable}")
        winreg.CloseKey(key)
    except Exception as e:
        logger.error(f"写入开机自启动失败: {e}")


def main():
    if not util.single_instance("core"):
        if util.launched_by_user():
            util.notify_ui("TimeGuard", "主控程序已在运行（请看任务栏托盘图标）。")
        return
    util.write_text(paths.service_pid_path("core"), str(os.getpid()))
    if paths.is_frozen():
        util.write_text(os.path.join(paths.state_dir(), "installed.flag"), "1")
        _ensure_autostart()
    logger.info(f"core 启动 pid={os.getpid()} frozen={paths.is_frozen()}")
    locker = FileLocker()
    locker.lock_self()
    locker.lock(paths.policy_path())  # 阻止删除/改名策略文件
    _ensure_protection()
    start_tray()
    cfg = policy.load()
    interval = max(2, int(cfg.get("check_interval_seconds", 5)))
    last_ts = time.time()
    last_policy_mtime = -1
    st = {"enforced": False, "reminded": False, "warned_no_pwd": False}
    tick_no = 0
    while True:
        if util.quit_flag_active():
            logger.info("core 收到退出指令")
            break
        try:
            # 策略热加载
            mtime = os.path.getmtime(paths.policy_path()) if os.path.exists(paths.policy_path()) else -1
            if mtime != last_policy_mtime:
                cfg = policy.load()
                last_policy_mtime = mtime
                interval = max(2, int(cfg.get("check_interval_seconds", 5)))
            # 未设置家长密码：不执行任何限制
            if not cfg.get("parent_password_hash"):
                if not st["warned_no_pwd"]:
                    logger.warn("未设置家长密码，限制功能未启用（请用 admin.exe 设置）")
                    st["warned_no_pwd"] = True
                time.sleep(interval)
                continue
            now = datetime.now()
            used, extra, _ = clock.tick(cfg, now)
            quota = policy.quota_for(cfg, now)
            allowed = quota + extra
            in_forb, until = policy.forbidden_window_info(cfg, now)
            # 手动锁定（家长在托盘/管理界面发起的锁定，直到时间到或密码解锁）
            manual = util.read_json(paths.lock_flag_path(), None)
            manual_active = bool(manual) and float(manual.get("until", 0)) > time.time()
            reason, until_ts = None, None
            if in_forb:
                reason = f"禁止使用时段，{until.strftime('%H:%M')} 后可继续"
                until_ts = until.timestamp()
            elif used >= allowed:
                reason = f"今日配额已用完（已用 {int(used)} / {int(allowed)} 分钟），次日 0 点重置"
                until_ts = datetime.combine(now.date() + timedelta(days=1),
                                            datetime.min.time()).timestamp()
            # 用量累计：仅在未被限制时计
            if not reason and not manual_active:
                elapsed = time.time() - last_ts
                if 0 < elapsed < interval * 4:
                    clock.accumulate(elapsed / 60.0)
            last_ts = time.time()
            if reason:
                if not st["enforced"]:
                    logger.info(f"开始限制：{reason}")
                enforcer.enforce(cfg, reason, until_ts)
                st["enforced"] = True
                st["reminded"] = False
            elif manual_active:
                enforcer.ensure_lockscreen()  # 维持手动锁定
            else:
                if st["enforced"]:
                    logger.info("限制解除")
                enforcer.clear_lock()
                st["enforced"] = False
                left = allowed - used
                remind = int(cfg.get("remind_minutes", 5))
                if 0 < left <= remind:
                    if not st["reminded"]:
                        _notify("TimeGuard 提醒", f"今日还可使用约 {int(left)} 分钟")
                        st["reminded"] = True
                else:
                    st["reminded"] = False
            _apply_extra_requests()
            # 周期性复核保护组件（防止守望进程被全部清掉）
            tick_no += 1
            if tick_no % 12 == 0:
                _ensure_protection()
        except Exception as e:
            logger.error(f"主循环异常: {e}")
        time.sleep(interval)
    locker.release()
    logger.info("core 退出")


if __name__ == "__main__":
    main()
