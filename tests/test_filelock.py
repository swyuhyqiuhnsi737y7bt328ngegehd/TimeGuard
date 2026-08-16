"""文件自锁测试：占用句柄后 删除/改名 被拒绝、写入允许、释放后可删。

跨进程验证：子进程持锁 -> 本进程删除失败 -> 子进程退出 -> 删除成功。
运行: python tests/test_filelock.py
"""
import os
import subprocess
import sys
import tempfile
import time

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")


def child_locker(path, seconds):
    """在子进程中占住文件句柄。"""
    code = "\n".join([
        "import sys; sys.path.insert(0, %r);" % SRC,
        "from share.lockfile import FileLocker;",
        "import time;",
        "l = FileLocker(); assert l.lock(sys.argv[1]);",
        "time.sleep(float(sys.argv[2]));",
    ])
    return subprocess.Popen([sys.executable, "-c", code, path, str(seconds)],
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def main():
    fd, path = tempfile.mkstemp(suffix=".exe")
    os.close(fd)
    with open(path, "w") as f:
        f.write("dummy")
    print("[1] 子进程持锁，删除应失败...")
    child = child_locker(path, 60)
    time.sleep(3)
    try:
        os.remove(path)
        print("FAIL: 持锁期间删除竟然成功")
        return 1
    except PermissionError:
        print("PASS: 持锁期间删除被拒绝")
    try:
        os.rename(path, path + ".renamed")
        print("FAIL: 持锁期间改名竟然成功")
        return 1
    except PermissionError:
        print("PASS: 持锁期间改名被拒绝")
    print("[2] 持锁期间写入应允许...")
    with open(path, "w") as f:
        f.write("still writable")
    print("PASS: 持锁期间可写入")
    print("[3] 释放锁后删除应成功...")
    child.terminate()
    child.wait(timeout=10)
    time.sleep(1)
    try:
        os.remove(path)
        print("PASS: 释放后删除成功")
    except PermissionError:
        print("FAIL: 释放后仍无法删除")
        return 1
    print("文件锁测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
