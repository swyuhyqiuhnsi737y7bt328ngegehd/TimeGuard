r"""加时请求（extra_req.json）的签名校验测试。

背景（真实漏洞）：这个文件是提权通道 —— core 把里面的 minutes 直接加进当日额度。
修复前孩子只要执行
    echo {"ts":<now>,"minutes":100000} > state\extra_req.json
就能让家长控制彻底失效。本测试把这条绕过路径钉死。

运行: python tests/test_extra_req.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from core import clock  # noqa: E402
from share import configmac  # noqa: E402


def main():
    key = configmac.create_key()

    # 1) 伪造：完全没签名（孩子的 echo）
    forged = {"ts": time.time(), "minutes": 100000}
    assert clock.validate_extra_request(forged, key) is None, "无签名的加时请求必须被拒绝"
    print("PASS: 无签名的伪造请求被拒绝")

    # 2) 伪造：签了名但事后改大分钟数
    signed = configmac.sign({"ts": time.time(), "minutes": 30}, key)
    signed["minutes"] = 100000
    assert clock.validate_extra_request(signed, key) is None, "改过内容的请求必须验签失败"
    print("PASS: 篡改 minutes 后验签失败")

    # 3) 伪造：用错误的密钥签名
    other = configmac.sign({"ts": time.time(), "minutes": 30}, "0" * 64)
    assert clock.validate_extra_request(other, key) is None, "错误密钥签名必须被拒绝"
    print("PASS: 错误密钥签名被拒绝")

    # 4) 合法请求
    ok = configmac.sign({"ts": time.time(), "minutes": 30}, key)
    assert clock.validate_extra_request(ok, key) == 30.0
    print("PASS: 合法请求通过")

    # 5) 过期请求（时效 120 秒）
    stale = configmac.sign({"ts": time.time() - 3600, "minutes": 30}, key)
    assert clock.validate_extra_request(stale, key) is None, "过期请求必须被拒绝"
    print("PASS: 过期请求被拒绝")

    # 6) 上限与非法值（即便签名合法也不接受）
    for bad in (0, -5, clock.MAX_EXTRA_MINUTES + 1, 1e9):
        r = configmac.sign({"ts": time.time(), "minutes": bad}, key)
        assert clock.validate_extra_request(r, key) is None, f"越界分钟数 {bad} 必须被拒绝"
    print("PASS: 越界/非法的分钟数被拒绝（含单次上限 %d）" % clock.MAX_EXTRA_MINUTES)

    # 7) 写入路径产出的请求，确实能通过校验
    written = clock.write_extra_request(45)
    assert written == 45.0, written
    p = clock.extra_req_path()
    assert os.path.exists(p), f"未写出 {p}"
    assert clock.read_extra_request() == 45.0, "自己写出的请求应当能通过校验"
    os.remove(p)
    print("PASS: 写出的签名请求可通过校验")

    # 8) write_extra_request 会夹到上限内，不写出越界请求
    assert clock.write_extra_request(999999) == float(clock.MAX_EXTRA_MINUTES)
    os.remove(clock.extra_req_path())
    print("PASS: 单次加时被夹到上限内")

    print("\n加时请求签名测试全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
