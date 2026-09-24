r"""对话框尺寸回归测试：_fit_and_center 必须保证内容不被裁切。

背景（修复的真实 bug）：三个对话框原来用固定几何尺寸（380x190 / 400x220 /
主窗口 620x880），而主窗口的表单是 5 列 grid，自然宽度 655px > 620px ——
右列（"周末:" 的输入框）被裁掉，底部按钮排到窗口外。
固定尺寸本质上是"猜"，换字体/换 DPI/加一行文案就会失效；这里改成按 Tk 自己
算出的内容需求（winfo_req*）定尺寸，并用本测试钉住这个不变量。

运行: python tests/test_admin_layout.py
"""
import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

try:
    from gui import admin as A  # noqa: E402
except Exception as e:                                            # pragma: no cover
    print(f"SKIP: 无法导入 admin（没有图形环境？）：{e}")
    sys.exit(0)


def _dummy_root():
    t = tk.Tk()
    t.withdraw()
    return t


def _pwd_dialog(win):
    tk.Label(win, text="请输入家长密码：", font=("Microsoft YaHei", 10)).pack(padx=18, pady=(16, 8))
    tk.Entry(win, show="*", font=("Microsoft YaHei", 12), width=24).pack(padx=18, pady=6)
    tk.Label(win, text="", font=("Microsoft YaHei", 9)).pack()
    btns = tk.Frame(win)
    btns.pack(pady=8)
    for txt in ("确定", "取消"):
        tk.Button(btns, text=txt, width=8).pack(side="left", padx=8)


def _quit_dialog(win):
    tk.Label(win, text="输入家长密码确认退出\n（退出后保护停止，需重新启动才能恢复）",
             font=("Microsoft YaHei", 10), justify="left").pack(padx=20, pady=(18, 8))
    tk.Entry(win, show="*", font=("Microsoft YaHei", 12), width=24).pack(padx=20, pady=6)
    tk.Label(win, text="", font=("Microsoft YaHei", 9)).pack()
    btns = tk.Frame(win)
    btns.pack(pady=10)
    for txt in ("确定退出", "取消"):
        tk.Button(btns, text=txt, width=10).pack(side="left", padx=8)


def _check(name, builder):
    win = tk.Tk()
    win.withdraw()
    builder(win)
    A._fit_and_center(win)
    win.update_idletasks()
    geo = win.geometry()
    w, h = (int(v) for v in geo.split("+")[0].split("x"))
    kids = win.winfo_children()
    need_w = max([c.winfo_reqwidth() for c in kids] or [0])
    need_h = sum(c.winfo_reqheight() for c in kids)
    win.destroy()
    assert w >= need_w, f"{name}: 窗口宽 {w} < 内容需要 {need_w}，右侧会被裁"
    assert h >= need_h, f"{name}: 窗口高 {h} < 内容需要 {need_h}，底部会被裁"
    print(f"PASS: {name} 尺寸足够（窗口 {w}x{h}，内容 {need_w}x{need_h}）")


def test_helper_exists():
    assert hasattr(A, "_fit_and_center"), "缺少 _fit_and_center 助手"
    print("PASS: _fit_and_center 存在")


def test_password_dialog_fits():
    _check("家长验证框", _pwd_dialog)


def test_quit_dialog_fits():
    _check("退出确认框", _quit_dialog)


def test_no_hardcoded_main_geometry():
    """主窗口不许再写死几何尺寸（写死就会在别的 DPI/字体下裁掉内容）。"""
    import inspect
    src = inspect.getsource(A._main)
    bad = [ln.strip() for ln in src.splitlines()
           if "geometry(" in ln and "x" in ln and "+" not in ln]
    assert not bad, f"主窗口仍在写死尺寸：{bad}"
    print("PASS: 主窗口没有写死的几何尺寸")


def main():
    for fn in (test_helper_exists, test_password_dialog_fits, test_quit_dialog_fits,
               test_no_hardcoded_main_geometry):
        fn()
    print("\nadmin 布局测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())