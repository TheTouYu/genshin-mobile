#!/usr/bin/env python3
"""txsim.py —— 按协议 v2 给进程内探针发脚本化触摸包（离线验收台的发送端）。

协议 v2（与 probe/mobui.cpp 的 net_apply 对齐）：
     byte0 = 'T' (0x54)
     byte1 = 0x4D  魔数（v1 没有；探针靠它把协议不符变成明确拒收）
     byte2 = count
     随后 count × {u16 id, u16 nx, u16 ny}（小端），nx/ny = 0..65535 归一化坐标
     总长 = 3 + 6*count

场景（每个场景前会往接收端日志追加一行 `SCENARIO Sx`）：
     S1 单点：id=20 中心点 1 包 → 2 包位移 → 1 包 count=0（抬指）
     S2 双指捏合：id=20,21 起始相距 200px，向两侧张开 4 包后同时抬指（count=0）
     S3 双指同起同落：一个包里两条 DOWN，一个包里两条 UP
     S4 id 复用：抬指后用同一 id 20 重新按下（必须是新的 DOWN，不能并成 MOVE）

发送记录写 build/txsim.sent，验收脚本据此算期望像素坐标：
     SCENARIO S1
     SEND S1 pkt=1 id=20 nx=32768 ny=32768
环境变量：TXSIM_PORT(51721) TXSIM_GAP(0.12) TXSIM_SGAP(0.05)
          TXSIM_W(2560) TXSIM_H(1440) TXSIM_SENT TXSIM_RECVLOG
"""
import os
import socket
import struct
import sys
import time

PORT = int(os.environ.get("TXSIM_PORT", "51721"))
GAP = float(os.environ.get("TXSIM_GAP", "0.12"))
SGAP = float(os.environ.get("TXSIM_SGAP", "0.05"))
W = int(os.environ.get("TXSIM_W", "2560"))
H = int(os.environ.get("TXSIM_H", "1440"))
SENT = os.environ.get("TXSIM_SENT", "build/txsim.sent")
RECVLOG = os.environ.get("TXSIM_RECVLOG", "build/touchrecv.log")

MAGIC = 0x4D


def norm(px, total):
    """像素 → 0..65535 归一化（四舍五入，越界夹紧）。"""
    v = int(round(px * 65535.0 / total))
    return max(0, min(65535, v))


def nxy(px, py):
    return norm(px, W), norm(py, H)


# 场景表：[场景名, [每包 [(id, px, py), ...]（空 = count=0 抬指包）]]
SCENARIOS = [
    ("S1", [
        [(20, 1280, 720)],          # 中心点按下
        [(20, 1024, 864)],          # 位移 1
        [(20, 1536, 576)],          # 位移 2
        [],                         # 抬指
    ]),
    ("S2", [
        [(20, 1180, 720), (21, 1380, 720)],   # 两指相距 200px
        [(20, 1120, 720), (21, 1440, 720)],   # 张开 1
        [(20, 1060, 720), (21, 1500, 720)],   # 张开 2
        [(20, 1000, 720), (21, 1560, 720)],   # 张开 3
        [(20, 940, 720), (21, 1620, 720)],    # 张开 4
        [],                                   # 同时抬指
    ]),
    ("S3", [
        [(30, 1100, 800), (31, 1460, 800)],   # 双指同时按下
        [],                                   # 双指同时抬起（一个消息里两条 UP）
    ]),
    ("S4", [
        [(20, 1280, 720)],          # 按下
        [],                         # 抬起
        [(20, 1000, 500)],          # 同一 id 重新按下 → 必须是新 DOWN
        [],                         # 抬起
    ]),
]


def _recv_lines():
    try:
        with open(RECVLOG, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def msgs_since_marker(scen):
    lines = _recv_lines()
    idx = -1
    for i, ln in enumerate(lines):
        if ln.strip() == "SCENARIO " + scen:
            idx = i
    if idx < 0:
        return 0
    return sum(1 for ln in lines[idx + 1:] if ln.startswith("MSG "))


def wait_scenario(scen, expect, timeout=4.0):
    """等接收端把这个场景的消息全部落盘，再写下个 SCENARIO 标记。

    没有这个握手时，Wine 消息泵一忙（全屏窗口上的高频指针流），最后一条 MOVE/UP 会晚于
    下个场景标记落盘，日志分段就错位一条——看起来像"整段序列移位"的假 FAIL。
    """
    t0 = time.time()
    n = 0
    while time.time() - t0 < timeout:
        n = msgs_since_marker(scen)
        if n >= expect:
            return n, time.time() - t0
        time.sleep(0.02)
    return n, time.time() - t0


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))

    sent_f = open(SENT, "w", encoding="utf-8")
    recv_f = open(RECVLOG, "a", encoding="utf-8")
    pkts = 0
    contacts = 0
    t0 = time.time()

    def mark(f, line):
        f.write(line + "\n")
        f.flush()

    for name, packets in SCENARIOS:
        mark(recv_f, "SCENARIO " + name)
        mark(sent_f, "SCENARIO " + name)
        for k, pts in enumerate(packets, 1):
            pkt = bytes([ord("T"), MAGIC, len(pts)])
            for pid, px, py in pts:
                nx, ny = nxy(px, py)
                pkt += struct.pack("<HHH", pid & 0xFFFF, nx, ny)
                mark(sent_f, "SEND %s pkt=%d id=%d nx=%d ny=%d" % (name, k, pid, nx, ny))
            assert len(pkt) == 3 + 6 * len(pts), "包长算错"
            sock.sendto(pkt, ("127.0.0.1", PORT))
            pkts += 1
            contacts += len(pts)
            print("SEND %s pkt=%d n=%d len=%d" % (name, k, len(pts), len(pkt)))
            time.sleep(GAP)
        got, dt = wait_scenario(name, len(packets))
        print("SYNC %s msgs=%d/%d %.2fs" % (name, got, len(packets), dt))
        time.sleep(SGAP)          # 握完再补一点余量，然后才写下个场景的标记

    mark(recv_f, "TXSIM_END pkts=%d contacts=%d" % (pkts, contacts))
    mark(sent_f, "TXSIM_END pkts=%d contacts=%d" % (pkts, contacts))
    sent_f.close()
    recv_f.close()
    print("TXSIM_END pkts=%d contacts=%d elapsed=%.2fs -> udp 127.0.0.1:%d"
          % (pkts, contacts, time.time() - t0, PORT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
