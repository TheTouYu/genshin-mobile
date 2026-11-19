#!/bin/bash
# verify-offline.sh —— 离线触摸验收台（不启动原神、不占真机、可无人值守重跑）
#
#   bash verify-offline.sh                     # 正式：从 build/mobui27.dll 造一份 test 模式拷贝再验
#   bash verify-offline.sh build/foo.dll       # 换源 DLL
#   bash verify-offline.sh --selftest          # 冒烟：不注入，接收端自投假 WM_TOUCH（验窗口/消息泵/日志）
#   bash verify-offline.sh --replay <recv.log> <txsim.sent>   # 只跑断言（重放历史日志，验证断言会 FAIL）
#
# 链路：起 build/touchrecv.exe → 注入 test 模式探针拷贝 → tools/txsim.py 发协议 v2 合成包
#       → 解析 build/touchrecv.log + 探针日志断言 → PASS/FAIL 表
#
# 为什么用独立端口：生产态 mobui-touchd 会往 51721 发 250ms cnt=0 心跳，插进场景里把接触点提前抬起
# （症状：MOVE 变成 UP+DOWN）。默认 TXSIM_PORT=51799，只在本验收台里用。
# 为什么用 DLL 拷贝：探针日志/cfg 路径跟随 DLL 名，拷贝成 build/mobui27v.dll 就不碰 Lead 的
#   probe/mobui.cpp、build/mobui27.*，也保证每次都从最新 DLL 起。
#
# 退出码：0=全 PASS｜1=有 FAIL｜2=前置/环境不就绪（DLL 缺失、窗口起不来、端口被占、已有实例在跑）
set -u

cd "$(dirname "$0")" || exit 2

export WINEPREFIX=/home/h/.local/share/anime-game-launcher/prefix
export W=/home/h/.local/share/anime-game-launcher/runners/spritz-wine-tkg-staging-wow64-11.14-1/bin/wine
export XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1 WAYLAND_DISPLAY=wayland-1
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export WINEDEBUG=-all
# XWayland 的 cookie 文件名每次登录会变（xauth_XXXXXX）；未显式给 XAUTHORITY 时自动认领，
# 否则 wine 建窗会报 "Authorization required, but no authorization protocol specified"
if [ -z "${XAUTHORITY:-}" ]; then
    for f in "$XDG_RUNTIME_DIR"/xauth_* "$HOME/.Xauthority"; do
        [ -f "$f" ] && { export XAUTHORITY=$f; break; }
    done
fi

PORT=${TXSIM_PORT:-51799}
LOG=build/touchrecv.log
SENT=build/txsim.sent
STOP=build/touchrecv.stop
WINE_LOG=build/touchrecv.wine.log
INJ_OUT=build/inject.out
TX_OUT=build/txsim.out
SUMMARY=build/verify-offline.summary
# 验收拷贝名按源 DLL 派生：build/vf_mobui28.dll + .cfg（日志跟着拷贝名走）
TEST_DLL=""
TEST_CFG=""
PROBE_BASE=""
PROBE_LOG="/dev/null"
TOUCH_LOG="/dev/null"
PROBE_SEG=build/vf-probe-seg.txt
TOUCH_SEG=build/vf-touch-seg.txt

export TOUCHRECV_LOG=$LOG TOUCHRECV_STOP=$STOP TXSIM_SENT=$SENT TXSIM_RECVLOG=$LOG TXSIM_PORT=$PORT

MODE=full
SRC_DLL=build/mobui27.dll
if [ "${1:-}" = "--selftest" ]; then MODE=selftest
elif [ "${1:-}" = "--equiv" ]; then
    MODE=equiv; EQ_A=${2:?-缺 DLL A}; EQ_B=${3:?-缺 DLL B}
elif [ "${1:-}" = "--equiv-dumps" ]; then
    MODE=equivdumps; EQ_DUMP_A=${2:?-缺 dump A}; EQ_DUMP_B=${3:?-缺 dump B}
elif [ "${1:-}" = "--replay" ]; then
    MODE=replay; REPLAY_LOG=${2:?-缺 recv.log}; REPLAY_SENT=${3:?-缺 txsim.sent}
    REPLAY_PROBE=${4:-/dev/null}; REPLAY_OFF=${5:-0}; REPLAY_TOUCH=${6:-/dev/null}
elif [ -n "${1:-}" ]; then SRC_DLL=$1
fi

if [ "$MODE" = full ] || [ "$MODE" = equiv ]; then
    SRC_BASE=$(basename "${SRC_DLL%.dll}")
    TEST_DLL="build/vf_${SRC_BASE}.dll"
    TEST_CFG="build/vf_${SRC_BASE}.cfg"
    PROBE_BASE=${TEST_DLL%.dll}
    PROBE_LOG=$PROBE_BASE.log              # 探针把扩展名替换成 .log
    TOUCH_LOG=$PROBE_BASE.touch.log        # 触摸流日志
fi

RCV_PID=0
cleanup() {
    if [ "$RCV_PID" -gt 1 ] 2>/dev/null; then
        touch "$STOP" 2>/dev/null
        for _ in $(seq 1 30); do kill -0 "$RCV_PID" 2>/dev/null || break; sleep 0.1; done
        if kill -0 "$RCV_PID" 2>/dev/null; then
            cl=$(tr '\0' ' ' < "/proc/$RCV_PID/cmdline" 2>/dev/null)
            case "$cl" in *touchrecv.exe*) kill "$RCV_PID" 2>/dev/null ;; esac
        fi
    fi
    :
}
trap cleanup EXIT

wait_for() {  # wait_for <秒> <命令...>
    local n=$(( $1 * 10 )); shift
    for _ in $(seq 1 "$n"); do "$@" && return 0; sleep 0.1; done
    return 1
}
winpath() { printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\\\')"; }
cnt() { if [ -s "$1" ]; then grep -c "$2" "$1"; else echo 0; fi; }

# 等价性对照：两份 MSG dump 去掉时钟字段（t/wt/mt，跨轮必然不同）与 mask 后应逐字符相等；
# 另外单独核对 dwMask 的预期差异（CONTACTAREA=0x4，+TIMEFROMSYSTEM=0x5）。
eq_compare() {
    local a=$1 b=$2
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        echo "[equiv] FAIL: 缺 dump（$a / $b）"; return 2
    fi
    local ma mb na nb same=0
    ma=$(grep -o ' mask=[0-9a-fA-F]*' "$a" | head -1 | cut -d= -f2)
    mb=$(grep -o ' mask=[0-9a-fA-F]*' "$b" | head -1 | cut -d= -f2)
    na=$(wc -l < "$a"); nb=$(wc -l < "$b")
    sed -E 's/( t| wt| mt)=[0-9]+//g' "$a" | sed -E 's/ mask=[0-9a-fA-F]+//g' > build/vf-equiv-a.norm
    sed -E 's/( t| wt| mt)=[0-9]+//g' "$b" | sed -E 's/ mask=[0-9a-fA-F]+//g' > build/vf-equiv-b.norm
    diff -q build/vf-equiv-a.norm build/vf-equiv-b.norm >/dev/null 2>&1 && same=1
    # 逐项一行，避开 CJK 在 bash printf 里按字节计数导致的列错位
    echo ""
    echo "等价性对照（时钟字段 t/wt/mt 跨轮必然不同，已归一化剔除）:"
    echo "  E1 MSG 条数相等                    -> $([ "$na" = "$nb" ] && echo PASS || echo FAIL)  (A=$na B=$nb)"
    echo "  E2 除时钟与 mask 外逐字符相等      -> $([ "$same" = 1 ] && echo PASS || echo FAIL)  (diff $([ "$same" = 1 ] && echo 空 || echo 非空))"
    echo "  E3 dwMask 差异符合预期(A=4/B=5)    -> $([ "$ma" = "4" ] && [ "$mb" = "5" ] && echo PASS || echo FAIL)  (A=${ma:-?} B=${mb:-?})"
    if [ "$same" != 1 ]; then
        echo "--- 归一化后差异（前 20 行）---"
        diff -u build/vf-equiv-a.norm build/vf-equiv-b.norm | head -20 | sed 's/^/    /'
        # 诊断（不改判定）：差异是"每点同一个固定位移"还是"随机不同"
        python3 - build/vf-equiv-a.norm build/vf-equiv-b.norm <<'EQPY' || true
import re, sys
def coords(p):
    out = []
    for ln in open(p, encoding="utf-8"):
        out.append([(int(a), int(b)) for a, b in re.findall(r"x=(-?\d+) y=(-?\d+)", ln)])
    return out
A, B = coords(sys.argv[1]), coords(sys.argv[2])
deltas = set()
for la, lb in zip(A, B):
    if len(la) != len(lb):
        print("  诊断: 行内接触点数不同（%d vs %d）→ 不是坐标系问题" % (len(la), len(lb)))
        sys.exit()
    for (xa, ya), (xb, yb) in zip(la, lb):
        deltas.add((xb - xa, yb - ya))
if len(deltas) == 1:
    dx, dy = next(iter(deltas))
    print("  诊断: 所有接触点位移恒为 (dx=%d, dy=%d) → 固定偏移（坐标系语义差），不是随机差异。" % (dx, dy))
    print("        若 |dx|,|dy| 与接收端窗口原点一致，说明 B 侧把客户端坐标转成了屏幕坐标（F2 生效）。")
    print("        真机窗口在 (0,0) 时该偏移应为 0；本次 FAIL 按规则不放宽，需人判。")
else:
    print("  诊断: 位移不恒定（样本 %s…）→ 不是坐标系问题" % sorted(deltas)[:3])
EQPY
    fi
    [ "$na" = "$nb" ] && [ "$same" = 1 ] && [ "$ma" = "4" ] && [ "$mb" = "5" ] && return 0
    return 1
}

# ---------- 断言器（内嵌 python）：读日志 → PASS/FAIL 表 ----------
run_checker() {  # run_checker <recvlog> <sent> <probelog> <probeoff> <gate_win> <gate_inj> <gate_net> <port> <replay>
python3 - "$@" <<'PY'
import os, re, sys, unicodedata

recv_path, sent_path, probe_path = sys.argv[1], sys.argv[2], sys.argv[3]
probe_off = int(sys.argv[4])
g_win, g_inj, g_net, port = sys.argv[5], sys.argv[6], sys.argv[7], sys.argv[8]
replay = len(sys.argv) > 9 and sys.argv[9] == "1"

E = os.environ
post_n = int(E.get("VF_POST", "-1"))
gti_n = int(E.get("VF_GTI", "-1"))
reject_n = int(E.get("VF_REJECT", "-1"))
failpost_n = int(E.get("VF_POSTFAIL", "-1"))
hookfail_n = int(E.get("VF_HOOKFAIL", "-1"))
singleton_n = int(E.get("VF_SINGLETON", "-1"))
netport_ok = E.get("VF_NETPORT_OK", "-1")
summary_path = E.get("VF_SUMMARY", "")

def load(p):
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []

recv = load(recv_path)
sent = load(sent_path)
probe_all = load(probe_path)
probe = probe_all[probe_off:] if probe_path and not probe_path.startswith("/dev/") else probe_all

rows = []
def add(cid, label, kind, ok, detail, ev=None):
    rows.append(dict(cid=cid, label=label, kind=kind, ok=ok, detail=detail, ev=ev or []))

RE_WIN  = re.compile(r"^WINDOW hwnd=\S+ title=(\S+) client=(\d+)x(\d+)(?: origin=(-?\d+),(-?\d+))? style=([0-9a-fA-F]+) borderless=(\d) visible=(\d)")
RE_RTW  = re.compile(r"^RTW -> (\d)")
RE_RSZ  = re.compile(r"^RESIZE client=(\d+)x(\d+)")
RE_WMSG = re.compile(r"^WMSG #(\d+) msg=([0-9a-fA-F]{4})")
RE_MSG  = re.compile(r"^MSG n=(\d+)(.*?)\s*$")
RE_CT   = re.compile(r"\[id=(\d+) f=([0-9a-fA-F]+) x=(-?\d+) y=(-?\d+) t=(\d+)\]")
RE_GTIF = re.compile(r"^GTI_FAIL")
RE_SUM  = re.compile(r"^SUMMARY touches=(\d+) gti_fail=(\d+) msgs=(\d+)")

win = None; rtw = None; resizes = []; wmsgs = []; events = []; gti_fail_lines = 0; summary = None
scen = None
for ln in recv:
    if ln.startswith("SCENARIO "):
        scen = ln.split()[1]; continue
    m = RE_WIN.match(ln)
    if m:
        win = dict(title=m.group(1), w=int(m.group(2)), h=int(m.group(3)),
                   origin=(int(m.group(4)), int(m.group(5))) if m.group(4) is not None else None,
                   style=m.group(6), borderless=m.group(7), visible=m.group(8)); continue
    m = RE_RTW.match(ln)
    if m: rtw = int(m.group(1)); continue
    m = RE_RSZ.match(ln)
    if m: resizes.append((int(m.group(1)), int(m.group(2)))); continue
    m = RE_WMSG.match(ln)
    if m: wmsgs.append(m.group(2).lower()); continue
    if RE_GTIF.match(ln): gti_fail_lines += 1; continue
    m = RE_SUM.match(ln)
    if m: summary = dict(touches=int(m.group(1)), gti_fail=int(m.group(2)), msgs=int(m.group(3))); continue
    m = RE_MSG.match(ln)
    if m:
        n = int(m.group(1))
        ct = [(int(a), int(b, 16), int(c), int(d), int(e)) for a, b, c, d, e in RE_CT.findall(m.group(2))]
        events.append(dict(scen=scen, n=n, ct=ct, line=ln)); continue

RE_SEND = re.compile(r"^SEND (\S+) pkt=(\d+) id=(\d+) nx=(\d+) ny=(\d+)")
send = {}
for ln in sent:
    if ln.startswith("SCENARIO "):
        send.setdefault(ln.split()[1], {}); continue
    m = RE_SEND.match(ln)
    if m:
        s, pkt, pid, nx, ny = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
        send.setdefault(s, {}).setdefault(pid, []).append((pkt, nx, ny))

SC = ["S1", "S2", "S3", "S4"]
DOWN, MOVE, UP, PRIM = 0x2, 0x1, 0x4, 0x10   # winuser.h: MOVE=0x1 DOWN=0x2 UP=0x4 PRIMARY=0x10
ACT = ((DOWN, "D"), (MOVE, "M"), (UP, "U"))
EXPECT = {
    "S1": [["1020:D"], ["1020:M"], ["1020:M"], ["1020:U"]],
    "S2": [["1020:D", "1021:D"], ["1020:M", "1021:M"], ["1020:M", "1021:M"],
           ["1020:M", "1021:M"], ["1020:M", "1021:M"], ["1020:U", "1021:U"]],
    "S3": [["1030:D", "1031:D"], ["1030:U", "1031:U"]],
    "S4": [["1020:D"], ["1020:U"], ["1020:D"], ["1020:U"]],
}

def act(f):
    for bit, name in ACT:
        if f & bit: return name
    return "?"

ok = bool(g_win == "1" and win)
detail = "未建窗" if not win else \
    "client=%dx%d borderless=%s visible=%s style=%s" % (win["w"], win["h"], win["borderless"], win["visible"], win["style"])
add("01", "窗口 2560x1440 无边框 title=touchrecv", "req",
    ok and win["title"] == "touchrecv" and win["w"] == 2560 and win["h"] == 1440
       and win["borderless"] == "1" and win["visible"] == "1", detail)

add("02", "RegisterTouchWindow(hwnd,0) 返回 TRUE", "req", rtw == 1, "RTW -> %s" % rtw)

if win and win["origin"] is not None:
    add("03b", "窗口原点 (0,0)（27/28 坐标等价的先决条件）", "req",
        win["origin"] == (0, 0), "origin=(%d,%d)" % win["origin"])
else:
    add("03b", "窗口原点 (0,0)（27/28 坐标等价的先决条件）", "opt", False,
        "日志无 origin 字段（旧版接收端，无法判断窗口原点）")

bad_resize = [r for r in resizes if r != (win["w"], win["h"])] if win else []
add("03", "客户区全程不变（漂移会让坐标断言失效）", "req", not bad_resize,
    "resize 行=%d 异常=%s" % (len(resizes), bad_resize or "无"))

ready = [l for l in probe if "attached" in l or "TEST MODE" in l]
netline = [l for l in probe if "NET listening" in l]
add("04", "注入成功（探针日志有新内容）", "opt" if replay else "req", g_inj == "1" and bool(ready),
    "新增探针日志行=%d '%s'" % (len(ready), ready[0][:64] if ready else ""))
add("05", "探针 UDP 监听端口 == 验收端口 %s" % port, "opt" if replay else "req",
    g_net == "1" and netport_ok in ("1", "-1"),
    "gate=%s netport_ok=%s %s" % (g_net, netport_ok, netline[0][:64] if netline else ""))

allct = sum(len(e["ct"]) for e in events)
wmsg_touch = sum(1 for m in wmsgs if m == "0240")
add("06", "WM_TOUCH 真到了并解码出接触点", "req", len(events) > 0 and allct > 0,
    "MSG 行=%d 接触点=%d WMSG(0240)=%d" % (len(events), allct, wmsg_touch))

gti_bad = summary["gti_fail"] if summary else gti_fail_lines
add("07", "GetTouchInputInfo 解码零失败（GTI_FAIL=0）", "req", gti_bad == 0, "gti_fail=%d" % gti_bad,
    [l for l in recv if l.startswith("GTI_FAIL")][:5])

have_probe = bool(probe_path) and not probe_path.startswith("/dev/")
hyg = []
if have_probe:
    if reject_n > 0: hyg.append("proto reject=%d" % reject_n)
    if failpost_n > 0: hyg.append("post FAILED=%d" % failpost_n)
    if hookfail_n > 0: hyg.append("hook FAILED=%d" % hookfail_n)
add("08", "探针日志无 proto reject / post FAILED / hook FAILED", "req" if have_probe else "opt", not hyg,
    "reject=%s postFAILED=%s hookFAILED=%s %s" % (reject_n, failpost_n, hookfail_n, " ".join(hyg)),
    [l for l in probe if ("proto reject" in l or "FAILED" in l)][:5])

add("08b", "探针未报 SINGLETON（单例互斥未抢占本轮注入）", "req" if have_probe else "opt",
    singleton_n == 0,
    "SINGLETON 行=%s" % singleton_n,
    [l for l in probe if "SINGLETON" in l][:3])

bit_bad = []
for e in events:
    if e["n"] != len(e["ct"]):
        bit_bad.append("n 与接触点数不符: " + e["line"])
    for cid, f, x, y, t in e["ct"]:
        if sum(1 for b, _ in ACT if f & b) != 1:
            bit_bad.append("id=%d f=%x 动作位不唯一" % (cid, f))
add("09", "每条接触点恰有一个 DOWN/MOVE/UP 位", "req", not bit_bad,
    "异常=%d" % len(bit_bad), bit_bad[:5])

st_problems = []
for s in SC:
    held = {}
    for e in [x for x in events if x["scen"] == s]:
        for cid, f, x, y, t in e["ct"]:
            if f & DOWN:
                if cid in held: st_problems.append("%s id=%d 按住期间又来 DOWN" % (s, cid))
                held[cid] = 1
            elif f & MOVE:
                if cid not in held: st_problems.append("%s id=%d MOVE 在 DOWN 之前/UP 之后" % (s, cid))
            elif f & UP:
                if cid not in held: st_problems.append("%s id=%d UP 没有对应 DOWN" % (s, cid))
                else: del held[cid]
    for cid in held: st_problems.append("%s id=%d 场景结束仍在按住（无 UP）" % (s, cid))
add("10", "DOWN/UP 成对·MOVE 只在按住期间·无重复 DOWN", "req", not st_problems,
    "问题=%d" % len(st_problems), st_problems[:6])

seq_problems = []
for s in SC:
    got = []
    for e in [x for x in events if x["scen"] == s]:
        got.append(sorted("%d:%s" % (c[0], act(c[1])) for c in e["ct"]))
    want = [sorted(x) for x in EXPECT[s]]
    if got != want:
        for i in range(max(len(got), len(want))):
            g = got[i] if i < len(got) else "-"
            w = want[i] if i < len(want) else "-"
            if g != w:
                seq_problems.append("%s 第%d条: 实际=%s 期望=%s" % (s, i + 1, g, w))
add("11", "逐场景消息动作序列与脚本严格一致", "req", not seq_problems,
    "总消息=%d 期望=%d 问题=%d" % (len(events), sum(len(v) for v in EXPECT.values()), len(seq_problems)),
    seq_problems[:6])

ledger = sum(len(v) for d in send.values() for v in d.values())
map_problems = [] if ledger else ["发送账本为空（build/txsim.sent 缺失/无 SEND 行）：dwID 无法核对"]
formula = set()
for s in SC:
    want = send.get(s, {})
    exp_ff = {1000 + (i & 0xFF): i for i in want}
    exp_3ff = {1000 + (i & 0x3FF): i for i in want}
    seen = {c[0] for e in events if e["scen"] == s for c in e["ct"]}
    for cid in seen:
        mf, m3 = cid in exp_ff, cid in exp_3ff
        if not (mf or m3): map_problems.append("%s dwID=%d 不在期望集 %s" % (s, cid, sorted(exp_ff)))
        formula.add("ff" if mf else "3ff")
    for cid in exp_ff:
        if cid not in seen: map_problems.append("%s 少了 id=%d 的接触点" % (s, cid))
add("12", "dwID = 1000+(sim id&0xFF) 与发包 id 对应", "req", not map_problems,
    "公式命中=%s 问题=%d" % ("/".join(sorted(formula)) or "-", len(map_problems)), map_problems[:5])
if formula == {"3ff"}:
    add("12b", "dwID 实际用 &0x3FF（task 文本写 &0xFF，需对齐文档）", "opt", False,
        "所有 id 只匹配 1000+(id&0x3FF)")

w, h = (win["w"], win["h"]) if win else (2560, 1440)
coord_problems = []; checked = 0
for s in SC:
    cur = {i: 0 for i in send.get(s, {})}
    for e in [x for x in events if x["scen"] == s]:
        for cid, f, x, y, t in e["ct"]:
            if f & UP: continue
            sims = [i for i in send.get(s, {}) if 1000 + (i & 0x3FF) == cid]
            if not sims: continue
            sim = sims[0]; arr = send[s][sim]; idx = cur.get(sim, 0)
            hit = None
            for k in range(idx, len(arr)):
                pkt, nx, ny = arr[k]
                ex, ey = nx * w // 65535, ny * h // 65535
                if abs(x - ex) < 3 and abs(y - ey) < 3:
                    hit = (k, ex, ey); break
            if hit is None:
                pkt, nx, ny = arr[min(idx, len(arr) - 1)]
                coord_problems.append("%s id=%d f=%x 实际=(%d,%d) 期望≈(%d,%d) nx/ny=%d/%d"
                                      % (s, cid, f, x, y, nx * w // 65535, ny * h // 65535, nx, ny))
            else:
                cur[sim] = hit[0]; checked += 1
if ledger and checked == 0:
    coord_problems.append("一次坐标比对都没发生（账本有 %d 条，事件里却没有可比对的 DOWN/MOVE）" % ledger)
add("13", "坐标与归一化值误差 <3px（按发送顺序）", "req", bool(ledger) and not coord_problems,
    "账本=%d 比对=%d 问题=%d" % (ledger, checked, len(coord_problems)), coord_problems[:5])

n2 = {s: [e for e in events if e["scen"] == s and e["n"] == 2] for s in SC}
add("14", "S2/S3 出现 n=2 消息且条数对", "req",
    len(n2["S2"]) == 6 and len(n2["S3"]) == 2,
    "S2 n=2=%d/6 S3 n=2=%d/2" % (len(n2["S2"]), len(n2["S3"])),
    [e["line"] for e in (n2["S2"] + n2["S3"])][:4])
add("15", "S3 一条消息里两条 UP（同起同落）", "req",
    any(len([c for c in e["ct"] if c[1] & UP]) == 2 for e in n2["S3"]),
    "S3 全 UP 消息=%d" % sum(1 for e in n2["S3"] if len([c for c in e["ct"] if c[1] & UP]) == 2))

prim_bad = []
for e in events:
    if e["n"] < 2: continue
    if not (e["ct"][0][1] & PRIM): prim_bad.append("首点无 PRIMARY: " + e["line"])
    if any(c[1] & PRIM for c in e["ct"][1:]): prim_bad.append("非首点带 PRIMARY: " + e["line"])
    if sum(1 for c in e["ct"] if c[1] & PRIM) != 1: prim_bad.append("PRIMARY 个数!=1: " + e["line"])
add("16", "多指消息只有首点带 PRIMARY（每条恰一个）", "req", not prim_bad,
    "n>=2 消息=%d 异常=%d" % (sum(1 for e in events if e["n"] >= 2), len(prim_bad)), prim_bad[:5])
single_no_prim = sum(1 for e in events if e["n"] == 1 and not any(c[1] & PRIM for c in e["ct"]))
add("16b", "单指消息也带 PRIMARY", "opt", single_no_prim == 0, "单指缺 PRIMARY=%d" % single_no_prim)

s4 = [(c[0], c[1]) for e in events if e["scen"] == "S4" for c in e["ct"] if c[0] == 1020]
downs = sum(1 for cid, f in s4 if f & DOWN)
seq_ok = len(s4) == 4 and bool(s4[0][1] & DOWN) and bool(s4[1][1] & UP) and bool(s4[2][1] & DOWN) and bool(s4[3][1] & UP)
add("17", "S4 同 id 复用重新出现新 DOWN（不并成 MOVE）", "req", downs == 2 and seq_ok,
    "S4 id=1020 事件=%d DOWN=%d 顺序=%s" % (len(s4), downs, seq_ok),
    [e["line"] for e in events if e["scen"] == "S4"])

if post_n >= 0 and gti_n >= 0:
    ok = (post_n == gti_n == len(events))
    add("18", "跨层一致：探针 POST == GTI == 接收端 MSG 条数", "req", ok,
        "POST=%d GTI=%d MSG=%d" % (post_n, gti_n, len(events)))
else:
    add("18", "跨层一致：探针 POST == GTI == 接收端 MSG 条数", "opt", False,
        "未取到探针触摸日志（POST/GTI 计数不可用）")

ts = [e["ct"][0][4] for e in events if e["ct"]]
add("19", "消息时间戳单调不减（顺序可信）", "req", bool(ts) and all(b >= a for a, b in zip(ts, ts[1:])),
    "t 首/末=%s/%s" % (ts[0] if ts else "-", ts[-1] if ts else "-"))

def dw(s):
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in s)
def pad(s, n):
    return s + " " * max(0, n - dw(s))

fails = [r for r in rows if r["kind"] == "req" and not r["ok"]]
print("")
print(pad("ID", 5) + pad("检查项", 52) + pad("结果", 7) + "详情")
print("-" * 122)
for r in rows:
    res = "PASS" if r["ok"] else ("FAIL" if r["kind"] == "req" else "INFO")
    print(pad(r["cid"], 5) + pad(r["label"], 52) + pad(res, 7) + r["detail"])
print("-" * 122)
print("汇总: %d 项必需检查，FAIL=%d，INFO=%d" %
      (sum(1 for r in rows if r["kind"] == "req"), len(fails),
       sum(1 for r in rows if r["kind"] == "opt")))

if fails:
    print("\n=== FAIL 证据（相关日志原文）===")
    for r in fails:
        print("[%s] %s -- %s" % (r["cid"], r["label"], r["detail"]))
        for line in r["ev"]:
            print("    " + line)
    if events:
        print("\n=== 收到的消息原文（前 8 条 / 共 %d 条）===" % len(events))
        for e in events[:8]:
            print("    " + e["line"])
    print("\n=== 发送记录（前 6 条）===")
    for ln in [l for l in sent if l.startswith("SEND")][:6]:
        print("    " + ln)

if summary_path:
    try:
        with open(summary_path, "w", encoding="utf-8") as f:
            f.write("RESULT=%s\n" % ("PASS" if not fails else "FAIL"))
            f.write("REQ=%d\nFAIL=%d\nINFO=%d\n" %
                    (sum(1 for r in rows if r["kind"] == "req"), len(fails),
                     sum(1 for r in rows if r["kind"] == "opt")))
            for r in rows:
                f.write("CHECK %s %s %s | %s\n" % (r["cid"], "PASS" if r["ok"] else "FAIL",
                                                  r["label"], r["detail"]))
    except OSError:
        pass

sys.exit(0 if not fails else 1)
PY
}

if [ "$MODE" = equivdumps ]; then
    echo "[equiv] 只比较两份已有 dump（不建窗、不注入）"
    eq_compare "$EQ_DUMP_A" "$EQ_DUMP_B"; exit $?
fi

if [ "$MODE" = equiv ]; then
    ta=$(basename "${EQ_A%.dll}"); tb=$(basename "${EQ_B%.dll}")
    echo "[equiv] 第一轮: $EQ_A（隐藏窗口不适用，本模式必须真实建窗）"
    if ! VF_TAG="$ta" bash "$0" "$EQ_A" > "build/vf-equiv-$ta.out" 2>&1; then
        echo "[equiv] FAIL: $EQ_A 那一轮没全 PASS，明细:"; tail -12 "build/vf-equiv-$ta.out" | sed 's/^/    /'; exit 1
    fi
    echo "[equiv] 第二轮: $EQ_B"
    if ! VF_TAG="$tb" bash "$0" "$EQ_B" > "build/vf-equiv-$tb.out" 2>&1; then
        echo "[equiv] FAIL: $EQ_B 那一轮没全 PASS，明细:"; tail -12 "build/vf-equiv-$tb.out" | sed 's/^/    /'; exit 1
    fi
    eq_compare "build/vf-msgs-$ta.txt" "build/vf-msgs-$tb.txt"; exit $?
fi

if [ "$MODE" = replay ]; then
    echo "[replay] 断言重放: $REPLAY_LOG + $REPLAY_SENT"
    echo "         探针日志: $REPLAY_PROBE (off=$REPLAY_OFF)  触摸流: $REPLAY_TOUCH"
    if [ "$REPLAY_TOUCH" != /dev/null ] && [ -f "$REPLAY_TOUCH" ]; then
        VF_POST=$(cnt "$REPLAY_TOUCH" 'POST n=')
        VF_GTI=$(cnt "$REPLAY_TOUCH" 'GTI count=')
    else
        VF_POST=-1; VF_GTI=-1
    fi
    if [ "$REPLAY_PROBE" != /dev/null ] && [ -f "$REPLAY_PROBE" ]; then
        tail -n +$((REPLAY_OFF+1)) "$REPLAY_PROBE" > build/vf-probe-seg.txt
        VF_REJECT=$(cnt build/vf-probe-seg.txt 'proto reject')
        VF_POSTFAIL=$(cnt build/vf-probe-seg.txt 'TOUCH post\[[^]]*\] FAILED')
        VF_HOOKFAIL=$(cnt build/vf-probe-seg.txt 'BRIDGE hook FAILED')
        VF_SINGLETON=$(cnt build/vf-probe-seg.txt 'SINGLETON')
        if grep -q "NET listening on 127.0.0.1" build/vf-probe-seg.txt; then VF_NETPORT_OK=1; else VF_NETPORT_OK=-1; fi
        export VF_SUMMARY=$SUMMARY
    else
        VF_REJECT=-1; VF_POSTFAIL=-1; VF_HOOKFAIL=-1; VF_SINGLETON=-1; VF_NETPORT_OK=-1
    fi
    export VF_POST VF_GTI VF_REJECT VF_POSTFAIL VF_HOOKFAIL VF_SINGLETON VF_NETPORT_OK
    run_checker "$REPLAY_LOG" "$REPLAY_SENT" "$REPLAY_PROBE" "$REPLAY_OFF" 1 1 1 "$PORT" 1
    exit $?
fi

command -v python3 >/dev/null || { echo "[preflight] 缺 python3"; exit 2; }
[ -x "$W" ] || { echo "[preflight] wine runner 不存在: $W"; exit 2; }

if [ "$MODE" = full ] && pgrep -x YuanShen.exe >/dev/null 2>&1; then
    echo "[preflight] 原神（YuanShen.exe）在跑 —— 探针单例 mobui_probe_singleton 是整个 WINEPREFIX 共享的："
    echo "            往 touchrecv.exe 注入验收拷贝会命中 ERROR_ALREADY_EXISTS → g_hook_mask=0 → 全表假 FAIL。"
    echo "            先关游戏，或用独立 WINEPREFIX 跑本验收台。当前实例:"
    pgrep -a -x YuanShen.exe 2>/dev/null | sed 's/^/    /'
    exit 2
fi

others=$(pgrep -x touchrecv.exe 2>/dev/null | tr '\n' ' ')
if [ -n "$others" ]; then
    echo "[preflight] 已有 touchrecv.exe 在跑（pid: $others）——探针单例互斥会让本次注入失效，停。"
    for p in $others; do echo "    pid=$p $(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | cut -c1-100)"; done
    exit 2
fi

if command -v ss >/dev/null 2>&1; then
    busy=$(ss -lunp 2>/dev/null | grep -F ":$PORT " || true)
    if [ -n "$busy" ]; then
        echo "[preflight] UDP $PORT 已被占用:"; echo "$busy" | sed 's/^/    /'
        pids=$(echo "$busy" | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u)
        foreign=0
        for p in $pids; do
            cl=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
            env=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -c "^TOUCHRECV_STOP=$STOP")
            case "$cl" in
                *genshin-mobile/build/touchrecv.exe*)
                    if [ "$env" -ge 1 ]; then
                        echo "[preflight] 清理本验收台残留 pid=$p ($cl)"
                        kill "$p" 2>/dev/null
                    else
                        foreign=1; echo "[preflight] 占用者是别的 touchrecv，不动它: pid=$p $cl"
                    fi ;;
                *) foreign=1; echo "[preflight] 占用者不是我方: pid=$p $cl" ;;
            esac
        done
        [ -z "$pids" ] && foreign=1
        if [ "$foreign" = 1 ]; then
            echo "[preflight] 端口 $PORT 被别的进程占着，停止（不动别人的进程）。换端口: TXSIM_PORT=518xx bash verify-offline.sh"
            exit 2
        fi
        sleep 0.6
    fi
fi

rm -f "$LOG" "$STOP" "$INJ_OUT" "$TX_OUT" "$SUMMARY"
[ "$MODE" = full ] && rm -f "$SENT"      # 只有 full 会重写账本；selftest 不动它
: > "$LOG"

if [ "$MODE" = selftest ]; then
    export TOUCHRECV_SELFTEST=1 TOUCHRECV_SECS=30 TOUCHRECV_HIDE=1
    echo "[selftest] 起 build/touchrecv.exe（隐藏窗口 + 自投假 WM_TOUCH，不注入、不占屏）"
    "$W" build/touchrecv.exe >"$WINE_LOG" 2>&1 &
    RCV_PID=$!
    if ! wait_for 8 grep -q '^RTW ' "$LOG"; then
        echo "[selftest] FAIL: 窗口/日志没起来"; tail -5 "$WINE_LOG"; exit 2
    fi
    wait_for 5 grep -q '^SELFTOUCH n=2' "$LOG" || true
    touch "$STOP"
    wait_for 3 bash -c "! kill -0 $RCV_PID 2>/dev/null" || true
    echo "--- build/touchrecv.log ---"; sed -n '1,20p' "$LOG"
    ok1=$(grep -c '^WINDOW .*client=2560x1440 .*borderless=1 .*hidden=1' "$LOG")
    ok2=$(grep -c '^SELFTOUCH n=2 \[id=1000 f=1 .*\] \[id=1001 f=4 ' "$LOG")
    ok3=$(grep -c '^SUMMARY ' "$LOG")
    printf '\n%-5s%-42s%-7s%s\n' "ID" "检查项" "结果" "详情"
    printf '%-5s%-42s%-7s%s\n' "01" "窗口 2560x1440 无边框" "$([ "$ok1" -ge 1 ] && echo PASS || echo FAIL)" "WINDOW 行=$ok1"
    printf '%-5s%-42s%-7s%s\n' "02" "消息泵收到 WM_TOUCH 并解码" "$([ "$ok2" -ge 1 ] && echo PASS || echo FAIL)" "SELFTOUCH n=2 行=$ok2"
    printf '%-5s%-42s%-7s%s\n' "03" "日志收尾 SUMMARY" "$([ "$ok3" -ge 1 ] && echo PASS || echo FAIL)" "SUMMARY 行=$ok3"
    [ "$ok1" -ge 1 ] && [ "$ok2" -ge 1 ] && [ "$ok3" -ge 1 ] && exit 0
    exit 1
fi

[ -f "$SRC_DLL" ] || { echo "[preflight] 源 DLL 不存在: $SRC_DLL（Lead 还没就绪？）"; exit 2; }

cp -f "$SRC_DLL" "$TEST_DLL"
cat > "$TEST_CFG" <<CFG
test=1
hooks=0x80
veh=0
map=0
suppress=0
net=1
net_port=$PORT
pump=0
convert=0
CFG
echo "[preflight] 拷贝 $(basename "$SRC_DLL") -> $TEST_DLL；cfg net_port=$PORT test=1"
PROBE_OFF=0; TOFF=0
[ -f "$PROBE_LOG" ] && PROBE_OFF=$(wc -l < "$PROBE_LOG")
[ -f "$TOUCH_LOG" ] && TOFF=$(wc -l < "$TOUCH_LOG")
DLL_WIN=$(winpath "$(readlink -f "$TEST_DLL")")

echo "[1/4] 起 build/touchrecv.exe"
TOUCHRECV_SECS=60 "$W" build/touchrecv.exe >"$WINE_LOG" 2>&1 &
RCV_PID=$!
G_WINDOW=0
if wait_for 8 grep -q '^RTW ' "$LOG"; then G_WINDOW=1; else
    echo "[1/4] FAIL 窗口没起来（8s）"; tail -5 "$WINE_LOG"; exit 2
fi
grep -E '^WINDOW ' "$LOG" | sed 's/^/    /'

echo "[2/4] 注入探针: $TEST_DLL"
"$W" build/inject.exe --name touchrecv.exe "$DLL_WIN" 2>&1 | tee "$INJ_OUT" | sed 's/^/    /'
G_INJECT=0
grep -q 'wait=0' "$INJ_OUT" && G_INJECT=1
[ "$G_INJECT" = 1 ] || { echo "[2/4] FAIL 注入没成功"; exit 2; }

G_NET=0
if command -v ss >/dev/null 2>&1 && wait_for 6 bash -c "ss -lunp 2>/dev/null | grep -qF ':$PORT '"; then
    G_NET=1
elif [ -f "$PROBE_LOG" ] && wait_for 4 bash -c "tail -n +$((PROBE_OFF+1)) '$PROBE_LOG' 2>/dev/null | grep -q 'NET listening'"; then
    G_NET=1
fi
sleep 0.3
tail -n +$((PROBE_OFF+1)) "$PROBE_LOG" > "$PROBE_SEG" 2>/dev/null || : > "$PROBE_SEG"
tail -n +$((TOFF+1)) "$TOUCH_LOG" > "$TOUCH_SEG" 2>/dev/null || : > "$TOUCH_SEG"
echo "    探针日志新增 $(wc -l < "$PROBE_SEG") 行，触摸流 $(wc -l < "$TOUCH_SEG") 行，UDP 监听=$G_NET"
grep -E 'attached|TEST MODE|NET listening|BRIDGE' "$PROBE_SEG" | head -6 | sed 's/^/    /'

echo "[3/4] txsim 发协议 v2 合成包（端口 $PORT）"
python3 tools/txsim.py | sed 's/^/    /'

echo "[4/4] 等消息落盘并断言"
wait_for 4 grep -q '^TXSIM_END' "$LOG" || true
prev=-1
for _ in $(seq 1 30); do
    cur=$(grep -c '^MSG ' "$LOG" 2>/dev/null || echo 0)
    [ "$cur" = "$prev" ] && [ "$cur" -gt 0 ] && break
    prev=$cur; sleep 0.1
done
touch "$STOP"
wait_for 3 bash -c "! kill -0 $RCV_PID 2>/dev/null" || true
tail -n +$((TOFF+1)) "$TOUCH_LOG" > "$TOUCH_SEG" 2>/dev/null || : > "$TOUCH_SEG"

VF_POST=$(cnt "$TOUCH_SEG" 'POST n=')
VF_GTI=$(cnt "$TOUCH_SEG" 'GTI count=')
VF_REJECT=$(cnt "$PROBE_SEG" 'proto reject')
VF_POSTFAIL=$(cnt "$PROBE_SEG" 'TOUCH post\[[^]]*\] FAILED')
VF_HOOKFAIL=$(cnt "$PROBE_SEG" 'BRIDGE hook FAILED')
VF_SINGLETON=$(cnt "$PROBE_SEG" 'SINGLETON')
if grep -q "NET listening on 127.0.0.1:$PORT" "$PROBE_SEG"; then VF_NETPORT_OK=1; else VF_NETPORT_OK=0; fi
export VF_POST VF_GTI VF_REJECT VF_POSTFAIL VF_HOOKFAIL VF_SINGLETON VF_NETPORT_OK VF_SUMMARY=$SUMMARY

echo ""
echo "=== touchrecv.log 关键行 ==="
grep -E '^(WINDOW|RTW|SCENARIO|TXSIM_END|SUMMARY)' "$LOG" | sed 's/^/    /'
grep '^MSG ' "$LOG" | head -6 | sed 's/^/    /'
grep '^MSG ' "$LOG" > build/vf-msgs-last.txt
if [ -n "${VF_TAG:-}" ]; then cp -f build/vf-msgs-last.txt "build/vf-msgs-$VF_TAG.txt"; fi
echo "=== 探针日志尾部 ==="
tail -n 8 "$PROBE_SEG" | sed 's/^/    /'
echo "=== 探针触摸流（POST/GTI）前 6 行 ==="
grep -E 'POST |GTI ' "$TOUCH_SEG" | head -6 | sed 's/^/    /'

run_checker "$LOG" "$SENT" "$PROBE_LOG" "$PROBE_OFF" "$G_WINDOW" "$G_INJECT" "$G_NET" "$PORT" 0
exit $?
