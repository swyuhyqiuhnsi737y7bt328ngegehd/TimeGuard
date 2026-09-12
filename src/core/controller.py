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
    enforcer.set_lock("家长手动锁定", time.time() + 8 * 3600, source="manual")
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


_live_cfg = {}          # 主循环重载后更新，限制执行器线程读取（避免线程内反复读盘验签）
_restrictor = None     # share.restrictor.Restrictor：进程拦截 + Win+R 钩子
_live_lockdown = False  # 是否处于锁定状态（锁定时强制封杀任务管理器/命令行等破解入口）


def _update_live_lockdown(locked: bool):
    """锁定状态变化时调用：锁屏期间强制拦截 taskmgr/cmd/powershell 等入口。

    单独用函数（带 global）赋值，避免 main 内直接赋值触发 UnboundLocalError。
    """
    global _live_lockdown
    _live_lockdown = bool(locked)


def _update_live_cfg(cfg):
    """主循环每次加载/重载策略后调用：同步给限制执行器并确保其已启动。"""
    global _live_cfg, _restrictor
    _live_cfg = dict(cfg)
    if _restrictor is None:
        try:
            from share import restrictor
            try:
                poll = max(0.5, float(cfg.get("restriction_poll_seconds", 1) or 1))
            except (TypeError, ValueError):
                poll = 1.0
            _restrictor = restrictor.Restrictor(
                lambda: _live_cfg.get("system_restrictions", []) or [],
                get_lockdown=lambda: _live_lockdown,
                poll_seconds=poll)
            _restrictor.start()
            logger.info(f"系统功能限制执行器已启动（轮询 {poll}s）：进程拦截 + Win+R 钩子")
        except Exception as e:
            logger.error(f"启动系统限制执行器失败: {e}")


def _stop_restrictor():
    global _restrictor
    if _restrictor is not None:
        try:
            _restrictor.stop()
        except Exception:
            pass
        _restrictor = None


def _ensure_autostart():
    """自愈开机自启动：注册表 HKCU Run 指向自己（core.exe）。

    启动项只在注册表（非计划任务/启动文件夹）；每次启动都校正，
    避免卸载测试/路径变动后重启不再自动运行。

    同时清除 StartupApproved\Run 里的 TimeGuard 禁用标记：任务管理器/
    360 开机加速把启动项标记为“禁用”后，即使 Run 键重新写回也不会执行，
    必须连标记一起清掉（只动 TimeGuard 自己的条目，不影响其它启动项）。
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
        # 清除禁用标记（StartupApproved\Run 下对应条目；不存在则忽略）
        try:
            k2 = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run",
                                0, winreg.KEY_SET_VALUE)
            try:
                winreg.DeleteValue(k2, "TimeGuard")
                logger.info("已清除开机自启动禁用标记（StartupApproved）")
            except FileNotFoundError:
                pass
            winreg.CloseKey(k2)
        except FileNotFoundError:
            pass
    except Exception as e:
        logger.error(f"写入开机自启动失败: {e}")


def main():
    global _restrictor  # 主循环内会重建执行器（对其赋值），须声明为模块级全局
    if not util.single_instance("core"):
        if util.launched_by_user():
            util.notify_ui("TimeGuard", "主控程序已在运行（请看任务栏托盘图标）。")
        return
    if util.launched_by_user():
        # 用户直接双击启动：清除可能残留的退出标记（显式启动 = 撤销退出意图），
        # 否则刚退出过（标记仍新鲜）时 core 会立即退出（“闪退”）
        try:
            if os.path.exists(paths.quit_flag_path()):
                os.remove(paths.quit_flag_path())
        except OSError:
            pass
    util.write_text(paths.service_pid_path("core"), str(os.getpid()))
    if paths.is_frozen():
        util.write_text(os.path.join(paths.state_dir(), "installed.flag"), "1")
        _ensure_autostart()
    logger.info(f"core 启动 pid={os.getpid()} frozen={paths.is_frozen()}")
    locker = FileLocker()
    locker.lock_self()
    locker.lock(paths.policy_path())  # 阻止删除/改名策略文件
    try:
        from share import configmac
        locker.lock(configmac.key_path())      # 完整性密钥
        locker.lock(configmac.backup_path())   # 配置备份
    except Exception:
        pass
    _ensure_protection()
    start_tray()
    cfg = policy.load()
    _update_live_cfg(cfg)  # 系统功能限制执行器（不依赖注册表，core 存活期间生效）
    interval = max(2, int(cfg.get("check_interval_seconds", 5)))
    last_ts = time.time()
    last_policy_mtime = -1
    st = {"enforced": False, "reminded": False, "warned_no_pwd": False}
    tick_no = 0
    while True:
        if util.quit_flag_active():
            logger.info("core 收到退出指令")
            break
        if _restrictor is not None and not _restrictor.is_alive():
            # 执行器意外退出则重建（防自愈失效）
            logger.warn("限制执行器线程已退出，正在重启")
            _restrictor = None
            _update_live_cfg(_live_cfg)
        try:
            # 策略热加载
            mtime = os.path.getmtime(paths.policy_path()) if os.path.exists(paths.policy_path()) else -1
            if mtime != last_policy_mtime:
                cfg = policy.load()
                last_policy_mtime = mtime
                interval = max(2, int(cfg.get("check_interval_seconds", 5)))
            # 系统功能限制：独立于家长密码，始终由限制执行器线程实施
            _update_live_cfg(cfg)
            # 未设置家长密码：不执行时间限制
            if not cfg.get("parent_password_hash"):
                if not st["warned_no_pwd"]:
                    logger.warn("未设置家长密码，限制功能未启用（请用 admin.exe 设置）")
                    st["warned_no_pwd"] = True
                _update_live_lockdown(False)
                time.sleep(interval)
                continue
            # 家长加时请求先于配额判断处理：lockscreen 解锁写了 extra_req.json，
            # 若等本 tick 末尾才加时，下个 tick 会因配额未变而重新锁定（解锁跳回 bug）
            _apply_extra_requests()
            now = datetime.now()
            used, extra, _ = clock.tick(cfg, now)
            quota = policy.quota_for(cfg, now)
            allowed = quota + extra
            in_forb, until = policy.forbidden_window_info(cfg, now)
            # 手动锁定（家长在托盘/管理界面发起，source=manual）：仅这种锁由 core 持续维持；
            # 配额/时段自动锁（source=auto/旧版无 source）在条件消失后必须能自动解除
            manual = util.read_json(paths.lock_flag_path(), None)
            manual_active = (bool(manual) and manual.get("source") == "manual"
                             and float(manual.get("until", 0)) > time.time())
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
            # 锁定期间强制封杀破解入口（任务管理器/命令行/终端/regedit）：
            # 堵死“Ctrl+Alt+Del -> 任务管理器 -> 运行新任务 -> taskkill”链路
            _update_live_lockdown(bool(reason) or manual_active)
            # 周期性复核保护组件（防止守望进程被全部清掉）
            tick_no += 1
            if tick_no % 12 == 0:
                _ensure_protection()
        except Exception as e:
            logger.error(f"主循环异常: {e}")
        time.sleep(interval)
    _stop_restrictor()   # 停限制执行器（退出时自动卸载 Win+R 钩子）
    locker.release()
    logger.info("core 退出")


if __name__ == "__main__":
    main()
