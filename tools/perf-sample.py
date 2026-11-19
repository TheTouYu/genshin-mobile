#!/usr/bin/env python3
"""触摸/操作期间的性能采样器（只读）：游戏进程 + 每个线程 CPU、Sunshine 进程 CPU、触摸设备事件率。
用法: python3 tools/perf-sample.py <秒数> [csv路径]
输出每 0.5s 一行: t,pid_total_pct,park_thread_pct,touch_ev_per_s,mouse_ev_per_s,sunshine_pct
"""
import os, sys, time, select, struct, glob, subprocess

def read_cpu(pid):
    try:
        with open(f"/proc/{pid}/stat") as f: p = f.read().split()
        return int(p[13]) + int(p[14])   # utime+stime (ticks)
    except Exception: return None

def threads(pid):
    d = {}
    for t in glob.glob(f"/proc/{pid}/task/*"):
        tid = t.split('/')[-1]
        try:
            with open(f"{t}/stat") as f: p = f.read().split()
            d[tid] = int(p[13]) + int(p[14])
        except Exception: pass
    return d

def gpu():
    try:
        o = subprocess.check_output(["nvidia-smi","--query-gpu=utilization.gpu,utilization.encoder","--format=csv,noheader,nounits"],
                                    timeout=2, text=True).strip().splitlines()[0]
        g, e = [int(x.strip()) for x in o.split(",")]
        return g, e
    except Exception:
        return -1, -1

def find_main_tids(pid):
    ids = set()
    for t in glob.glob(f"/proc/{pid}/task/*"):
        try:
            if open(f"{t}/comm").read().strip() in ("Noelle Main", "dxvk-submit"): ids.add(t.split("/")[-1])
        except Exception: pass
    return ids

def pid_of(name):
    for d in glob.glob("/proc/[0-9]*"):
        try:
            with open(f"{d}/comm") as f:
                if f.read().strip() == name: return int(d.split('/')[-1])
        except Exception: pass
    return None

dur = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/perf-sample.csv"
gpid = pid_of("YuanShen.exe")
spid = pid_of("sunshine")
if not gpid: print("YuanShen.exe 没在跑", file=sys.stderr); sys.exit(1)
MAIN_TIDS = find_main_tids(gpid)
fds = {}
for ev in ("event25", "event26", "event27"):
    try: fds[ev] = os.open(f"/dev/input/{ev}", os.O_RDONLY | os.O_NONBLOCK)
    except Exception: fds[ev] = None
HZ = os.sysconf("SC_CLK_TCK")
pc0 = sum(read_cpu(gpid) or 0 for _ in [0]) if gpid else 0
prev = {"t": time.time(), "g": read_cpu(gpid), "s": read_cpu(spid) if spid else None,
        "th": threads(gpid), "touch": 0, "mouse": 0}
rows = []
end = time.time() + dur
with open(out, "w") as fo:
    fo.write("t,game_pct,sunshine_pct,main_thread_pct,touch_ev_s,mouse_ev_s,gpu_pct,encoder_pct,noelle_main_pct\n")
    while time.time() < end:
        r, _, _ = select.select([f for f in fds.values() if f], [], [], 0.5)
        for f in r:
            try: data = os.read(f, 24 * 256)
            except Exception: data = b""
            n = len(data) // 24
            if f is fds.get("event27"): prev["touch"] += n
            elif f in (fds.get("event25"), fds.get("event26")): prev["mouse"] += n
        now = time.time(); dt = now - prev["t"]
        if dt < 0.45: continue
        g = read_cpu(gpid); s = read_cpu(spid) if spid else None
        th = threads(gpid)
        maincpu = None
        gpct = 100.0 * (g - prev["g"]) / HZ / dt if g is not None and prev["g"] is not None else -1
        spct = 100.0 * (s - prev["s"]) / HZ / dt if s is not None and prev["s"] is not None else -1
        # 主线程近似 = CPU 占用最高的那个线程（Unity 主线程通常居首）
        mx = 0
        for tid, t2 in th.items():
            t1 = prev["th"].get(tid)
            if t1 is not None:
                v = 100.0 * (t2 - t1) / HZ / dt
                mx = max(mx, v)
                if tid in MAIN_TIDS: maincpu = max(maincpu or 0, v)
        gpu_pct, enc_pct = gpu()
        row = "%d,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%.1f" % (int(now), gpct, spct, mx,
                prev["touch"] / dt, prev["mouse"] / dt, gpu_pct, enc_pct, maincpu if maincpu is not None else -1)
        fo.write(row + "\n"); fo.flush(); rows.append(row)
        prev.update({"t": now, "g": g, "s": s, "th": th, "touch": 0, "mouse": 0})
print("\n".join(rows[-3:]))
print("样本 %d 行 → %s" % (len(rows), out))
