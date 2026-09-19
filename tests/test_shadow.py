"""磁盘还原模块的契约测试（不需要驱动即可运行）。

重点验证【结构体布局与内核头文件一致】——历史上因为用户态/内核结构体大小不一致，
DeviceIoControl 直接返回 error 122（缓冲区太小），排查花了不少时间，
这里用断言把它钉死。
"""
import ctypes
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

from share import shadow  # noqa: E402


def test_struct_layout():
    # 与 driver/tgshadow/tgshadow.h 一一对应：不一致会让 IOCTL 直接失败
    # 6 个 U32 + 12 个 U64 + 5 个 U32 = 24+96+20 = 140，按 8 字节对齐 -> 144
    assert ctypes.sizeof(shadow.TGSHADOW_STATUS) == 144, \
        f"TGSHADOW_STATUS 大小 {ctypes.sizeof(shadow.TGSHADOW_STATUS)} != 144"
    assert ctypes.sizeof(shadow.TGSHADOW_ENABLE_INPUT) == 544, \
        f"TGSHADOW_ENABLE_INPUT 大小 {ctypes.sizeof(shadow.TGSHADOW_ENABLE_INPUT)} != 544"
    assert ctypes.sizeof(shadow.TGSHADOW_VOLUME_INFO) == 16
    # 字段偏移抽查
    assert shadow.TGSHADOW_STATUS.ShadowBytesTotal.offset == 24
    assert shadow.TGSHADOW_STATUS.RedirectedBlocks.offset == 56
    assert shadow.TGSHADOW_STATUS.CowFailed.offset == 112


def test_ioctl_codes_match_header():
    # 与 tgshadow.h 的 CTL_CODE 结果一致（ENABLE = 0x8331A00C）
    assert shadow.IOCTL_GET_VERSION == 0x83312000
    assert shadow.IOCTL_GET_STATUS == 0x83312004
    assert shadow.IOCTL_ENABLE == 0x8331A00C
    assert shadow.IOCTL_DISABLE == 0x8331A010
    assert shadow.IOCTL_COMMIT == 0x8331A014
    assert shadow.IOCTL_DISCARD == 0x8331A018


def test_drives_and_volume_pick():
    drives = shadow.writable_drives()
    assert isinstance(drives, list)
    # 系统盘必须被排除（影子绝不能和受保护卷同盘）
    assert all(d.upper() != shadow.system_drive() for d, _ in drives)
    path, free_mb = shadow.pick_shadow_location(64)
    if drives:
        assert path is not None and path.lower().endswith("timeguardshadow.bin")


def test_shadow_file_preallocate():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "s.bin")
        n = shadow.ensure_shadow_file(p, 1)
        assert os.path.getsize(p) == n == 1024 * 1024
        # 再次调用不缩小
        assert shadow.ensure_shadow_file(p, 1) == 1024 * 1024


def test_names_used_by_admin_exist():
    """admin.py 用到的接口必须都在——避免界面点下去才发现方法不存在。"""
    for name in ("available", "driver_status_text", "pick_shadow_location",
                 "ensure_shadow_file", "apply_config", "sync_parent_password",
                 "enable", "disable", "commit", "discard", "deploy",
                 "service_running", "service_install" if False else "service_ask_now"):
        assert hasattr(shadow, name), f"shadow.{name} 缺失"


def test_graceful_without_driver():
    """没有驱动时不能抛异常炸掉界面。"""
    assert shadow.available() in (True, False)
    text = shadow.driver_status_text()
    assert isinstance(text, str) and text


if __name__ == "__main__":
    fails = 0
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        try:
            fn()
            print(f"  OK   {fn.__name__}")
        except Exception as e:
            fails += 1
            print(f"  FAIL {fn.__name__}: {e}")
    print("全部通过" if not fails else f"{fails} 项失败")
    sys.exit(1 if fails else 0)
